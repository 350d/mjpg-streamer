#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <time.h>
#include <limits.h>
#include <errno.h>
#include <sys/time.h>
#include <fcntl.h>

#include "../../mjpg_streamer.h"
#include "../../utils.h"
#include "../../jpeg_utils.h"
#ifdef HAVE_TURBOJPEG
#include <turbojpeg.h>
#endif

#define MAX_CLIENTS 10
#define RTP_PAYLOAD_TYPE 26  // JPEG
#define RTP_SSRC 0x12345678
#define MAX_RTP_PACKET_SIZE 1500  // Standard Ethernet MTU
#define MAX_TCP_PACKET_SIZE 8192  // Larger packet size for TCP to reduce fragmentation
#define MAX_FRAME_SIZE (10 * 1024 * 1024)  // 10MB max frame size

/* RFC 2435 zigzag order (as in FFmpeg libavformat/rtpenc_jpeg.c) */
static const uint8_t rfc2435_zigzag[64] = {
    0,  1,  5,  6, 14, 15, 27, 28,
    2,  4,  7, 13, 16, 26, 29, 42,
    3,  8, 12, 17, 25, 30, 41, 43,
    9, 11, 18, 24, 31, 40, 44, 53,
   10, 19, 23, 32, 39, 45, 52, 54,
   20, 22, 33, 38, 46, 51, 55, 60,
   21, 34, 37, 47, 50, 56, 59, 61,
   35, 36, 48, 49, 57, 58, 62, 63
};

typedef struct {
    int socket;
    int active;
    struct sockaddr_in addr;
    int rtp_port;
    int rtcp_port;
    uint16_t sequence_number;
    uint32_t timestamp;
    int playing;
} rtsp_client_t;

static rtsp_client_t clients[MAX_CLIENTS];
static int server_socket = -1;
static int rtp_socket = -1;
static int server_running = 0;
static pthread_t server_thread;
static pthread_t stream_thread;
static int input_number = 0;
static globals *pglobal;
static pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
/* RTP timestamp increment (90kHz clock) - updated per frame from wall clock */
static uint32_t rtp_ts_increment = 3000; /* init ~30fps */
/* Cached frame dimensions for consistent SDP and RTP headers - prevents VLC flickering */
static int cached_sdp_width = 640;
static int cached_sdp_height = 480;
static int sdp_dimensions_cached = 0;

/* Performance optimization: static buffers */
static unsigned char static_frame_buffer[MAX_FRAME_SIZE];
static int use_static_buffers = 1;

/* Snapshot buffer for HTTP /snapshot endpoint */
static unsigned char *snapshot_buffer = NULL;
static size_t snapshot_size = 0;
static pthread_mutex_t snapshot_mutex = PTHREAD_MUTEX_INITIALIZER;


typedef struct {
    unsigned char *rtp_payload;        /* RTP/JPEG payload (JPEG header + optional QT + scan data) or full JPEG */
    size_t rtp_payload_size;           /* Size of RTP payload */
    int width;
    int height;
    int subsamp;                       /* TurboJPEG subsampling (TJSAMP_*) */
    int jpeg_type;                     /* RFC2435 type (0/1/3) */
    int is_rtp_format;                 /* 1 = RTP/JPEG format (no SOI/EOI), 0 = full JPEG format */
    /* Quantization tables for this frame (to avoid race condition with global cache) */
    uint8_t qt_luma[64];
    uint8_t qt_chroma[64];
    int have_luma;
    int have_chroma;
    int qt_precision;                  /* 0 = 8-bit, 1 = 16-bit */
} rtp_jpeg_frame_t;

static void free_rtp_jpeg_frame(rtp_jpeg_frame_t *frame);
static int prepare_rtp_jpeg_frame(const unsigned char *jpeg_data, size_t jpeg_size,
                                  rtp_jpeg_frame_t *frame_info);

static void free_rtp_jpeg_frame(rtp_jpeg_frame_t *frame)
{
    if (!frame) {
        return;
    }
    if (frame->rtp_payload) {
        free(frame->rtp_payload);
        frame->rtp_payload = NULL;
        frame->rtp_payload_size = 0;
    }
}

static int prepare_rtp_jpeg_frame(const unsigned char *jpeg_data, size_t jpeg_size,
                                  rtp_jpeg_frame_t *frame_info)
{
    if (!jpeg_data || jpeg_size == 0 || !frame_info) {
        return -1;
    }

    memset(frame_info, 0, sizeof(*frame_info));
    
    /* DEBUG: Check if input JPEG already contains duplicate EOI or next JPEG frame */
    static int input_check_count = 0;
    if (input_check_count++ < 3) {
        /* Find first EOI in input JPEG */
        size_t first_eoi = 0;
        for (size_t i = 1; i < jpeg_size; ) {
            if (jpeg_data[i-1] == 0xFF && jpeg_data[i] == 0xD9) {
                first_eoi = i + 1; /* Position after first EOI */
                break;
            } else if (jpeg_data[i-1] == 0xFF && jpeg_data[i] == 0x00) {
                i += 2;
                continue;
            } else {
                i++;
            }
        }
        OPRINT("[RTP DEBUG] Input JPEG: size=%zu, first_eoi=%zu\n", jpeg_size, first_eoi);
        if (first_eoi > 0 && first_eoi < jpeg_size) {
            unsigned char next1 = jpeg_data[first_eoi];
            unsigned char next2 = jpeg_data[first_eoi + 1];
            OPRINT("[RTP DEBUG] Input JPEG: bytes after first EOI (offset %zu): 0x%02X 0x%02X\n",
                   first_eoi, next1, next2);
            if (next1 == 0xFF && (next2 == 0xD9 || next2 == 0xD8)) {
                OPRINT("[RTP DEBUG] Input JPEG ALREADY contains duplicate EOI or next JPEG after first EOI!\n");
            }
        } else if (first_eoi == 0) {
            OPRINT("[RTP DEBUG] Input JPEG: EOI not found!\n");
        } else if (first_eoi >= jpeg_size) {
            OPRINT("[RTP DEBUG] Input JPEG: first EOI at end of JPEG (first_eoi=%zu, jpeg_size=%zu)\n", 
                   first_eoi, jpeg_size);
        }
    }

    /* Get dimensions and subsampling from input JPEG */
    int input_width = 0;
    int input_height = 0;
    int input_subsamp = -1;
    if (turbojpeg_header_info(jpeg_data, (int)jpeg_size, &input_width, &input_height, &input_subsamp) != 0) {
        OPRINT("[RTP ERROR] turbojpeg_header_info failed during preparation\n");
        return -1;
    }

    /* Trim JPEG to first EOI to remove any padding - handle escape sequences */
    /* CRITICAL: Must find FIRST EOI (not last) and trim strictly to it (including EOI, excluding anything after) */
    /* Search from SOI (0xFF 0xD8) to find first EOI (0xFF 0xD9) */
    size_t eoi_pos = jpeg_size;
    
    /* First, find SOI to ensure we're searching within a valid JPEG */
    size_t soi_pos = 0;
    if (jpeg_size < 2 || jpeg_data[0] != 0xFF || jpeg_data[1] != 0xD8) {
        OPRINT("[RTP ERROR] JPEG does not start with SOI (0xFF 0xD8)\n");
        return -1;
    }
    soi_pos = 0;
    
    /* Search for first EOI after SOI - handle escape sequences (0xFF 0x00) in scan data */
    for (size_t i = soi_pos + 2; i < jpeg_size; ) {
        if (jpeg_data[i-1] == 0xFF) {
            if (jpeg_data[i] == 0xD9) {
                /* Found FIRST EOI marker - eoi_pos points to position AFTER EOI (i + 1) */
                /* This means EOI is at positions (i-1, i) = (eoi_pos-2, eoi_pos-1) */
                eoi_pos = i + 1;
                break; /* CRITICAL: Break on FIRST EOI, not last */
            } else if (jpeg_data[i] == 0x00) {
                /* Escape sequence in scan data - skip both bytes */
                i += 2;
                continue;
            } else {
                i++;
            }
        } else {
            i++;
        }
    }
    
    /* CRITICAL: Verify that bytes immediately after EOI are not duplicate EOI or SOI */
    /* This prevents including duplicate EOI or next JPEG frame in the payload */
    /* eoi_pos points to position AFTER EOI, so bytes at eoi_pos and eoi_pos+1 are after EOI */
    if (eoi_pos < jpeg_size) {
        if (eoi_pos + 1 < jpeg_size && jpeg_data[eoi_pos] == 0xFF) {
            unsigned char next_byte = jpeg_data[eoi_pos + 1];
            if (next_byte == 0xD9 || next_byte == 0xD8) {
                /* Duplicate EOI or next JPEG SOI found - trim strictly to first EOI */
                /* eoi_pos already set correctly above (i + 1) - this is the position AFTER first EOI */
                /* We will copy eoi_pos bytes, which includes EOI at (eoi_pos-2, eoi_pos-1) */
            }
        }
    }
    
    /* CRITICAL: Verify that eoi_pos points to position after EOI, and EOI is at (eoi_pos-2, eoi_pos-1) */
    if (eoi_pos < 2 || eoi_pos > jpeg_size) {
        OPRINT("[RTP ERROR] Invalid EOI position: eoi_pos=%zu, jpeg_size=%zu\n", eoi_pos, jpeg_size);
        return -1;
    }
    if (jpeg_data[eoi_pos - 2] != 0xFF || jpeg_data[eoi_pos - 1] != 0xD9) {
        OPRINT("[RTP ERROR] EOI marker not found at expected position: eoi_pos=%zu\n", eoi_pos);
        return -1;
    }
    
    /* Verify no non-padding data after EOI */
    size_t check_pos = eoi_pos;
    while (check_pos < jpeg_size && (jpeg_data[check_pos] == 0x00 || jpeg_data[check_pos] == 0xFF)) {
        check_pos++;
    }
    if (check_pos < jpeg_size) {
        /* Non-padding data found after EOI - truncate at first EOI */
        /* eoi_pos already set correctly above */
    }
    
    /* Locate Start of Scan (SOS) marker and extract only entropy-coded scan */
    size_t sos_pos = 0, scan_start = 0;
    for (size_t i = soi_pos + 2; i + 3 < jpeg_size; ) {
        if (jpeg_data[i] == 0xFF && jpeg_data[i + 1] == 0xDA) {
            sos_pos = i;
            break;
        }
        if (jpeg_data[i] == 0xFF && jpeg_data[i + 1] == 0x00) {
            i += 2;
            continue;
        }
        i++;
    }
    if (sos_pos == 0) {
        OPRINT("[RTP ERROR] SOS not found in JPEG\n");
        return -1;
    }
    if (sos_pos + 3 >= jpeg_size) {
        OPRINT("[RTP ERROR] SOS header truncated\n");
        return -1;
    }
    uint16_t sos_len = ((uint16_t)jpeg_data[sos_pos + 2] << 8) | (uint16_t)jpeg_data[sos_pos + 3];
    if (sos_pos + 2 + sos_len > jpeg_size) {
        OPRINT("[RTP ERROR] SOS segment length exceeds JPEG size (sos_len=%u)\n", sos_len);
        return -1;
    }
    scan_start = sos_pos + 2 + sos_len;
    if (scan_start >= jpeg_size || scan_start > eoi_pos - 2) {
        OPRINT("[RTP ERROR] Invalid scan_start computed (scan_start=%zu, eoi_pos=%zu)\n", scan_start, eoi_pos);
        return -1;
    }

    /* Copy only entropy-coded scan data (exclude SOI..SOS headers and EOI) */
    size_t scan_len = (eoi_pos - 2) - scan_start;
    if (scan_len == 0) {
        OPRINT("[RTP ERROR] Empty scan segment\n");
        return -1;
    }
    unsigned char *scan_only = (unsigned char *)malloc(scan_len);
    if (!scan_only) {
        OPRINT("[RTP ERROR] Failed to allocate scan buffer (%zu bytes)\n", scan_len);
        return -1;
    }
    simd_memcpy(scan_only, jpeg_data + scan_start, scan_len);
    if (scan_len >= 2 && scan_only[scan_len - 2] == 0xFF && scan_only[scan_len - 1] == 0xD9) {
        free(scan_only);
        OPRINT("[RTP ERROR] Scan buffer ends with EOI unexpectedly\n");
        return -1;
    }

    /* Store scan-only payload for RTP transmission */
    frame_info->rtp_payload = scan_only;
    frame_info->rtp_payload_size = scan_len;
    frame_info->is_rtp_format = 1;
    frame_info->width = input_width;
    frame_info->height = input_height;
    frame_info->subsamp = input_subsamp;

               /* RFC 2435, Table: Types 0-7 (without restart)
                * Type 0 = 4:2:2, Type 1 = 4:2:0, Type 2 = 4:1:1, Type 3 = 4:4:4
                * CRITICAL: Correct subsampling mapping for color stability
                */
               switch (frame_info->subsamp) {
                   case TJSAMP_422: frame_info->jpeg_type = 0; break; /* 4:2:2 -> Type 0 */
                   case TJSAMP_420: frame_info->jpeg_type = 1; break; /* 4:2:0 -> Type 1 */
                   case TJSAMP_411: frame_info->jpeg_type = 2; break; /* 4:1:1 -> Type 2 */
                   case TJSAMP_444: frame_info->jpeg_type = 3; break; /* 4:4:4 -> Type 3 */
                   case TJSAMP_440: 
                       /* RFC 2435 doesn't support 4:4:0 - force to 4:2:0 (Type 1) */
                       OPRINT("[RTP WARNING] TJSAMP_440 (4:4:0) not supported by RFC 2435, mapping to Type 1 (4:2:0)\n");
                       frame_info->jpeg_type = 1; 
                       break;
                   case TJSAMP_GRAY:
                       /* Grayscale - RFC 2435 doesn't explicitly support it */
                       OPRINT("[RTP WARNING] TJSAMP_GRAY (grayscale) - using Type 3 (4:4:4) temporarily\n");
                       frame_info->jpeg_type = 3;
                       break;
                   default:
                       OPRINT("[RTP WARNING] Unknown subsampling %d, defaulting to Type 0 (4:2:2)\n", frame_info->subsamp);
                       frame_info->jpeg_type = 0; /* fallback: 4:2:2 */
                       break;
               }

               /* DEBUG: Log subsampling and type mapping */
               static int subsamp_log_count = 0;
               if (subsamp_log_count++ < 10) {
                   const char *subsamp_name = "UNKNOWN";
                   switch (frame_info->subsamp) {
                       case TJSAMP_420: subsamp_name = "4:2:0"; break;
                       case TJSAMP_422: subsamp_name = "4:2:2"; break;
                       case TJSAMP_444: subsamp_name = "4:4:4"; break;
                       case TJSAMP_440: subsamp_name = "4:4:0"; break;
                       case TJSAMP_411: subsamp_name = "4:1:1"; break;
                       case TJSAMP_GRAY: subsamp_name = "GRAY"; break;
                       default: break;
                   }
                   OPRINT("[RTP DEBUG] Frame subsamp=%s (%d), RTP type=%d, width=%d, height=%d\n",
                          subsamp_name, frame_info->subsamp, frame_info->jpeg_type,
                          frame_info->width, frame_info->height);
               }

    /* CRITICAL: Extract and cache quantization tables from source JPEG BEFORE extracting scan data.
     * Store tables in frame structure to avoid race condition with global cache.
     * Tables will be included in RTP payload if Q=255 is set.
     * NO sanitization - preserve original values as-is in NATURAL order (as in DQT)
     */
    rtpjpeg_cache_qtables_from_jpeg(jpeg_data, eoi_pos);
    const uint8_t *qt_luma_ptr = NULL, *qt_chroma_ptr = NULL;
    int have_luma = 0, have_chroma = 0, qt_precision = 0;
    if (rtpjpeg_get_cached_qtables(&qt_luma_ptr, &qt_chroma_ptr, &have_luma, &have_chroma, &qt_precision) == 0) {
        /* Copy tables to frame structure - preserve NATURAL order as in DQT */
        frame_info->have_luma = have_luma;
        frame_info->have_chroma = have_chroma;
        frame_info->qt_precision = qt_precision;
        if (have_luma && qt_luma_ptr) {
            memcpy(frame_info->qt_luma, qt_luma_ptr, 64);
            /* NO sanitization - preserve original values as-is */
        }
        if (have_chroma && qt_chroma_ptr) {
            memcpy(frame_info->qt_chroma, qt_chroma_ptr, 64);
            /* NO sanitization - preserve original values as-is */
        }
        static int cache_log_count = 0;
        if (cache_log_count++ < 5) {
            OPRINT("[RTP DEBUG] Quantization tables cached: have_luma=%d, have_chroma=%d, precision=%d\n",
                   have_luma, have_chroma, qt_precision);
        }
    } else {
        frame_info->have_luma = 0;
        frame_info->have_chroma = 0;
        frame_info->qt_precision = 0;
        static int cache_log_count = 0;
        if (cache_log_count++ < 5) {
            OPRINT("[RTP DEBUG] No quantization tables found in JPEG\n");
        }
    }

    return 0;
}

/******************************************************************************
Description.: Send RTP packet with RFC 2435 compliant JPEG payload
Input Value.: client, prepared frame info, timestamp
Return Value: 0 on success, -1 on error
******************************************************************************/
static int send_rtp_packet(int rtp_socket, rtsp_client_t *client, const rtp_jpeg_frame_t *frame,
                           uint32_t frame_timestamp)
{
    /* Initialize QT insertion settings */
    const int qt_insertion_enabled = 1; /* ENABLED: RFC 2435 QT header insertion */
    const int have_both = frame->have_luma && frame->have_chroma;
    const int q255_ok = (frame->qt_precision == 0) && have_both;
    int q_value_fixed = q255_ok ? 255 : 75;
    
    /* Log QT disabled fallback */
    static int qt_fallback_log_count = 0;
    if (!q255_ok && qt_fallback_log_count++ < 5) {
        OPRINT("[RTP WARNING] QT disabled, fallback Q=75 (no DQT): precision=%d, have_luma=%d, have_chroma=%d\n",
               frame->qt_precision, frame->have_luma, frame->have_chroma);
    }
    
    /* DEBUG: Log actual subsamp and selected type before sending */
    static int send_subsamp_log_count = 0;
    if (send_subsamp_log_count++ < 5) {
        const char *subsamp_name = "UNKNOWN";
        switch (frame->subsamp) {
            case TJSAMP_420: subsamp_name = "4:2:0"; break;
            case TJSAMP_422: subsamp_name = "4:2:2"; break;
            case TJSAMP_444: subsamp_name = "4:4:4"; break;
            case TJSAMP_440: subsamp_name = "4:4:0"; break;
            case TJSAMP_411: subsamp_name = "4:1:1"; break;
            case TJSAMP_GRAY: subsamp_name = "GRAY"; break;
            default: break;
        }
        OPRINT("[RTP DEBUG] send_rtp_packet: subsamp=%s (%d), type=%d, Q=%d, is_rtp_format=%d\n",
               subsamp_name, frame->subsamp, frame->jpeg_type, q_value_fixed, frame->is_rtp_format);
    }
    
    static int send_count = 0;
    if (send_count++ < 5) {
        OPRINT("[RTP DEBUG] send_rtp_packet called: frame_size=%zu, subsamp=%d, type=%d, is_rtp_format=%d\n",
               frame->rtp_payload_size, frame->subsamp, frame->jpeg_type, frame->is_rtp_format);
    }

    if (!client || !frame || !frame->rtp_payload || frame->rtp_payload_size <= 0) {
        OPRINT("[RTP ERROR] invalid frame data for transmission\n");
        return -1;
    }

    if (frame->width <= 0 || frame->height <= 0) {
        OPRINT("[RTP ERROR] invalid frame dimensions (%d x %d)\n", frame->width, frame->height);
        return -1;
    }

    /* Fragment and send scan-only payload per RFC 2435 (no SOI/EOI in payload) */
    const unsigned char *jpeg_data = frame->rtp_payload;   /* points to scan data */
    size_t jpeg_size = frame->rtp_payload_size;            /* scan length */
    size_t fragment_offset = 0;
    size_t remaining = jpeg_size;
    uint16_t seq = client->sequence_number;
    int is_tcp = (client->rtp_port == 0);
    size_t max_packet_size = is_tcp ? MAX_TCP_PACKET_SIZE : MAX_RTP_PACKET_SIZE;
    /* Q=255 if quantization tables are cached and 8-bit AND will be inserted, else 75 */
    /* Use tables from frame structure (not global cache) to avoid race condition */
    const uint8_t *qt_luma = frame->have_luma ? frame->qt_luma : NULL;
    const uint8_t *qt_chroma = frame->have_chroma ? frame->qt_chroma : NULL;
    int have_luma = frame->have_luma;
    int have_chroma = frame->have_chroma;
    int qt_precision = frame->qt_precision;
    /* qt_insertion_enabled and q_value_fixed already initialized at function start */

    /* Calculate width/8 and height/8 with safe rounding up (ceil) */
    int frame_width_div8 = (frame->width + 7) / 8;
    int frame_height_div8 = (frame->height + 7) / 8;
    if (frame_width_div8 <= 0 || frame_height_div8 <= 0 || frame_width_div8 > 255 || frame_height_div8 > 255) {
        OPRINT("[RTP ERROR] frame dimensions not divisible by 8 or too large (%d x %d, div8: %d x %d)\n", 
               frame->width, frame->height, frame_width_div8, frame_height_div8);
        return -1;
    }

    const unsigned char jpeg_type_fixed = (unsigned char)frame->jpeg_type;

    while (remaining > 0) {
        size_t total_header_size = 20; /* 12 RTP + 8 JPEG header */
        if (total_header_size >= max_packet_size) {
            OPRINT("[RTP ERROR] header size %zu exceeds packet size limit %zu\n", total_header_size, max_packet_size);
            return -1;
        }

        size_t max_payload = max_packet_size - total_header_size;
        if (max_payload == 0) {
            OPRINT("[RTP ERROR] no room for payload after headers\n");
            return -1;
        }

        /* Calculate QT header length if needed (for first fragment with Q=255) */
        /* CRITICAL: Must be calculated BEFORE payload_size calculation */
        /* We calculate it here, but actual building happens later after packet creation */
        size_t qt_hdr_len = 0;
        if (qt_insertion_enabled && fragment_offset == 0 && q_value_fixed == 255) {
            /* Calculate QT header size: MBZ(1) + Pq(1) + Lq(2) + [Tq(1) + 64 bytes]*2 */
            qt_hdr_len = 4 + 65 + 65; /* MBZ,Pq,Lq + (Tq+64)*2 */
        }

        /* Calculate payload size: reduce by qt_hdr_len if QT header will be inserted */
        /* CRITICAL: payload_size must be reduced by qt_hdr_len in first fragment to make room for QT header */
        /* CRITICAL: Ensure max_scan_payload is not negative */
        size_t max_scan_payload = (qt_hdr_len < max_payload) ? (max_payload - qt_hdr_len) : 0;
        if (max_scan_payload == 0 && qt_hdr_len > 0) {
            OPRINT("[RTP ERROR] QT header too large: qt_hdr_len=%zu, max_payload=%zu\n", qt_hdr_len, max_payload);
            return -1;
        }
        size_t payload_size = (remaining < max_scan_payload) ? remaining : max_scan_payload;
        if (payload_size == 0) {
            OPRINT("[RTP ERROR] payload_size is 0: remaining=%zu, max_scan_payload=%zu, qt_hdr_len=%zu, max_payload=%zu\n", 
                   remaining, max_scan_payload, qt_hdr_len, max_payload);
            return -1;
        }
        int is_last_packet = (payload_size == remaining);

        unsigned char packet[MAX_TCP_PACKET_SIZE];

        packet[0] = 0x80; /* V=2, P=0, X=0, CC=0 */
        packet[1] = (is_last_packet ? 0x80 : 0x00) | RTP_PAYLOAD_TYPE; /* M=1 only for last */
        packet[2] = (seq >> 8) & 0xFF;
        packet[3] = seq & 0xFF;
        packet[4] = (frame_timestamp >> 24) & 0xFF;
        packet[5] = (frame_timestamp >> 16) & 0xFF;
        packet[6] = (frame_timestamp >> 8) & 0xFF;
        packet[7] = frame_timestamp & 0xFF;
        packet[8] = (RTP_SSRC >> 24) & 0xFF;
        packet[9] = (RTP_SSRC >> 16) & 0xFF;
        packet[10] = (RTP_SSRC >> 8) & 0xFF;
        packet[11] = RTP_SSRC & 0xFF;
        
        /* 8‑byte JPEG header */
        /* CRITICAL: fragment_offset in JPEG header (bytes 13-15) counts ONLY scan data, NOT QT header */
        /* This is correct - fragment_offset is already set to scan data offset */
        packet[12] = 0;                                      /* Type‑specific */
        packet[13] = (fragment_offset >> 16) & 0xFF;         /* off[23:16] */
        packet[14] = (fragment_offset >> 8) & 0xFF;          /* off[15:8]  */
        packet[15] = fragment_offset & 0xFF;                 /* off[7:0]   */
        packet[16] = jpeg_type_fixed;                        /* Type */
        packet[17] = (unsigned char)q_value_fixed;           /* Q (75 when QT disabled, 255 when QT enabled) */
        packet[18] = (unsigned char)frame_width_div8;        /* Width/8 */
        packet[19] = (unsigned char)frame_height_div8;       /* Height/8 */
        
        size_t payload_offset = 20;
        
        /* CRITICAL: QT header must fit in first packet */
        /* Check: payload_offset (20) + qt_hdr_len <= max_packet_size - 12 (TCP wrapper) */
        if (qt_insertion_enabled && fragment_offset == 0 && q_value_fixed == 255) {
            if (payload_offset + qt_hdr_len > max_packet_size - 12) {
                OPRINT("[RTP WARNING] QT header too large for packet: qt_hdr_len=%zu, max_packet_size=%zu, falling back to Q=75\n",
                       qt_hdr_len, max_packet_size);
                q_value_fixed = 75;
                qt_hdr_len = 0;
                /* Update Q value in packet header */
                packet[17] = (unsigned char)q_value_fixed;
            }
        }
        
        /* Build and insert QT header if needed (for first fragment with Q=255) */
        if (qt_insertion_enabled && fragment_offset == 0 && q_value_fixed == 255) {
            /* Quantization Table Header (RFC 2435 Section 3.1.8)
             * Convert from NATURAL order (DQT) to ZIGZAG order (as FFmpeg expects)
             * Validate for zeros AFTER zigzag conversion
             */
            
            /* Convert tables from natural order (DQT) to zigzag order (RFC 2435) */
            uint8_t lz[64], cz[64];
            for (int i = 0; i < 64; i++) {
                lz[i] = frame->qt_luma[rfc2435_zigzag[i]];
            }
            for (int i = 0; i < 64; i++) {
                cz[i] = frame->qt_chroma[rfc2435_zigzag[i]];
            }
            
            /* Validate: FFmpeg crashes on zeros/garbage */
            int found_zero = 0;
            for (int i = 0; i < 64; i++) {
                if (!lz[i] || !cz[i]) {
                    found_zero = 1;
                    static int zero_log_count = 0;
                    if (zero_log_count++ < 3) {
                        OPRINT("[RTP WARNING] Found zero in zigzag-ordered QT table (lz[%d]=%d, cz[%d]=%d), falling back to Q=75\n",
                               i, lz[i], i, cz[i]);
                    }
                    break;
                }
            }
            
            /* Fallback to Q=75 if zero found */
            if (found_zero) {
                q_value_fixed = 75;
                qt_hdr_len = 0;
                /* Update Q value in packet header */
                packet[17] = (unsigned char)q_value_fixed;
                static int qt_fallback_log_count = 0;
                if (qt_fallback_log_count++ < 3) {
                    OPRINT("[RTP WARNING] QT disabled, fallback Q=75 (no DQT) - zero found in zigzag tables\n");
                }
            } else {
                /* Build QT header: MBZ(1) + Pq(1) + Lq(2) + [Tq(1) + 64 bytes] per table */
                size_t qt_hdr_len_calc = 4 + 65 + 65; /* MBZ,Pq,Lq + (Tq+64)*2 */
                
                /* Check that QT header fits in first packet */
                if (20 + qt_hdr_len_calc >= max_packet_size) {
                    q_value_fixed = 75;
                    qt_hdr_len = 0;
                    /* Update Q value in packet header */
                    packet[17] = (unsigned char)q_value_fixed;
                    static int qt_fallback_log_count = 0;
                    if (qt_fallback_log_count++ < 3) {
                        OPRINT("[RTP WARNING] QT disabled, fallback Q=75 (no DQT) - QT header too large\n");
                    }
                } else {
                    uint8_t qt_hdr[4 + 130];
                    size_t off = 0;
                    
                    qt_hdr[off++] = 0x00;                 /* MBZ */
                    qt_hdr[off++] = 0x00;                 /* Pq=0 (8-bit) */
                    uint16_t Lq = 130;                    /* (Tq+64)*2 */
                    qt_hdr[off++] = (Lq >> 8) & 0xFF;
                    qt_hdr[off++] = Lq & 0xFF;
                    qt_hdr[off++] = 0x00;                 /* Tq=0 (luma) */
                    memcpy(qt_hdr + off, lz, 64);
                    off += 64;
                    qt_hdr[off++] = 0x01;                 /* Tq=1 (chroma) */
                    memcpy(qt_hdr + off, cz, 64);
                    off += 64;
                    
                    /* Verify QT header length */
                    if (off != qt_hdr_len_calc) {
                        OPRINT("[RTP ERROR] QT header length mismatch: calculated=%zu, actual=%zu\n", qt_hdr_len_calc, off);
                        return -1;
                    }
                    
                    /* Update qt_hdr_len for payload_size calculation */
                    qt_hdr_len = qt_hdr_len_calc;
                    
                    /* DEBUG: Log QT header info (once per frame) */
                    static int qt_pre_insert_log_count = 0;
                    if (qt_pre_insert_log_count++ < 1) {
                        OPRINT("[RTP QT PRE-INSERT] Lq=%04x (BE), lz[0..7]=%02x %02x %02x %02x %02x %02x %02x %02x, cz[0..7]=%02x %02x %02x %02x %02x %02x %02x %02x\n",
                               Lq, lz[0], lz[1], lz[2], lz[3], lz[4], lz[5], lz[6], lz[7],
                               cz[0], cz[1], cz[2], cz[3], cz[4], cz[5], cz[6], cz[7]);
                    }
                    
                    /* Insert QT header into packet */
                    memcpy(packet + payload_offset, qt_hdr, qt_hdr_len);
                }
            }
        }
        
        /* Copy scan data to packet */
        if (qt_insertion_enabled && fragment_offset == 0 && q_value_fixed == 255 && qt_hdr_len > 0) {
            /* QT header already inserted, copy scan data after it */
            simd_memcpy(packet + payload_offset + qt_hdr_len, jpeg_data + fragment_offset, payload_size);
        } else {
            /* No QT header - just copy scan data */
            simd_memcpy(packet + payload_offset, jpeg_data + fragment_offset, payload_size);
        }
        
        /* Calculate packet size: RTP+JPEG headers (20) + QT header (if any) + scan data */
        size_t packet_size = payload_offset + qt_hdr_len + payload_size;
        
        /* CRITICAL: Dump raw RTP packet BEFORE TCP wrapping for analysis */
        static int raw_packet_dump_count = 0;
        if (raw_packet_dump_count++ < 3 && fragment_offset == 0) {
            OPRINT("[RTP DEBUG] raw_packet_dump_count=%d, fragment_offset=%zu, q_value_fixed=%d, packet_size=%zu\n", 
                   raw_packet_dump_count, fragment_offset, q_value_fixed, packet_size);
            const char *dump_bin = "/Users/350d/Library/Mobile Documents/com~apple~CloudDocs/GIT/mjpg-streamer/tmp/raw_rtp_packet.bin";
            const char *dump_hex = "/Users/350d/Library/Mobile Documents/com~apple~CloudDocs/GIT/mjpg-streamer/tmp/raw_rtp_packet.hex";
            FILE *dump_file = fopen(dump_bin, "wb");
            if (dump_file) {
                fwrite(packet, 1, packet_size, dump_file);
                fclose(dump_file);
                OPRINT("[RTP RAW DUMP] Dumped %zu bytes to %s\n", packet_size, dump_bin);
                
                FILE *hex_file = fopen(dump_hex, "w");
                if (hex_file) {
                    fprintf(hex_file, "Raw RTP packet (%zu bytes):\n", packet_size);
                    fprintf(hex_file, "RTP Header (12 bytes):\n");
                    for (int i = 0; i < 12 && i < packet_size; i++) {
                        fprintf(hex_file, "%02x ", packet[i]);
                    }
                    fprintf(hex_file, "\n\nJPEG Header (8 bytes, offset 12):\n");
                    for (int i = 12; i < 20 && i < packet_size; i++) {
                        fprintf(hex_file, "%02x ", packet[i]);
                    }
                    fprintf(hex_file, "\n\nPayload (offset 20, first 200 bytes):\n");
                    for (int i = 20; i < packet_size && i < 220; i++) {
                        fprintf(hex_file, "%02x ", packet[i]);
                        if ((i+1) % 16 == 0) fprintf(hex_file, "\n");
                    }
                    fprintf(hex_file, "\n");
                    fclose(hex_file);
                    OPRINT("[RTP RAW DUMP] Dumped hex to %s\n", dump_hex);
                }
            } else {
                OPRINT("[RTP RAW DUMP] Failed to open %s: %s\n", dump_bin, strerror(errno));
            }
        }
        
        /* CRITICAL: Verify packet size is valid */
        if (packet_size > MAX_TCP_PACKET_SIZE) {
            OPRINT("[RTP ERROR] Packet size exceeds MAX_TCP_PACKET_SIZE: packet_size=%zu, MAX=%d\n", 
                   packet_size, MAX_TCP_PACKET_SIZE);
            return -1;
        }
        if (packet_size < 20) {
            OPRINT("[RTP ERROR] Packet size too small: packet_size=%zu (minimum 20 bytes)\n", packet_size);
            return -1;
        }

        if (fragment_offset == 0 && payload_size >= 2 && jpeg_data[0] == 0xFF) {
            unsigned char b2 = jpeg_data[1];
            if (b2 == 0xD8 || b2 == 0xE0 || b2 == 0xE1 || b2 == 0xDB || b2 == 0xC0 || b2 == 0xC4 || b2 == 0xDA) {
                OPRINT("[RTP ERROR] Scan payload begins with JPEG marker 0xFF 0x%02X\n", b2);
                return -1;
            }
        }
        if (is_last_packet && payload_size >= 2) {
            if (jpeg_data[fragment_offset + payload_size - 2] == 0xFF &&
                jpeg_data[fragment_offset + payload_size - 1] == 0xD9) {
                OPRINT("[RTP ERROR] Last packet payload ends with FF D9! This should not happen - EOI was excluded in prepare_rtp_jpeg_frame\n");
                return -1;
            }
        }

        int sent = 0;
        if (is_tcp) {
            static int tcp_nodelay_sockets_[MAX_CLIENTS] = {0};
            int client_idx = -1;
            pthread_mutex_lock(&clients_mutex);
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (clients[i].socket == client->socket) { client_idx = i; break; }
            }
            pthread_mutex_unlock(&clients_mutex);
            if (client_idx >= 0 && !tcp_nodelay_sockets_[client_idx]) {
                int opt = 1; setsockopt(client->socket, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
                tcp_nodelay_sockets_[client_idx] = 1;
            }
            unsigned char tcp_packet[4 + MAX_TCP_PACKET_SIZE];
            tcp_packet[0] = '$'; tcp_packet[1] = 0; /* channel 0 */
            tcp_packet[2] = (packet_size >> 8) & 0xFF; tcp_packet[3] = packet_size & 0xFF;
            simd_memcpy(tcp_packet + 4, packet, packet_size);
            size_t to_send = 4 + packet_size; const unsigned char *ptr = tcp_packet;
            
            /* DEBUG: Log packet sending with QT header */
            static int packet_send_log_count = 0;
            if (qt_insertion_enabled && fragment_offset == 0 && q_value_fixed == 255 && packet_send_log_count++ < 3) {
                OPRINT("[RTP DEBUG] Sending packet with QT header: packet_size=%zu, tcp_packet_size=%zu, fragment_offset=%zu\n",
                       packet_size, to_send, fragment_offset);
            }
            
            while (to_send > 0) {
                sent = send(client->socket, ptr, to_send, MSG_NOSIGNAL | MSG_DONTWAIT);
                if (sent <= 0) {
                    if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                        /* Check for connection errors - socket may be closed */
                        if (errno == EPIPE || errno == ECONNRESET || errno == EBADF) {
                            OPRINT("Error sending RTP over TCP: %s (socket closed, packet_size=%zu)\n", strerror(errno), packet_size);
                            return -1;
                        }
                        OPRINT("Error sending RTP over TCP: %s (packet_size=%zu, sent=%d)\n", strerror(errno), packet_size, sent); 
                        return -1; 
                    }
                    if (errno == EAGAIN || errno == EWOULDBLOCK) { 
                        /* Socket buffer full - wait a bit and retry */
                        usleep(1000); /* 1ms */
                        continue; 
                    }
                    continue;
                }
                to_send -= sent; ptr += sent;
            }
        } else {
            struct sockaddr_in rtp_addr; memset(&rtp_addr, 0, sizeof(rtp_addr));
            rtp_addr.sin_family = AF_INET; rtp_addr.sin_addr = client->addr.sin_addr; rtp_addr.sin_port = htons(client->rtp_port);
            sent = sendto(rtp_socket, packet, packet_size, 0, (struct sockaddr *)&rtp_addr, sizeof(rtp_addr));
            if (sent < 0 || sent != (int)packet_size) { OPRINT("Error/partial UDP send: %s\n", strerror(errno)); return -1; }
        }

        seq++;
        fragment_offset += payload_size;
        remaining -= payload_size;
    }

    client->sequence_number = seq;
    return 0;
}

/******************************************************************************
Description.: Handle RTSP request
Input Value.: client socket, request buffer
Return Value: 0 on success, -1 on error
******************************************************************************/
static void handle_rtsp_request(int client_socket, struct sockaddr_in client_addr, char *request, int input_number) {
    char response[1024];
    int cseq = 0;
    int session_id = 123456;
    char request_copy[4096];
    char *saveptr = NULL;
    
    // Make a copy since strtok_r modifies the buffer
    strncpy(request_copy, request, sizeof(request_copy) - 1);
    request_copy[sizeof(request_copy) - 1] = '\0';
    
    char *line = strtok_r(request_copy, "\r\n", &saveptr);
    if (!line) {
        OPRINT(" o: Invalid RTSP request\n");
        return;
    }
    char *method = strtok(line, " ");
    char *uri = strtok(NULL, " ");
    char *version = strtok(NULL, " ");
    if (!method || !uri || !version) {
        OPRINT(" o: Malformed RTSP request\n");
        return;
    }
    
    OPRINT(" o: RTSP request: %s %s %s\n", method, uri, version);

    // Parse headers
    while ((line = strtok_r(NULL, "\r\n", &saveptr))) {
        if (strncmp(line, "CSeq:", 5) == 0) {
            cseq = atoi(line + 6);
        }
    }

    if (strcmp(method, "OPTIONS") == 0) {
        snprintf(response, sizeof(response),
                 "RTSP/1.0 200 OK\r\n"
                 "CSeq: %d\r\n"
                 "Public: OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN\r\n"
                 "Server: MJPG-Streamer RTSP Server\r\n"
                 "\r\n", cseq);
        send(client_socket, response, strlen(response), 0);
    } else if (strcmp(method, "DESCRIBE") == 0) {
        char sdp[512];
        int width = 640, height = 480; // Default values
        
        /* CRITICAL: Always use cached dimensions for SDP - they are updated from actual JPEG frames */
        /* This ensures SDP and RTP headers always match, preventing VLC flickering */
        /* If cached dimensions are not set yet, try to get them from globals as fallback */
        if (sdp_dimensions_cached && cached_sdp_width > 0 && cached_sdp_height > 0) {
            /* Use cached dimensions from actual JPEG frames - guaranteed to match RTP headers */
            width = cached_sdp_width;
            height = cached_sdp_height;
        } else if (pglobal != NULL && input_number >= 0 && input_number < pglobal->incnt) {
            /* Fallback: use globals if cached dimensions are not set yet */
            int current_width = pglobal->in[input_number].width;
            int current_height = pglobal->in[input_number].height;
            
            if (current_width > 0 && current_height > 0) {
                /* Cache dimensions from globals as initial value */
                /* They will be updated from actual JPEG frames in stream_worker_thread */
                cached_sdp_width = current_width;
                cached_sdp_height = current_height;
                sdp_dimensions_cached = 1;
                width = cached_sdp_width;
                height = cached_sdp_height;
            }
        }
        
        /* Get FPS from input plugin if available */
        int fps = 30; /* default */
        if (pglobal != NULL && input_number >= 0 && input_number < pglobal->incnt && pglobal->in[input_number].fps > 0) {
            fps = pglobal->in[input_number].fps;
        }
        
        snprintf(sdp, sizeof(sdp),
                 "v=0\r\n"
                 "o=- %d %d IN IP4 %s\r\n"
                 "s=MJPG-Streamer Stream\r\n"
                 "t=0 0\r\n"
                 "a=tool:MJPG-Streamer\r\n"
                 "m=video 0 RTP/AVP 26\r\n"
                 "c=IN IP4 0.0.0.0\r\n"
                 "b=AS:5000\r\n"
                 "a=control:track1\r\n"
                 "a=rtpmap:26 JPEG/90000\r\n"
                 "a=fmtp:26 width=%d;height=%d\r\n"
                 "a=framesize:26 %d-%d\r\n"
                 "a=framerate:%d\r\n",
                 (int)time(NULL), (int)time(NULL), inet_ntoa(client_addr.sin_addr), width, height, width, height, fps);
        snprintf(response, sizeof(response),
                 "RTSP/1.0 200 OK\r\n"
                 "CSeq: %d\r\n"
                 "Content-Type: application/sdp\r\n"
                 "Content-Length: %zu\r\n"
                 "Server: MJPG-Streamer RTSP Server\r\n"
                 "\r\n"
                 "%s",
                 cseq, strlen(sdp), sdp);
        send(client_socket, response, strlen(response), 0);
    } else if (strcmp(method, "SETUP") == 0) {
        int client_rtp_port = 0, client_rtcp_port = 0;
        int use_tcp = 0; /* Default to UDP, but use what client requests */
        // Parse Transport header - use what client requests
        char *transport_line = strstr(request, "Transport:");
        if (transport_line) {
            /* Check if client explicitly requested TCP */
            if (strstr(transport_line, "RTP/AVP/TCP")) { 
                use_tcp = 1;
            } else {
                /* Check if client explicitly requested UDP */
            char *client_port = strstr(transport_line, "client_port=");
            if (client_port) {
                sscanf(client_port, "client_port=%d-%d", &client_rtp_port, &client_rtcp_port);
                    if (client_rtp_port > 0 && client_rtcp_port > 0) {
                        use_tcp = 0;
                    }
                }
            }
        }
        
        if (use_tcp) {
            int i;
            pthread_mutex_lock(&clients_mutex);
            for (i = 0; i < MAX_CLIENTS; i++) {
                if (!clients[i].active) {
                    clients[i].active = 1;
                    clients[i].socket = client_socket;
                    clients[i].rtp_port = 0; // TCP mode
                    clients[i].rtcp_port = 0;
                    clients[i].sequence_number = 0;
                    /* Initialize timestamp at 0 - will increment from first frame */
                    clients[i].timestamp = 0;
                    clients[i].addr = client_addr;
                    clients[i].playing = 0;
                    break;
                }
            }
            pthread_mutex_unlock(&clients_mutex);
            if (i < MAX_CLIENTS) {
                snprintf(response, sizeof(response),
                         "RTSP/1.0 200 OK\r\n"
                         "CSeq: %d\r\n"
                         "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
                         "Session: %d\r\n"
                         "Server: MJPG-Streamer RTSP Server\r\n"
                         "\r\n", cseq, session_id);
            } else {
                snprintf(response, sizeof(response),
                         "RTSP/1.0 503 Service Unavailable\r\n"
                         "CSeq: %d\r\n"
                         "Server: MJPG-Streamer RTSP Server\r\n"
                         "\r\n", cseq);
            }
        } else {
            int i;
            pthread_mutex_lock(&clients_mutex);
            for (i = 0; i < MAX_CLIENTS; i++) {
                if (!clients[i].active) {
                    clients[i].active = 1;
                    clients[i].socket = client_socket;
                    clients[i].rtp_port = client_rtp_port;
                    clients[i].rtcp_port = client_rtcp_port;
                    clients[i].sequence_number = 0;
                    /* Initialize timestamp at 0 - will increment from first frame */
                    clients[i].timestamp = 0;
                    clients[i].addr = client_addr;
                    clients[i].playing = 0;
                    break;
                }
            }
            pthread_mutex_unlock(&clients_mutex);
            if (i < MAX_CLIENTS) {
                snprintf(response, sizeof(response),
                         "RTSP/1.0 200 OK\r\n"
                         "CSeq: %d\r\n"
                         "Transport: RTP/AVP;unicast;client_port=%d-%d;server_port=5004-5005;source=%s\r\n"
                         "Session: %d\r\n"
                         "Server: MJPG-Streamer RTSP Server\r\n"
                         "\r\n", cseq, client_rtp_port, client_rtcp_port, inet_ntoa(client_addr.sin_addr), session_id);
            } else {
                snprintf(response, sizeof(response),
                         "RTSP/1.0 503 Service Unavailable\r\n"
                         "CSeq: %d\r\n"
                         "Server: MJPG-Streamer RTSP Server\r\n"
                         "\r\n", cseq);
            }
        }
        send(client_socket, response, strlen(response), 0);
    } else if (strcmp(method, "PLAY") == 0) {
        int i;
        pthread_mutex_lock(&clients_mutex);
        for (i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].active && clients[i].socket == client_socket) {
                clients[i].playing = 1;
                OPRINT(" o: Client %d set to playing state (socket %d)\n", i, client_socket);
                break;
            }
        }
        if (i < MAX_CLIENTS) {
        } else {
            OPRINT("[RTSP ERROR] PLAY request but no matching client found for socket %d\n", client_socket);
        }
        pthread_mutex_unlock(&clients_mutex);
        /* Send PLAY response FIRST, then wake streaming thread */
        /* This ensures client is ready to receive RTP packets before we start sending */
        snprintf(response, sizeof(response),
                 "RTSP/1.0 200 OK\r\n"
                 "CSeq: %d\r\n"
                 "Session: %d\r\n"
                 "Server: MJPG-Streamer RTSP Server\r\n"
                 "\r\n", cseq, session_id);
        send(client_socket, response, strlen(response), 0);
        /* Wake streaming thread AFTER PLAY response is sent */
        /* This ensures client is ready to receive RTP packets */
        /* Send immediately - delays may cause flickering in VLC */
        if (pglobal && i < MAX_CLIENTS) {
            pthread_mutex_lock(&pglobal->in[input_number].db);
            pthread_cond_broadcast(&pglobal->in[input_number].db_update);
            pthread_mutex_unlock(&pglobal->in[input_number].db);
        }
    } else if (strcmp(method, "PAUSE") == 0) {
        int i;
        pthread_mutex_lock(&clients_mutex);
        for (i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].active && clients[i].socket == client_socket) {
                clients[i].playing = 0;
                break;
            }
        }
        pthread_mutex_unlock(&clients_mutex);
        snprintf(response, sizeof(response),
                 "RTSP/1.0 200 OK\r\n"
                 "CSeq: %d\r\n"
                 "Session: %d\r\n"
                 "Server: MJPG-Streamer RTSP Server\r\n"
                 "\r\n", cseq, session_id);
        send(client_socket, response, strlen(response), 0);
    } else if (strcmp(method, "TEARDOWN") == 0) {
        int i;
        pthread_mutex_lock(&clients_mutex);
        for (i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].active && clients[i].socket == client_socket) {
                clients[i].active = 0;
                clients[i].playing = 0;
                break;
            }
        }
        pthread_mutex_unlock(&clients_mutex);
        snprintf(response, sizeof(response),
                 "RTSP/1.0 200 OK\r\n"
                 "CSeq: %d\r\n"
                 "Session: %d\r\n"
                 "Server: MJPG-Streamer RTSP Server\r\n"
                 "\r\n", cseq, session_id);
        send(client_socket, response, strlen(response), 0);
    } else {
        snprintf(response, sizeof(response),
                 "RTSP/1.0 400 Bad Request\r\n"
                 "CSeq: %d\r\n"
                 "Server: MJPG-Streamer RTSP Server\r\n"
                 "\r\n", cseq);
        send(client_socket, response, strlen(response), 0);
    }
}

/******************************************************************************
Description.: Handle client in separate thread
Input Value.: client data structure
Return Value: NULL
******************************************************************************/
typedef struct {
    int socket;
    struct sockaddr_in addr;
} client_data_t;

/******************************************************************************
Description.: Handle HTTP snapshot request
Input Value.: client socket
Return Value: 0 on success, -1 on error
******************************************************************************/
static int handle_http_snapshot(int client_socket) {
    pthread_mutex_lock(&snapshot_mutex);
    
    if (snapshot_buffer == NULL || snapshot_size == 0) {
        pthread_mutex_unlock(&snapshot_mutex);
        const char *error_response = 
            "HTTP/1.0 503 Service Unavailable\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 19\r\n"
            "\r\n"
            "No frame available";
        send(client_socket, error_response, strlen(error_response), 0);
        return -1;
    }
    
    /* Allocate buffer for snapshot copy */
    unsigned char *snapshot_copy = malloc(snapshot_size);
    if (!snapshot_copy) {
        pthread_mutex_unlock(&snapshot_mutex);
        const char *error_response = 
            "HTTP/1.0 500 Internal Server Error\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 13\r\n"
            "\r\n"
            "Out of memory";
        send(client_socket, error_response, strlen(error_response), 0);
        return -1;
    }
    
    simd_memcpy(snapshot_copy, snapshot_buffer, snapshot_size);
    size_t snapshot_size_copy = snapshot_size;
    pthread_mutex_unlock(&snapshot_mutex);
    
    /* Send HTTP response with JPEG snapshot */
    char header[512];
    snprintf(header, sizeof(header),
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: image/jpeg\r\n"
        "Content-Length: %zu\r\n"
        "Cache-Control: no-cache, no-store, must-revalidate\r\n"
        "Pragma: no-cache\r\n"
        "Expires: 0\r\n"
        "\r\n",
        snapshot_size_copy);
    
    if (send(client_socket, header, strlen(header), 0) < 0) {
        free(snapshot_copy);
        return -1;
    }
    
    if (send(client_socket, snapshot_copy, snapshot_size_copy, 0) < 0) {
        free(snapshot_copy);
        return -1;
    }
    
    free(snapshot_copy);
    return 0;
}

/******************************************************************************
Description.: Handle HTTP request
Input Value.: client socket, request buffer
Return Value: 0 on success, -1 on error
******************************************************************************/
static int handle_http_request(int client_socket, char *request) {
    /* Check if this is a GET or HEAD request for /snapshot */
    int is_get = (strncmp(request, "GET /snapshot", 13) == 0 || 
                  strncmp(request, "GET /snapshot ", 14) == 0 ||
                  strstr(request, "GET /snapshot") != NULL);
    int is_head = (strncmp(request, "HEAD /snapshot", 14) == 0 || 
                   strncmp(request, "HEAD /snapshot ", 15) == 0 ||
                   strstr(request, "HEAD /snapshot") != NULL);
    
    if (is_get || is_head) {
        if (is_head) {
            /* HEAD request - return headers only */
            pthread_mutex_lock(&snapshot_mutex);
            if (snapshot_buffer == NULL || snapshot_size == 0) {
                pthread_mutex_unlock(&snapshot_mutex);
                const char *error_response = 
                    "HTTP/1.0 503 Service Unavailable\r\n"
                    "Content-Type: text/plain\r\n"
                    "Content-Length: 19\r\n"
                    "\r\n";
                send(client_socket, error_response, strlen(error_response), 0);
                return -1;
            }
            size_t size = snapshot_size;
            pthread_mutex_unlock(&snapshot_mutex);
            
            char header[512];
            snprintf(header, sizeof(header),
                "HTTP/1.0 200 OK\r\n"
                "Content-Type: image/jpeg\r\n"
                "Content-Length: %zu\r\n"
                "Cache-Control: no-cache, no-store, must-revalidate\r\n"
                "Pragma: no-cache\r\n"
                "Expires: 0\r\n"
                "\r\n",
                size);
            send(client_socket, header, strlen(header), 0);
            return 0;
        } else {
            /* GET request - return full snapshot */
            return handle_http_snapshot(client_socket);
        }
    }
    
    /* Unknown HTTP request - return 404 */
    const char *not_found = 
        "HTTP/1.0 404 Not Found\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 13\r\n"
        "\r\n"
        "Not Found";
    send(client_socket, not_found, strlen(not_found), 0);
    return -1;
}

static void* handle_client_thread(void* arg) {
    client_data_t* data = (client_data_t*)arg;
    int client_socket = data->socket;
    struct sockaddr_in client_addr = data->addr;
    char buffer[4096];
    int bytes_read;


    while ((bytes_read = recv(client_socket, buffer, sizeof(buffer) - 1, 0)) > 0) {
        buffer[bytes_read] = '\0';
        
        /* Check if this is HTTP request (starts with GET, POST, etc.) */
        if (strncmp(buffer, "GET ", 4) == 0 || 
            strncmp(buffer, "POST ", 5) == 0 ||
            strncmp(buffer, "HEAD ", 5) == 0) {
            /* This is HTTP request */
            if (handle_http_request(client_socket, buffer) == 0) {
                /* HTTP request handled successfully */
                break;
            }
            /* HTTP request failed or not found - close connection */
            break;
        }
        
        /* Check if this is interleaved binary data (RTP/RTCP over TCP) */
        /* RTSP over TCP uses '$' prefix for interleaved binary data */
        if (buffer[0] == '$' && bytes_read >= 4) {
            /* Interleaved binary data: $ + channel + length(2) + data */
            /* For RTP over TCP, we send these packets, but don't need to receive them */
            /* Skip this packet - it's interleaved RTP/RTCP data */
            unsigned char channel = buffer[1];
            unsigned int length = ((unsigned char)buffer[2] << 8) | (unsigned char)buffer[3];
            /* If we received partial packet, read remaining bytes */
            if (bytes_read < 4 + length) {
                size_t remaining = 4 + length - bytes_read;
                char *remaining_buf = buffer + bytes_read;
                while (remaining > 0) {
                    int n = recv(client_socket, remaining_buf, remaining, 0);
                    if (n <= 0) break;
                    remaining -= n;
                    remaining_buf += n;
                }
            }
            continue; /* Skip binary data - it's not RTSP request */
        }
        
        /* This is RTSP text request */
        buffer[bytes_read] = '\0';
        
        handle_rtsp_request(client_socket, client_addr, buffer, input_number);
    }
    
    if (bytes_read < 0) {
        OPRINT("Error receiving data from client: %s", strerror(errno));
    } else {
        OPRINT("Client disconnected (socket %d)", client_socket);
    }
    
    /* Cleanup client */
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].socket == client_socket) {
            clients[i].socket = 0;
            clients[i].rtp_port = 0;
            clients[i].rtcp_port = 0;
            clients[i].active = 0;
            clients[i].playing = 0;
            clients[i].timestamp = 0;
            clients[i].sequence_number = 0;
            memset(&clients[i].addr, 0, sizeof(clients[i].addr));
            OPRINT(" o: Client %d cleaned up (socket %d)\n", i, client_socket);
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    close(client_socket);
    free(data);
    return NULL;
}

/******************************************************************************
Description.: RTSP server thread
Input Value.: unused
Return Value: NULL
******************************************************************************/
void *rtsp_server_thread(void *arg)
{
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_socket;
    char buffer[4096];
    int bytes_read;
    
    OPRINT("RTSP server thread started\n");
    
    while (server_running && !pglobal->stop) {
        client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &client_len);
        if (client_socket < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (server_running) {
                OPRINT("Accept failed: %s (errno: %d)\n", strerror(errno), errno);
            }
            break;
        }
        
        /* Set TCP_NODELAY to disable Nagle algorithm - send packets immediately */
        /* This is critical for RTSP streaming to reduce startup delay */
        int flag = 1;
        if (setsockopt(client_socket, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) < 0) {
            /* Failed to set TCP_NODELAY - non-critical, continue */
        }
        /* Increase send buffer size for VLC - helps with stable streaming */
        /* VLC may need larger buffer to handle packet bursts */
        int send_buf_size = 256 * 1024; /* 256KB send buffer */
        if (setsockopt(client_socket, SOL_SOCKET, SO_SNDBUF, &send_buf_size, sizeof(send_buf_size)) < 0) {
            /* Failed to set SO_SNDBUF - non-critical, continue */
        }
        
        OPRINT("RTSP client connected from %s:%d\n", 
               inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
        
        /* Handle each client in a separate thread */
        pthread_t client_thread;
        client_data_t *client_data = malloc(sizeof(client_data_t));
        if (!client_data) {
            OPRINT("Failed to allocate client data\n");
            close(client_socket);
            continue;
        }
        
        client_data->socket = client_socket;
        client_data->addr = client_addr;
        
        
        if (pthread_create(&client_thread, NULL, handle_client_thread, client_data) != 0) {
            free(client_data);
            close(client_socket);
            continue;
        }
        
        pthread_detach(client_thread);
    }
    
    OPRINT("RTSP server thread stopped\n");
    return NULL;
}

/******************************************************************************
Description.: Stream worker thread - sends frames to all active clients
Input Value.: unused
Return Value: NULL
******************************************************************************/
void *stream_worker_thread(void *arg)
{
    int frame_size = 0;
    unsigned char *current_frame = NULL;
    static unsigned int last_sequence = UINT_MAX;
    static struct timeval last_tv = {0};
    
    OPRINT("RTSP stream worker started\n");
    
    static unsigned int last_rtsp_sequence = UINT_MAX;
    
    while (!pglobal->stop && server_running) {
        /* Wait for fresh frame or check current frame immediately after PLAY */
        /* For new clients after PLAY, we want to send current frame immediately */
        pthread_mutex_lock(&pglobal->in[input_number].db);
        
        /* Check if current frame is new (not processed yet) */
        unsigned int current_seq = pglobal->in[input_number].frame_sequence;
        int is_new_frame = (current_seq != last_rtsp_sequence) && 
                           (pglobal->in[input_number].size > 0);
        
        if (!is_new_frame) {
            /* No new frame yet - wait for fresh frame using helper */
        pthread_mutex_unlock(&pglobal->in[input_number].db);
            if (!wait_for_fresh_frame(&pglobal->in[input_number], &last_rtsp_sequence)) {
                /* wait_for_fresh_frame returns 0 only on error; mutex is unlocked */
                usleep(1000); /* Small delay to prevent busy waiting */
                continue;
            }
            /* mutex is locked by wait_for_fresh_frame */
        } else {
            /* Current frame is new - process it immediately (mutex already locked) */
            last_rtsp_sequence = current_seq;
        }
        
        frame_size = pglobal->in[input_number].size;
        
        /* Allocate buffer if needed */
        if (use_static_buffers && frame_size <= MAX_FRAME_SIZE) {
            current_frame = static_frame_buffer;
        } else if (current_frame == NULL || frame_size > MAX_FRAME_SIZE) {
            if (current_frame && current_frame != static_frame_buffer) free(current_frame);
            current_frame = malloc(frame_size);
            if (!current_frame) {
                OPRINT("Failed to allocate frame buffer\n");
                pthread_mutex_unlock(&pglobal->in[input_number].db);
                break;
            }
        }
        
        /* Copy frame data while mutex is locked */
        if (current_frame && frame_size > 0 && pglobal->in[input_number].buf != NULL) {
            simd_memcpy(current_frame, pglobal->in[input_number].buf, frame_size);
            
            /* Update snapshot buffer for HTTP /snapshot endpoint */
            pthread_mutex_lock(&snapshot_mutex);
            if (snapshot_buffer == NULL || snapshot_size < frame_size) {
                if (snapshot_buffer) free(snapshot_buffer);
                snapshot_buffer = malloc(frame_size);
                if (snapshot_buffer) {
                    snapshot_size = frame_size;
                }
            }
            if (snapshot_buffer && snapshot_size >= frame_size) {
                simd_memcpy(snapshot_buffer, current_frame, frame_size);
                snapshot_size = frame_size;
            }
            pthread_mutex_unlock(&snapshot_mutex);
        }
        
        /* Allow others to access the global buffer again - like HTTP plugin */
        pthread_mutex_unlock(&pglobal->in[input_number].db);
        

        pthread_mutex_lock(&clients_mutex);
        int playing_clients = 0;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            int is_tcp_client = (clients[i].socket > 0 && clients[i].rtp_port == 0);
            int is_udp_client = (clients[i].socket > 0 && clients[i].rtp_port > 0 && clients[i].addr.sin_addr.s_addr != 0);
            if (clients[i].active && clients[i].playing && (is_tcp_client || is_udp_client)) {
                playing_clients++;
            }
        }
        pthread_mutex_unlock(&clients_mutex);
        
        if (playing_clients == 0) {
            if (current_frame && current_frame != static_frame_buffer) {
                free(current_frame);
                current_frame = NULL;
            }
            continue;
        }
        
        /* Use fixed timestamp increment from input plugin FPS - calculate once */
        static int cached_fps = 0;
        if (cached_fps == 0) {
            /* Calculate from input plugin FPS */
            int input_fps = 30; /* default */
            if (pglobal && input_number >= 0 && input_number < pglobal->incnt && pglobal->in[input_number].fps > 0) {
                input_fps = pglobal->in[input_number].fps;
            }
            cached_fps = input_fps;
            rtp_ts_increment = (uint32_t)(90000 / input_fps); /* 90kHz / fps */
        }

        rtp_jpeg_frame_t prepared_frame;
        if (prepare_rtp_jpeg_frame(current_frame, frame_size, &prepared_frame) != 0) {
            OPRINT("[RTP ERROR] failed to prepare JPEG for RTP, dropping frame\n");
            if (current_frame && current_frame != static_frame_buffer) {
                free(current_frame);
                current_frame = NULL;
            }
            continue;
        }

        /* Send to all active clients - check playing state again after frame is copied */
        pthread_mutex_lock(&clients_mutex);
        int clients_count = 0;
        /* Re-count playing clients after frame is copied to ensure we have latest state */
        playing_clients = 0;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            int is_tcp_client = (clients[i].socket > 0 && clients[i].rtp_port == 0);
            int is_udp_client = (clients[i].socket > 0 && clients[i].rtp_port > 0 && clients[i].addr.sin_addr.s_addr != 0);
            if (clients[i].active && clients[i].playing && (is_tcp_client || is_udp_client)) {
                playing_clients++;
            }
        }
        uint32_t base_timestamp = 0;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].active && clients[i].playing && clients[i].timestamp > 0) {
                base_timestamp = clients[i].timestamp;
                break;
            }
        }
        if (base_timestamp == 0 && rtp_ts_increment > 0) {
            base_timestamp = rtp_ts_increment;
        }

        if (prepared_frame.width > 0 && prepared_frame.height > 0) {
            if (!sdp_dimensions_cached ||
                (prepared_frame.width != cached_sdp_width || prepared_frame.height != cached_sdp_height)) {
                cached_sdp_width = prepared_frame.width;
                cached_sdp_height = prepared_frame.height;
                sdp_dimensions_cached = 1;
            }
        }

        for (int i = 0; i < MAX_CLIENTS; i++) {
            /* Check if client is active, playing and properly initialized */
            /* For TCP: socket > 0 and rtp_port == 0 */
            /* For UDP: socket > 0, addr != 0, and rtp_port > 0 */
            int is_tcp_client = (clients[i].socket > 0 && clients[i].rtp_port == 0);
            int is_udp_client = (clients[i].socket > 0 && clients[i].rtp_port > 0 && clients[i].addr.sin_addr.s_addr != 0);
            if (clients[i].active && clients[i].playing && (is_tcp_client || is_udp_client) && prepared_frame.rtp_payload != NULL && prepared_frame.rtp_payload_size > 0) {
                if (clients[i].timestamp == 0) {
                    if (base_timestamp > 0) {
                        clients[i].timestamp = base_timestamp;
                    } else if (rtp_ts_increment > 0) {
                        clients[i].timestamp = rtp_ts_increment;
                    }
                }
                clients_count++;
                int send_result = send_rtp_packet(rtp_socket, &clients[i], &prepared_frame, clients[i].timestamp);
                if (send_result < 0) {
                    OPRINT("[RTP ERROR] Failed to send RTP packet to client %d (socket %d, active=%d, playing=%d)\n", 
                           i, clients[i].socket, clients[i].active, clients[i].playing);
                    /* Mark client as inactive if send fails - socket may be closed */
                    if (errno == EPIPE || errno == ECONNRESET || errno == EBADF) {
                        clients[i].active = 0;
                        clients[i].playing = 0;
                    }
                }
            }
        }
        if (clients_count > 0) {
            for (int i = 0; i < MAX_CLIENTS; i++) {
                int is_tcp_client = (clients[i].socket > 0 && clients[i].rtp_port == 0);
                int is_udp_client = (clients[i].socket > 0 && clients[i].rtp_port > 0 && clients[i].addr.sin_addr.s_addr != 0);
                if (clients[i].active && clients[i].playing && (is_tcp_client || is_udp_client)) {
                    clients[i].timestamp += rtp_ts_increment;
                }
            }
        }
        pthread_mutex_unlock(&clients_mutex);

        free_rtp_jpeg_frame(&prepared_frame);
        
        /* Free current_frame if it was allocated dynamically (not static buffer) */
        /* Note: We keep current_frame allocated between frames for performance */
        /* Only free if frame size exceeds MAX_FRAME_SIZE and we allocated dynamically */
        if (current_frame && current_frame != static_frame_buffer && frame_size > MAX_FRAME_SIZE) {
            free(current_frame);
            current_frame = NULL;
        }
    }
    
    /* Free current_frame on thread exit */
    if (current_frame && current_frame != static_frame_buffer) {
        free(current_frame);
    }
    OPRINT("RTSP stream worker stopped\n");
    return NULL;
}

/******************************************************************************
Description.: Initialize output plugin
Input Value.: plugin number, input number, output parameters
Return Value: 0 on success, -1 on error
******************************************************************************/
int output_init(output_parameter *param, int id)
{
    int port = 554;
    
    /* Initialize global variables */
    pglobal = param->global;
    input_number = param->id;
    
    /* Parse parameters */
    /* Parameters are already parsed by mjpg_streamer main */
    if (param->argv && param->argc > 0) {
        for (int i = 0; i < param->argc; i++) {
            if (param->argv[i] && (!strcmp(param->argv[i], "-h") || !strcmp(param->argv[i], "--help"))) {
                OPRINT("RTSP output plugin options:\n");
                OPRINT("  -i, --input <num>   Input channel index (default from core)\n");
                OPRINT("  -p, --port <num>    RTSP server port (default 554)\n");
                return -1;
            } else if (param->argv[i] && (!strcmp(param->argv[i], "-i") || !strcmp(param->argv[i], "--input"))) {
                if (i + 1 < param->argc && param->argv[i + 1]) {
                    int req_input = atoi(param->argv[i + 1]);
                    if (req_input >= 0 && req_input < pglobal->incnt) {
                        input_number = req_input;
                    }
                    i++;
                }
            } else if (param->argv[i] && (!strcmp(param->argv[i], "-p") || !strcmp(param->argv[i], "--port"))) {
                if (i + 1 < param->argc && param->argv[i + 1]) {
                    port = atoi(param->argv[i + 1]);
                    i++;
                }
            }
        }
    }
    
    OPRINT("RTSP server will use port: %d\n", port);
    
    /* Validate input plugin */
    if (input_number >= pglobal->incnt) {
        OPRINT("ERROR: input plugin %d not available (only %d loaded)\n", 
               input_number, pglobal->incnt);
        return -1;
    }
    
    /* Create server socket */
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        OPRINT("Failed to create server socket: %s\n", strerror(errno));
        return -1;
    }
    
    /* Set socket options */
    int opt = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    /* Bind to port */
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);
    
    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        OPRINT("Failed to bind to port %d: %s\n", port, strerror(errno));
        close(server_socket);
        return -1;
    }
    
    /* Listen for connections */
    if (listen(server_socket, MAX_CLIENTS) < 0) {
        OPRINT("Failed to listen on port %d: %s\n", port, strerror(errno));
        close(server_socket);
        return -1;
    }
    
    /* Create RTP socket */
    rtp_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (rtp_socket < 0) {
        OPRINT("Failed to create RTP socket: %s\n", strerror(errno));
        close(server_socket);
        return -1;
    }
    
    /* Initialize clients */
    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].active = 0;
        clients[i].socket = -1;
        clients[i].rtp_port = 0;
        clients[i].rtcp_port = 0;
        clients[i].sequence_number = 0;
        clients[i].timestamp = 0;
        memset(&clients[i].addr, 0, sizeof(clients[i].addr));
    }
    
    OPRINT("RTSP server initialized on port %d\n", port);
    OPRINT("Input plugin: %d\n", input_number);

    /* DEBUG: Log server socket status */
    OPRINT("[RTSP DEBUG] Server socket created: fd=%d\n", server_socket);

    return 0;
}

/******************************************************************************
Description.: Stop output plugin
Input Value.: plugin number
Return Value: 0 on success, -1 on error
******************************************************************************/
int output_stop(int id)
{
    server_running = 0;
    
    /* Close all client connections */
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active) {
            close(clients[i].socket);
            clients[i].active = 0;
            clients[i].socket = -1;
            clients[i].rtp_port = 0;
            clients[i].rtcp_port = 0;
            memset(&clients[i].addr, 0, sizeof(clients[i].addr));
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    
    /* Join threads */
        pthread_join(server_thread, NULL);
        pthread_join(stream_thread, NULL);
    
    /* Close server socket */
    if (server_socket >= 0) {
        close(server_socket);
        server_socket = -1;
    }
    
    /* Close RTP socket */
    if (rtp_socket >= 0) {
        close(rtp_socket);
        rtp_socket = -1;
    }
    cleanup_turbojpeg_handles();
    
    /* Free snapshot buffer */
    pthread_mutex_lock(&snapshot_mutex);
    if (snapshot_buffer) {
        free(snapshot_buffer);
        snapshot_buffer = NULL;
        snapshot_size = 0;
    }
    pthread_mutex_unlock(&snapshot_mutex);
    
    OPRINT("RTSP server stopped\n");
    return 0;
}

/******************************************************************************
Description.: Run output plugin
Input Value.: plugin number
Return Value: 0 on success, -1 on error
******************************************************************************/
int output_run(int id)
{
    server_running = 1;
    
    /* Start server thread */
    if (pthread_create(&server_thread, NULL, rtsp_server_thread, NULL) != 0) {
        OPRINT("Failed to create server thread: %s\n", strerror(errno));
        server_running = 0;
        return -1;
    }
    
    /* Start stream worker thread */
    if (pthread_create(&stream_thread, NULL, stream_worker_thread, NULL) != 0) {
        OPRINT("Failed to create stream worker thread: %s\n", strerror(errno));
        server_running = 0;
        pthread_join(server_thread, NULL);
        return -1;
    }
    
    OPRINT("RTSP server started\n");
    return 0;
}

/******************************************************************************
Description.: Output plugin command
Input Value.: plugin number, command, parameter
Return Value: 0 on success, -1 on error
******************************************************************************/
int output_cmd(int id, unsigned int control_id, unsigned int group, int value, char *value_str)
{
    return 0;
}

/******************************************************************************
Description.: Output plugin structure
******************************************************************************/
static output output_rtsp = {
    .plugin = "output_rtsp",
    .name = "RTSP Server",
    .init = output_init,
    .stop = output_stop,
    .run = output_run,
    .cmd = output_cmd
};

/******************************************************************************
Description.: Plugin interface functions
******************************************************************************/
output *output_init_plugin(void)
{
    return &output_rtsp;
}

void output_cleanup_plugin(void)
{
    // Nothing to cleanup
}