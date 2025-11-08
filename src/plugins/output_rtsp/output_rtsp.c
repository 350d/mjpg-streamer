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

/* RTSP response templates */
#define RTSP_SERVER_NAME "MJPG-Streamer RTSP Server"
#define RTSP_VERSION "RTSP/1.0"

/* GStreamer style: QT tables are used in natural order (as in DQT), no zigzag conversion */

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
static uint32_t rtp_ts_increment = 3000;
static int cached_sdp_width = 640;
static int cached_sdp_height = 480;
static int sdp_dimensions_cached = 0;
static unsigned char static_frame_buffer[MAX_FRAME_SIZE];
static int use_static_buffers = 1;
static unsigned char *snapshot_buffer = NULL;
static size_t snapshot_size = 0;
static pthread_mutex_t snapshot_mutex = PTHREAD_MUTEX_INITIALIZER;


typedef struct {
    unsigned char *rtp_payload;
    size_t rtp_payload_size;
    int width;
    int height;
    int subsamp;
    int jpeg_type;
    int is_rtp_format;
    uint8_t qt_luma[64];
    uint8_t qt_chroma[64];
    int have_luma;
    int have_chroma;
    int qt_precision;
} rtp_jpeg_frame_t;

static void free_rtp_jpeg_frame(rtp_jpeg_frame_t *frame);
static int prepare_rtp_jpeg_frame(const unsigned char *jpeg_data, size_t jpeg_size,
                                  rtp_jpeg_frame_t *frame_info);
static int send_rtsp_response(int client_socket, int cseq, int status_code, const char *status_text, 
                              const char *headers, const char *body);
static int find_client_by_socket(int client_socket);
static void clear_client(int client_idx);
static int is_valid_client(int client_idx);
static void build_session_header(char *headers, size_t headers_size, int session_id);
static void build_sdp_headers(char *headers, size_t headers_size, size_t sdp_len);
static void build_http_headers(char *headers, size_t headers_size, int status_code, 
                              const char *status_text, const char *content_type, size_t content_length);
static int send_http_error(int client_socket, int status_code, const char *status_text, 
                          const char *content_type, const char *error_body);
static void handle_rtsp_options(int client_socket, int cseq);
static void handle_rtsp_describe(int client_socket, int cseq, struct sockaddr_in client_addr, int input_number);
static void handle_rtsp_setup(int client_socket, int cseq, struct sockaddr_in client_addr, char *request);
static void handle_rtsp_play(int client_socket, int cseq, int session_id, int input_number);
static void handle_rtsp_pause(int client_socket, int cseq, int session_id);
static void handle_rtsp_teardown(int client_socket, int cseq, int session_id);

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
    

    int input_width = 0;
    int input_height = 0;
    int input_subsamp = -1;
    if (turbojpeg_header_info(jpeg_data, (int)jpeg_size, &input_width, &input_height, &input_subsamp) != 0) {
        OPRINT("[RTP ERROR] turbojpeg_header_info failed during preparation\n");
        return -1;
    }

    size_t eoi_pos = jpeg_size;
    size_t soi_pos = 0;
    if (jpeg_size < 2 || jpeg_data[0] != 0xFF || jpeg_data[1] != 0xD8) {
        OPRINT("[RTP ERROR] JPEG does not start with SOI (0xFF 0xD8)\n");
        return -1;
    }
    soi_pos = 0;
    
    for (size_t i = soi_pos + 2; i < jpeg_size; ) {
        if (jpeg_data[i-1] == 0xFF) {
            if (jpeg_data[i] == 0xD9) {
                eoi_pos = i + 1;
                break;
            } else if (jpeg_data[i] == 0x00) {
                i += 2;
                continue;
            } else {
                i++;
            }
        } else {
            i++;
        }
    }
    
    if (eoi_pos < jpeg_size) {
        if (eoi_pos + 1 < jpeg_size && jpeg_data[eoi_pos] == 0xFF) {
            unsigned char next_byte = jpeg_data[eoi_pos + 1];
            if (next_byte == 0xD9 || next_byte == 0xD8) {
            }
        }
    }
    
    if (eoi_pos < 2 || eoi_pos > jpeg_size) {
        OPRINT("[RTP ERROR] Invalid EOI position: eoi_pos=%zu, jpeg_size=%zu\n", eoi_pos, jpeg_size);
        return -1;
    }
    if (jpeg_data[eoi_pos - 2] != 0xFF || jpeg_data[eoi_pos - 1] != 0xD9) {
        OPRINT("[RTP ERROR] EOI marker not found at expected position: eoi_pos=%zu\n", eoi_pos);
        return -1;
    }
    
    size_t check_pos = eoi_pos;
    while (check_pos < jpeg_size && (jpeg_data[check_pos] == 0x00 || jpeg_data[check_pos] == 0xFF)) {
        check_pos++;
    }
    if (check_pos < jpeg_size) {
    }
    
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

    size_t scan_len = (eoi_pos - 2) - scan_start;
    if (scan_len == 0) {
        OPRINT("[RTP ERROR] Empty scan segment\n");
        return -1;
    }
    
    if (scan_len >= 2 && jpeg_data[scan_start + scan_len - 2] == 0xFF && jpeg_data[scan_start + scan_len - 1] == 0xD9) {
        OPRINT("[RTP ERROR] Scan buffer ends with EOI unexpectedly\n");
        return -1;
    }
    
    unsigned char *scan_only = (unsigned char *)malloc(scan_len);
    if (!scan_only) {
        OPRINT("[RTP ERROR] Failed to allocate scan buffer (%zu bytes)\n", scan_len);
        return -1;
    }
    simd_memcpy(scan_only, jpeg_data + scan_start, scan_len);

    frame_info->rtp_payload = scan_only;
    frame_info->rtp_payload_size = scan_len;
    frame_info->is_rtp_format = 1;
    frame_info->width = input_width;
    frame_info->height = input_height;
    frame_info->subsamp = input_subsamp;

               switch (frame_info->subsamp) {
                   case TJSAMP_422: frame_info->jpeg_type = 0; break;
                   case TJSAMP_420: frame_info->jpeg_type = 1; break;
                   case TJSAMP_411: frame_info->jpeg_type = 2; break;
                   case TJSAMP_444: frame_info->jpeg_type = 3; break;
                   case TJSAMP_440: 
                       OPRINT("[RTP WARNING] TJSAMP_440 (4:4:0) not supported by RFC 2435, mapping to Type 1 (4:2:0)\n");
                       frame_info->jpeg_type = 1; 
                       break;
                   case TJSAMP_GRAY:
                       OPRINT("[RTP WARNING] TJSAMP_GRAY (grayscale) - using Type 3 (4:4:4) temporarily\n");
                       frame_info->jpeg_type = 3;
                       break;
                   default:
                       OPRINT("[RTP WARNING] Unknown subsampling %d, defaulting to Type 0 (4:2:2)\n", frame_info->subsamp);
                       frame_info->jpeg_type = 0;
                       break;
               }


    rtpjpeg_cache_qtables_from_jpeg(jpeg_data, eoi_pos);
    const uint8_t *qt_luma_ptr = NULL, *qt_chroma_ptr = NULL;
    int have_luma = 0, have_chroma = 0, qt_precision = 0;
    if (rtpjpeg_get_cached_qtables(&qt_luma_ptr, &qt_chroma_ptr, &have_luma, &have_chroma, &qt_precision) == 0) {
        frame_info->have_luma = have_luma;
        frame_info->have_chroma = have_chroma;
        frame_info->qt_precision = qt_precision;
        if (have_luma && qt_luma_ptr) {
            simd_memcpy(frame_info->qt_luma, qt_luma_ptr, 64);
        }
        if (have_chroma && qt_chroma_ptr) {
            simd_memcpy(frame_info->qt_chroma, qt_chroma_ptr, 64);
        }
    } else {
        frame_info->have_luma = 0;
        frame_info->have_chroma = 0;
        frame_info->qt_precision = 0;
    }

    return 0;
}

/******************************************************************************
Description.: Send RTSP response
Input Value.: client socket, cseq, status code, status text, headers, body
Return Value: 0 on success, -1 on error
******************************************************************************/
static int send_rtsp_response(int client_socket, int cseq, int status_code, const char *status_text, 
                              const char *headers, const char *body)
{
    char response[1024];
    int len = snprintf(response, sizeof(response),
                      RTSP_VERSION " %d %s\r\n"
                      "CSeq: %d\r\n"
                      "%s"
                      "Server: " RTSP_SERVER_NAME "\r\n"
                      "\r\n",
                      status_code, status_text, cseq, headers ? headers : "");
    if (body && len < (int)sizeof(response) - 1) {
        len += snprintf(response + len, sizeof(response) - len, "%s", body);
    }
    return send(client_socket, response, len, 0) == len ? 0 : -1;
}

/******************************************************************************
Description.: Find client by socket
Input Value.: client socket
Return Value: client index or -1 if not found
******************************************************************************/
static int find_client_by_socket(int client_socket)
{
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && clients[i].socket == client_socket) {
            pthread_mutex_unlock(&clients_mutex);
            return i;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    return -1;
}

/******************************************************************************
Description.: Clear client structure
Input Value.: client index
Return Value: none
******************************************************************************/
static void clear_client(int client_idx)
{
    if (client_idx < 0 || client_idx >= MAX_CLIENTS) {
        return;
    }
    clients[client_idx].active = 0;
    clients[client_idx].playing = 0;
    clients[client_idx].socket = 0;
    clients[client_idx].rtp_port = 0;
    clients[client_idx].rtcp_port = 0;
    clients[client_idx].sequence_number = 0;
    clients[client_idx].timestamp = 0;
    memset(&clients[client_idx].addr, 0, sizeof(clients[client_idx].addr));
}

/******************************************************************************
Description.: Check if client is valid (active, playing, and has valid transport)
Input Value.: client index
Return Value: 1 if valid, 0 otherwise
******************************************************************************/
static int is_valid_client(int client_idx)
{
    if (client_idx < 0 || client_idx >= MAX_CLIENTS) {
        return 0;
    }
    if (!clients[client_idx].active || !clients[client_idx].playing) {
        return 0;
    }
    int is_tcp_client = (clients[client_idx].socket > 0 && clients[client_idx].rtp_port == 0);
    int is_udp_client = (clients[client_idx].socket > 0 && clients[client_idx].rtp_port > 0 && 
                         clients[client_idx].addr.sin_addr.s_addr != 0);
    return (is_tcp_client || is_udp_client) ? 1 : 0;
}

/******************************************************************************
Description.: Build Session header for RTSP response
Input Value.: headers buffer, buffer size, session ID
Return Value: none
******************************************************************************/
static void build_session_header(char *headers, size_t headers_size, int session_id)
{
    snprintf(headers, headers_size, "Session: %d\r\n", session_id);
}

/******************************************************************************
Description.: Build SDP headers for RTSP DESCRIBE response
Input Value.: headers buffer, buffer size, SDP length
Return Value: none
******************************************************************************/
static void build_sdp_headers(char *headers, size_t headers_size, size_t sdp_len)
{
    snprintf(headers, headers_size,
             "Content-Type: application/sdp\r\n"
             "Content-Length: %zu\r\n",
             sdp_len);
}

/******************************************************************************
Description.: Build HTTP headers for HTTP response
Input Value.: headers buffer, buffer size, status code, status text, content type, content length
Return Value: none
******************************************************************************/
static void build_http_headers(char *headers, size_t headers_size, int status_code, 
                              const char *status_text, const char *content_type, size_t content_length)
{
    if (content_type && strcmp(content_type, "image/jpeg") == 0) {
        snprintf(headers, headers_size,
                 "HTTP/1.0 %d %s\r\n"
                 "Content-Type: %s\r\n"
                 "Content-Length: %zu\r\n"
                 "Cache-Control: no-cache, no-store, must-revalidate\r\n"
                 "Pragma: no-cache\r\n"
                 "Expires: 0\r\n"
                 "\r\n",
                 status_code, status_text, content_type, content_length);
    } else {
        snprintf(headers, headers_size,
                 "HTTP/1.0 %d %s\r\n"
                 "Content-Type: %s\r\n"
                 "Content-Length: %zu\r\n"
                 "\r\n",
                 status_code, status_text, content_type ? content_type : "text/plain", content_length);
    }
}

/******************************************************************************
Description.: Send HTTP error response
Input Value.: client socket, status code, status text, content type, error body
Return Value: 0 on success, -1 on error
******************************************************************************/
static int send_http_error(int client_socket, int status_code, const char *status_text, 
                          const char *content_type, const char *error_body)
{
    size_t error_body_len = strlen(error_body);
    char header[512];
    build_http_headers(header, sizeof(header), status_code, status_text, 
                      content_type ? content_type : "text/plain", error_body_len);
    if (send(client_socket, header, strlen(header), 0) < 0) {
        return -1;
    }
    if (send(client_socket, error_body, error_body_len, 0) < 0) {
        return -1;
    }
    return 0;
}

/******************************************************************************
Description.: Send RTP packet with RFC 2435 compliant JPEG payload
Input Value.: client, prepared frame info, timestamp
Return Value: 0 on success, -1 on error
******************************************************************************/
static int send_rtp_packet(int rtp_socket, rtsp_client_t *client, const rtp_jpeg_frame_t *frame,
                           uint32_t frame_timestamp, int client_idx)
{
    const int qt_insertion_enabled = 1;
    const int have_both = frame->have_luma && frame->have_chroma;
    const int q255_ok = (frame->qt_precision == 0) && have_both;
    int q_value_fixed = q255_ok ? 255 : 75;

    if (!client || !frame || !frame->rtp_payload || frame->rtp_payload_size <= 0) {
        OPRINT("[RTP ERROR] invalid frame data for transmission\n");
        return -1;
    }

    if (frame->width <= 0 || frame->height <= 0) {
        OPRINT("[RTP ERROR] invalid frame dimensions (%d x %d)\n", frame->width, frame->height);
        return -1;
    }

    const unsigned char *jpeg_data = frame->rtp_payload;
    size_t jpeg_size = frame->rtp_payload_size;
    size_t fragment_offset = 0;
    size_t remaining = jpeg_size;
    uint16_t seq = client->sequence_number;
    int is_tcp = (client->rtp_port == 0);
    size_t max_packet_size = is_tcp ? MAX_TCP_PACKET_SIZE : MAX_RTP_PACKET_SIZE;
    const uint8_t *qt_luma = frame->have_luma ? frame->qt_luma : NULL;
    const uint8_t *qt_chroma = frame->have_chroma ? frame->qt_chroma : NULL;
    int have_luma = frame->have_luma;
    int have_chroma = frame->have_chroma;
    int qt_precision = frame->qt_precision;

    int frame_width_div8 = (frame->width + 7) / 8;
    int frame_height_div8 = (frame->height + 7) / 8;
    if (frame_width_div8 <= 0 || frame_height_div8 <= 0 || frame_width_div8 > 255 || frame_height_div8 > 255) {
        OPRINT("[RTP ERROR] frame dimensions not divisible by 8 or too large (%d x %d, div8: %d x %d)\n", 
               frame->width, frame->height, frame_width_div8, frame_height_div8);
        return -1;
    }

    const unsigned char jpeg_type_fixed = (unsigned char)frame->jpeg_type;

    while (remaining > 0) {
        size_t total_header_size = 20;
        if (total_header_size >= max_packet_size) {
            OPRINT("[RTP ERROR] header size %zu exceeds packet size limit %zu\n", total_header_size, max_packet_size);
            return -1;
        }

        size_t max_payload = max_packet_size - total_header_size;
        if (max_payload == 0) {
            OPRINT("[RTP ERROR] no room for payload after headers\n");
            return -1;
        }

        size_t qt_hdr_len = 0;
        if (qt_insertion_enabled && fragment_offset == 0 && q_value_fixed == 255) {
            qt_hdr_len = 4 + 128;
        }

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
        
        packet[12] = 0;
        packet[13] = (fragment_offset >> 16) & 0xFF;
        packet[14] = (fragment_offset >> 8) & 0xFF;
        packet[15] = fragment_offset & 0xFF;
        packet[16] = jpeg_type_fixed;
        packet[17] = (unsigned char)q_value_fixed;
        packet[18] = (unsigned char)frame_width_div8;
        packet[19] = (unsigned char)frame_height_div8;
        
        size_t payload_offset = 20;
        
        if (qt_insertion_enabled && fragment_offset == 0 && q_value_fixed == 255) {
            if (payload_offset + qt_hdr_len > max_packet_size - 12) {
                OPRINT("[RTP WARNING] QT header too large for packet: qt_hdr_len=%zu, max_packet_size=%zu, falling back to Q=75\n",
                       qt_hdr_len, max_packet_size);
                q_value_fixed = 75;
                qt_hdr_len = 0;
                packet[17] = (unsigned char)q_value_fixed;
            }
        }
        
        if (qt_insertion_enabled && fragment_offset == 0 && q_value_fixed == 255) {
            int found_zero = 0;
            for (int i = 0; i < 64; i++) {
                if (!frame->qt_luma[i] || !frame->qt_chroma[i]) {
                    found_zero = 1;
                    OPRINT("[RTP WARNING] Found zero in QT table (luma[%d]=%d, chroma[%d]=%d), falling back to Q=75\n",
                           i, frame->qt_luma[i], i, frame->qt_chroma[i]);
                    break;
                }
            }
            
            if (found_zero) {
                q_value_fixed = 75;
                qt_hdr_len = 0;
                packet[17] = (unsigned char)q_value_fixed;
                OPRINT("[RTP WARNING] QT disabled, fallback Q=75 - zero found in QT tables\n");
            } else {
                size_t qt_hdr_len_calc = 4 + 128;
                
                if (20 + qt_hdr_len_calc >= max_packet_size) {
                    q_value_fixed = 75;
                    qt_hdr_len = 0;
                    packet[17] = (unsigned char)q_value_fixed;
                    OPRINT("[RTP WARNING] QT disabled, fallback Q=75 - QT header too large\n");
                } else {
                    uint8_t qt_hdr[4 + 128];
                    size_t off = 0;
                    
                    qt_hdr[off++] = 0x00;
                    qt_hdr[off++] = 0x00;
                    uint16_t Lq = 128;
                    qt_hdr[off++] = (Lq >> 8) & 0xFF;
                    qt_hdr[off++] = Lq & 0xFF;
                    simd_memcpy(qt_hdr + off, frame->qt_luma, 64);
                    off += 64;
                    simd_memcpy(qt_hdr + off, frame->qt_chroma, 64);
                    off += 64;
                    
                    if (off != qt_hdr_len_calc) {
                        OPRINT("[RTP ERROR] QT header length mismatch: calculated=%zu, actual=%zu\n", qt_hdr_len_calc, off);
                        return -1;
                    }
                    qt_hdr_len = qt_hdr_len_calc;
                    
                    
                    /* Insert QT header into packet */
                    simd_memcpy(packet + payload_offset, qt_hdr, qt_hdr_len);
                }
            }
        }
        
        if (qt_insertion_enabled && fragment_offset == 0 && q_value_fixed == 255 && qt_hdr_len > 0) {
            simd_memcpy(packet + payload_offset + qt_hdr_len, jpeg_data + fragment_offset, payload_size);
        } else {
            simd_memcpy(packet + payload_offset, jpeg_data + fragment_offset, payload_size);
        }
        
        size_t packet_size = payload_offset + qt_hdr_len + payload_size;
        
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
            if (client_idx >= 0 && client_idx < MAX_CLIENTS && !tcp_nodelay_sockets_[client_idx]) {
                int opt = 1; setsockopt(client->socket, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
                tcp_nodelay_sockets_[client_idx] = 1;
            }
            unsigned char tcp_packet[4 + MAX_TCP_PACKET_SIZE];
            tcp_packet[0] = '$'; tcp_packet[1] = 0;
            tcp_packet[2] = (packet_size >> 8) & 0xFF; tcp_packet[3] = packet_size & 0xFF;
            simd_memcpy(tcp_packet + 4, packet, packet_size);
            size_t to_send = 4 + packet_size; const unsigned char *ptr = tcp_packet;
            
            while (to_send > 0) {
                sent = send(client->socket, ptr, to_send, MSG_NOSIGNAL | MSG_DONTWAIT);
                if (sent <= 0) {
                    if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                        if (errno == EPIPE || errno == ECONNRESET || errno == EBADF) {
                            OPRINT("Error sending RTP over TCP: %s (socket closed, packet_size=%zu)\n", strerror(errno), packet_size);
                            return -1;
                        }
                        OPRINT("Error sending RTP over TCP: %s (packet_size=%zu, sent=%d)\n", strerror(errno), packet_size, sent); 
                        return -1; 
                    }
                    if (errno == EAGAIN || errno == EWOULDBLOCK) { 
                        usleep(1000);
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
Description.: Handle RTSP OPTIONS request
Input Value.: client socket, CSeq
Return Value: none
******************************************************************************/
static void handle_rtsp_options(int client_socket, int cseq) {
    send_rtsp_response(client_socket, cseq, 200, "OK", 
                      "Public: OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN\r\n", NULL);
}

/******************************************************************************
Description.: Handle RTSP DESCRIBE request
Input Value.: client socket, CSeq, client address, input number
Return Value: none
******************************************************************************/
static void handle_rtsp_describe(int client_socket, int cseq, struct sockaddr_in client_addr, int input_number) {
    char sdp[512];
    int width = 640, height = 480;
    
    if (sdp_dimensions_cached && cached_sdp_width > 0 && cached_sdp_height > 0) {
        width = cached_sdp_width;
        height = cached_sdp_height;
    } else if (pglobal != NULL && input_number >= 0 && input_number < pglobal->incnt) {
        int current_width = pglobal->in[input_number].width;
        int current_height = pglobal->in[input_number].height;
        
        if (current_width > 0 && current_height > 0) {
            cached_sdp_width = current_width;
            cached_sdp_height = current_height;
            sdp_dimensions_cached = 1;
            width = cached_sdp_width;
            height = cached_sdp_height;
        }
    }
    
    int fps = 30;
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
    char headers[256];
    build_sdp_headers(headers, sizeof(headers), strlen(sdp));
    send_rtsp_response(client_socket, cseq, 200, "OK", headers, sdp);
}

/******************************************************************************
Description.: Handle RTSP SETUP request
Input Value.: client socket, CSeq, client address, request buffer
Return Value: none
******************************************************************************/
static void handle_rtsp_setup(int client_socket, int cseq, struct sockaddr_in client_addr, char *request) {
    int client_rtp_port = 0, client_rtcp_port = 0;
    int use_tcp = 0;
    int session_id = 123456;
    
    char *transport_line = strstr(request, "Transport:");
    if (transport_line) {
        if (strstr(transport_line, "RTP/AVP/TCP")) {
            use_tcp = 1;
        } else {
            char *client_port = strstr(transport_line, "client_port=");
            if (client_port) {
                sscanf(client_port, "client_port=%d-%d", &client_rtp_port, &client_rtcp_port);
                if (client_rtp_port > 0 && client_rtcp_port > 0) {
                    use_tcp = 0;
                }
            }
        }
    }
    
    int i;
    pthread_mutex_lock(&clients_mutex);
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].active && (clients[i].socket == 0 || clients[i].socket == -1)) {
            clients[i].active = 1;
            clients[i].socket = client_socket;
            clients[i].rtp_port = use_tcp ? 0 : client_rtp_port;
            clients[i].rtcp_port = use_tcp ? 0 : client_rtcp_port;
            clients[i].sequence_number = 0;
            clients[i].timestamp = 0;
            clients[i].addr = client_addr;
            clients[i].playing = 0;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    
    if (i < MAX_CLIENTS) {
        char headers[256];
        char session_hdr[64];
        build_session_header(session_hdr, sizeof(session_hdr), session_id);
        if (use_tcp) {
            snprintf(headers, sizeof(headers),
                    "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
                    "%s", session_hdr);
        } else {
            snprintf(headers, sizeof(headers),
                    "Transport: RTP/AVP;unicast;client_port=%d-%d;server_port=5004-5005;source=%s\r\n"
                    "%s",
                    client_rtp_port, client_rtcp_port, inet_ntoa(client_addr.sin_addr), session_hdr);
        }
        send_rtsp_response(client_socket, cseq, 200, "OK", headers, NULL);
    } else {
        send_rtsp_response(client_socket, cseq, 503, "Service Unavailable", NULL, NULL);
    }
}

/******************************************************************************
Description.: Handle RTSP PLAY request
Input Value.: client socket, CSeq, session ID, input number
Return Value: none
******************************************************************************/
static void handle_rtsp_play(int client_socket, int cseq, int session_id, int input_number) {
    int i = find_client_by_socket(client_socket);
    
    if (i >= 0) {
        pthread_mutex_lock(&clients_mutex);
        clients[i].playing = 1;
        pthread_mutex_unlock(&clients_mutex);
        OPRINT(" o: Client %d set to playing state (socket %d)\n", i, client_socket);
    } else {
        OPRINT("[RTSP ERROR] PLAY request but no matching client found for socket %d\n", client_socket);
    }
    
    char headers[128];
    build_session_header(headers, sizeof(headers), session_id);
    send_rtsp_response(client_socket, cseq, 200, "OK", headers, NULL);
    
    if (pglobal && i >= 0) {
        pthread_mutex_lock(&pglobal->in[input_number].db);
        pthread_cond_broadcast(&pglobal->in[input_number].db_update);
        pthread_mutex_unlock(&pglobal->in[input_number].db);
    }
}

/******************************************************************************
Description.: Handle RTSP PAUSE request
Input Value.: client socket, CSeq, session ID
Return Value: none
******************************************************************************/
static void handle_rtsp_pause(int client_socket, int cseq, int session_id) {
    int i = find_client_by_socket(client_socket);
    
    if (i >= 0) {
        pthread_mutex_lock(&clients_mutex);
        clients[i].playing = 0;
        pthread_mutex_unlock(&clients_mutex);
    }
    
    char headers[128];
    build_session_header(headers, sizeof(headers), session_id);
    send_rtsp_response(client_socket, cseq, 200, "OK", headers, NULL);
}

/******************************************************************************
Description.: Handle RTSP TEARDOWN request
Input Value.: client socket, CSeq, session ID
Return Value: none
******************************************************************************/
static void handle_rtsp_teardown(int client_socket, int cseq, int session_id) {
    int i = find_client_by_socket(client_socket);
    
    if (i >= 0) {
        pthread_mutex_lock(&clients_mutex);
        clear_client(i);
        pthread_mutex_unlock(&clients_mutex);
        OPRINT(" o: Client %d cleaned up on TEARDOWN (socket %d)\n", i, client_socket);
    }
    
    char headers[128];
    build_session_header(headers, sizeof(headers), session_id);
    send_rtsp_response(client_socket, cseq, 200, "OK", headers, NULL);
}

/******************************************************************************
Description.: Handle RTSP request
Input Value.: client socket, request buffer
Return Value: 0 on success, -1 on error
******************************************************************************/
static void handle_rtsp_request(int client_socket, struct sockaddr_in client_addr, char *request, int input_number) {
    int cseq = 0;
    const int session_id = 123456;
    char request_copy[4096];
    char *saveptr = NULL;
    
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

    while ((line = strtok_r(NULL, "\r\n", &saveptr))) {
        if (strncmp(line, "CSeq:", 5) == 0) {
            cseq = atoi(line + 6);
        }
    }
    
    if (strcmp(method, "OPTIONS") == 0) {
        handle_rtsp_options(client_socket, cseq);
    } else if (strcmp(method, "DESCRIBE") == 0) {
        handle_rtsp_describe(client_socket, cseq, client_addr, input_number);
    } else if (strcmp(method, "SETUP") == 0) {
        handle_rtsp_setup(client_socket, cseq, client_addr, request);
    } else if (strcmp(method, "PLAY") == 0) {
        handle_rtsp_play(client_socket, cseq, session_id, input_number);
    } else if (strcmp(method, "PAUSE") == 0) {
        handle_rtsp_pause(client_socket, cseq, session_id);
    } else if (strcmp(method, "TEARDOWN") == 0) {
        handle_rtsp_teardown(client_socket, cseq, session_id);
    } else {
        send_rtsp_response(client_socket, cseq, 400, "Bad Request", NULL, NULL);
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
        send_http_error(client_socket, 503, "Service Unavailable", "text/plain", "No frame available");
        return -1;
    }
    
    unsigned char *snapshot_copy = malloc(snapshot_size);
    if (!snapshot_copy) {
        pthread_mutex_unlock(&snapshot_mutex);
        send_http_error(client_socket, 500, "Internal Server Error", "text/plain", "Out of memory");
        return -1;
    }
    
    simd_memcpy(snapshot_copy, snapshot_buffer, snapshot_size);
    size_t snapshot_size_copy = snapshot_size;
    pthread_mutex_unlock(&snapshot_mutex);
    
    char header[512];
    build_http_headers(header, sizeof(header), 200, "OK", "image/jpeg", snapshot_size_copy);
    
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
                send_http_error(client_socket, 503, "Service Unavailable", "text/plain", "Service Unavailable");
                return -1;
            }
            size_t size = snapshot_size;
            pthread_mutex_unlock(&snapshot_mutex);
            
            char header[512];
            build_http_headers(header, sizeof(header), 200, "OK", "image/jpeg", size);
            send(client_socket, header, strlen(header), 0);
            return 0;
        } else {
            /* GET request - return full snapshot */
            return handle_http_snapshot(client_socket);
        }
    }
    
    /* Unknown HTTP request - return 404 */
    send_http_error(client_socket, 404, "Not Found", "text/plain", "Not Found");
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
            clear_client(i);
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
        
        int flag = 1;
        if (setsockopt(client_socket, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) < 0) {
        }
        int send_buf_size = 256 * 1024;
        if (setsockopt(client_socket, SOL_SOCKET, SO_SNDBUF, &send_buf_size, sizeof(send_buf_size)) < 0) {
        }
        
        OPRINT("RTSP client connected from %s:%d\n", 
               inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
        
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
    
    OPRINT("RTSP stream worker started\n");
    
    static unsigned int last_rtsp_sequence = UINT_MAX;
    
    while (!pglobal->stop && server_running) {
        pthread_mutex_lock(&pglobal->in[input_number].db);
        
        unsigned int current_seq = pglobal->in[input_number].frame_sequence;
        int is_new_frame = (current_seq != last_rtsp_sequence) && 
                           (pglobal->in[input_number].size > 0);
        
        if (!is_new_frame) {
        pthread_mutex_unlock(&pglobal->in[input_number].db);
            if (!wait_for_fresh_frame(&pglobal->in[input_number], &last_rtsp_sequence)) {
                usleep(1000);
                continue;
            }
        } else {
            last_rtsp_sequence = current_seq;
        }
        
        frame_size = pglobal->in[input_number].size;
        
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
        
        if (current_frame && frame_size > 0 && pglobal->in[input_number].buf != NULL) {
            simd_memcpy(current_frame, pglobal->in[input_number].buf, frame_size);
            
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
        
        pthread_mutex_unlock(&pglobal->in[input_number].db);
        

        pthread_mutex_lock(&clients_mutex);
        int playing_clients = 0;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (is_valid_client(i)) {
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
        
        static int cached_fps = 0;
        if (cached_fps == 0) {
            int input_fps = 30;
            if (pglobal && input_number >= 0 && input_number < pglobal->incnt && pglobal->in[input_number].fps > 0) {
                input_fps = pglobal->in[input_number].fps;
            }
            cached_fps = input_fps;
            rtp_ts_increment = (uint32_t)(90000 / input_fps);
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

        pthread_mutex_lock(&clients_mutex);
        
        if (prepared_frame.width > 0 && prepared_frame.height > 0) {
            if (!sdp_dimensions_cached ||
                (prepared_frame.width != cached_sdp_width || prepared_frame.height != cached_sdp_height)) {
                cached_sdp_width = prepared_frame.width;
                cached_sdp_height = prepared_frame.height;
                sdp_dimensions_cached = 1;
            }
        }
        
        int clients_count = 0;
        playing_clients = 0;
        uint32_t base_timestamp = 0;
        
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!is_valid_client(i)) continue;
            
            playing_clients++;
            if (base_timestamp == 0 && clients[i].timestamp > 0) {
                base_timestamp = clients[i].timestamp;
            }
        }
        
        if (base_timestamp == 0 && rtp_ts_increment > 0) {
            base_timestamp = rtp_ts_increment;
        }
        
        /* Second pass: send packets to all playing clients */
        if (prepared_frame.rtp_payload != NULL && prepared_frame.rtp_payload_size > 0) {
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (!is_valid_client(i)) continue;
                
                /* Initialize timestamp if needed */
                if (clients[i].timestamp == 0) {
                    if (base_timestamp > 0) {
                        clients[i].timestamp = base_timestamp;
                    } else if (rtp_ts_increment > 0) {
                        clients[i].timestamp = rtp_ts_increment;
                    }
                }
                
                int send_result = send_rtp_packet(rtp_socket, &clients[i], &prepared_frame, clients[i].timestamp, i);
                if (send_result < 0) {
                    OPRINT("[RTP ERROR] Failed to send RTP packet to client %d (socket %d, active=%d, playing=%d)\n", 
                           i, clients[i].socket, clients[i].active, clients[i].playing);
                    if (errno == EPIPE || errno == ECONNRESET || errno == EBADF) {
                        int old_socket = clients[i].socket;
                        clear_client(i);
                        OPRINT(" o: Client %d cleaned up on send error (socket %d)\n", i, old_socket);
                        continue;
                    }
                }
                
                if (send_result >= 0) {
                    clients[i].timestamp += rtp_ts_increment;
                    clients_count++;
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
    
    pglobal = param->global;
    input_number = param->id;
    
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
    
    for (int i = 0; i < MAX_CLIENTS; i++) {
        clear_client(i);
        clients[i].socket = -1;
    }
    
    OPRINT("RTSP server initialized on port %d\n", port);
    OPRINT("Input plugin: %d\n", input_number);

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
    
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active) {
            close(clients[i].socket);
            clear_client(i);
            clients[i].socket = -1;
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