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
/* Transport mode: 0 = UDP (default), 1 = TCP */
static int transport_mode = 0;
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
    unsigned char *baseline_jpeg;      /* recompressed JPEG with default DHT (tjFree) */
    unsigned long baseline_size;
    int width;
    int height;
    int subsamp;                       /* TurboJPEG subsampling (TJSAMP_*) */
    int jpeg_type;                     /* RFC2435 type (0/1/3) */
} rtp_jpeg_frame_t;

static void free_rtp_jpeg_frame(rtp_jpeg_frame_t *frame);
static int prepare_rtp_jpeg_frame(const unsigned char *jpeg_data, size_t jpeg_size,
                                  rtp_jpeg_frame_t *frame_info);



static void free_rtp_jpeg_frame(rtp_jpeg_frame_t *frame)
{
    if (!frame) {
        return;
    }
    if (frame->baseline_jpeg) {
        tjFree(frame->baseline_jpeg);
        frame->baseline_jpeg = NULL;
        frame->baseline_size = 0;
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

    /* Try to enforce 4:2:2 when possible (even width). Otherwise keep original subsampling. */
    int desired_subsamp = TJSAMP_422;
    if ((input_width % 2) != 0) {
        desired_subsamp = input_subsamp; /* Cannot use 4:2:2 on odd width frames */
    }
    if (desired_subsamp < 0) {
        desired_subsamp = input_subsamp >= 0 ? input_subsamp : TJSAMP_422;
    }

    unsigned char *baseline = NULL;
    unsigned long baseline_size = 0;
    if (recompress_jpeg_to_baseline_with_default_dht(jpeg_data, (int)jpeg_size,
                                                     &baseline, &baseline_size,
                                                     75, desired_subsamp) != 0 || !baseline || baseline_size == 0) {
        OPRINT("[RTP ERROR] Failed to recompress JPEG to baseline/default DHT\n");
        return -1;
    }

    /* CRITICAL: Trim JPEG to exact size - find last EOI marker (0xFF 0xD9) */
    /* TurboJPEG may return size with padding/extra bytes after EOI, causing "overread 8" in decoder */
    /* "overread 8" occurs when decoder reads scan data and goes 8 bytes beyond EOI */
    size_t trimmed_size = baseline_size;
    
    /* Find last EOI marker (0xFF 0xD9) - search backwards from end */
    for (int i = (int)baseline_size - 2; i >= 0; i--) {
        if (baseline[i] == 0xFF && baseline[i + 1] == 0xD9) {
            trimmed_size = (size_t)(i + 2); /* Include EOI marker (0xFF 0xD9) */
            break;
        }
    }
    
    if (trimmed_size < baseline_size) {
        frame_info->baseline_size = trimmed_size;
    } else {
        frame_info->baseline_size = baseline_size;
    }
    frame_info->baseline_jpeg = baseline;

    if (turbojpeg_header_info(frame_info->baseline_jpeg, (int)frame_info->baseline_size,
                              &frame_info->width, &frame_info->height, &frame_info->subsamp) != 0) {
        OPRINT("[RTP ERROR] Failed to read header of recompressed JPEG\n");
        free_rtp_jpeg_frame(frame_info);
        return -1;
    }

    /* RFC 2435, Table: Types 0-7 (without restart)
     * Type 0 = 4:2:2, Type 1 = 4:2:0, Type 2 = 4:1:1, Type 3 = 4:4:4
     */
    switch (frame_info->subsamp) {
        case TJSAMP_420: frame_info->jpeg_type = 1; break; /* 4:2:0 -> Type 1 */
        case TJSAMP_422: frame_info->jpeg_type = 0; break; /* 4:2:2 -> Type 0 */
        case TJSAMP_444: frame_info->jpeg_type = 3; break; /* 4:4:4 -> Type 3 */
        default:
            frame_info->jpeg_type = 0; /* fallback: 4:2:2 */
            break;
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
    if (!client || !frame || !frame->baseline_jpeg || frame->baseline_size <= 0) {
        OPRINT("[RTP ERROR] invalid frame data for transmission\n");
        return -1;
    }

    if (frame->width <= 0 || frame->height <= 0) {
        OPRINT("[RTP ERROR] invalid frame dimensions (%d x %d)\n", frame->width, frame->height);
        return -1;
    }

    int frame_width_div8 = frame->width / 8;
    int frame_height_div8 = frame->height / 8;
    if (frame_width_div8 <= 0 || frame_height_div8 <= 0) {
        OPRINT("[RTP ERROR] frame dimensions not divisible by 8 (%d x %d)\n", frame->width, frame->height);
        return -1;
    }

    /* Send full baseline JPEG including SOI - Q=75 (tables in JPEG payload) */
    const unsigned char *jpeg_data = frame->baseline_jpeg;
    size_t jpeg_size = (size_t)frame->baseline_size;
    size_t fragment_offset = 0;
    size_t remaining = jpeg_size;
    uint16_t seq = client->sequence_number;
    int is_tcp = (client->rtp_port == 0);
    size_t max_packet_size = is_tcp ? MAX_TCP_PACKET_SIZE : MAX_RTP_PACKET_SIZE;
    const int q_value_fixed = 75; /* Q=0..99 when tables are in JPEG payload */


    /* CRITICAL: Ensure type=1 (4:2:2) is consistent for all fragments */
    const unsigned char jpeg_type_fixed = (unsigned char)frame->jpeg_type;

    while (remaining > 0) {
        int first_fragment = (fragment_offset == 0);
        size_t total_header_size = 20; /* 12-byte RTP + 8-byte JPEG header */

        if (total_header_size >= max_packet_size) {
            OPRINT("[RTP ERROR] header size %zu exceeds packet size limit %zu\n", total_header_size, max_packet_size);
            return -1;
        }

        size_t max_payload = max_packet_size - total_header_size;
        if (max_payload == 0) {
            OPRINT("[RTP ERROR] no room for payload after headers\n");
            return -1;
        }

        size_t payload_size = (remaining < max_payload) ? remaining : max_payload;
        int is_last_packet = (payload_size == remaining);
        
        /* CRITICAL: Verify fragment offset is correct - must match position in JPEG */
        if (fragment_offset + payload_size > jpeg_size) {
            OPRINT("[RTP ERROR] Fragment offset %zu + payload %zu exceeds JPEG size %zu!\n",
                   fragment_offset, payload_size, jpeg_size);
            return -1;
        }

        unsigned char packet[MAX_TCP_PACKET_SIZE];

        packet[0] = 0x80;
        packet[1] = (is_last_packet ? 0x80 : 0x00) | RTP_PAYLOAD_TYPE;
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
        packet[16] = jpeg_type_fixed; /* CRITICAL: Use fixed type for all fragments */
        packet[17] = (unsigned char)q_value_fixed;
        packet[18] = (unsigned char)frame_width_div8;
        packet[19] = (unsigned char)frame_height_div8;

        size_t payload_offset = 20;
        simd_memcpy(packet + payload_offset, jpeg_data + fragment_offset, payload_size);
        size_t packet_size = payload_offset + payload_size;


        int sent = 0;
        if (is_tcp) {
            static int tcp_nodelay_sockets[MAX_CLIENTS] = {0};
            int client_idx = -1;
            pthread_mutex_lock(&clients_mutex);
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (clients[i].socket == client->socket) {
                    client_idx = i;
                    break;
                }
            }
            pthread_mutex_unlock(&clients_mutex);

            if (client_idx >= 0 && !tcp_nodelay_sockets[client_idx]) {
                int opt = 1;
                if (setsockopt(client->socket, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) == 0) {
                    tcp_nodelay_sockets[client_idx] = 1;
                }
            }

            unsigned char tcp_packet[4 + MAX_TCP_PACKET_SIZE];
            tcp_packet[0] = '$';
            tcp_packet[1] = 0;
            tcp_packet[2] = (packet_size >> 8) & 0xFF;
            tcp_packet[3] = packet_size & 0xFF;
            simd_memcpy(tcp_packet + 4, packet, packet_size);
            size_t to_send = 4 + packet_size;
            const unsigned char *ptr = tcp_packet;
            while (to_send > 0) {
                sent = send(client->socket, ptr, to_send, MSG_NOSIGNAL);
                if (sent <= 0) {
                    if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                        OPRINT("Error sending RTP over TCP: %s\n", strerror(errno));
                        return -1;
                    }
                    continue;
                }
                to_send -= sent;
                ptr += sent;
            }
        } else {
            struct sockaddr_in rtp_addr;
            memset(&rtp_addr, 0, sizeof(rtp_addr));
            rtp_addr.sin_family = AF_INET;
            rtp_addr.sin_addr = client->addr.sin_addr;
            rtp_addr.sin_port = htons(client->rtp_port);
            sent = sendto(rtp_socket, packet, packet_size, 0,
                          (struct sockaddr *)&rtp_addr, sizeof(rtp_addr));
            if (sent < 0) {
                OPRINT("Error sending RTP over UDP: %s\n", strerror(errno));
                return -1;
            }
            if (sent != (int)packet_size) {
                OPRINT("Partial UDP send: %d of %zu\n", sent, packet_size);
                return -1;
            }
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
        OPRINT(" o: Sending OPTIONS response: %zu bytes", strlen(response));
        send(client_socket, response, strlen(response), 0);
        OPRINT(" o: Sent OPTIONS response: %zu bytes", strlen(response));
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
                 "a=framesize:26 %dx%d\r\n"
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
        OPRINT(" o: Sending DESCRIBE response: %zu bytes", strlen(response));
        send(client_socket, response, strlen(response), 0);
        OPRINT(" o: Sent DESCRIBE response: %zu bytes", strlen(response));
    } else if (strcmp(method, "SETUP") == 0) {
        int client_rtp_port = 0, client_rtcp_port = 0;
        int use_tcp = (transport_mode == 1) ? 1 : 0; /* Default from transport_mode */
        int client_explicitly_requested_tcp = 0;
        int client_explicitly_requested_udp = 0;
        // Parse Transport header - use original request buffer
        char *transport_line = strstr(request, "Transport:");
        if (transport_line) {
            /* Check if client explicitly requested TCP */
            if (strstr(transport_line, "RTP/AVP/TCP")) { 
                client_explicitly_requested_tcp = 1;
                use_tcp = 1;
            }
            /* Check if client explicitly requested UDP */
            char *client_port = strstr(transport_line, "client_port=");
            if (client_port && !client_explicitly_requested_tcp) {
                sscanf(client_port, "client_port=%d-%d", &client_rtp_port, &client_rtcp_port);
                if (client_rtp_port > 0 && client_rtcp_port > 0) {
                    client_explicitly_requested_udp = 1;
                    use_tcp = 0;
                }
                OPRINT(" o: Parsed client RTP/RTCP ports: %d/%d", client_rtp_port, client_rtcp_port);
            }
        }
        
        if (use_tcp) {
            OPRINT(" o: Client requested TCP transport");
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
                    OPRINT("[RTSP] Added client %d (TCP) on socket %d, addr=%s\n", 
                           i, client_socket, inet_ntoa(client_addr.sin_addr));
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
            OPRINT(" o: Client requested UDP transport");
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
                    OPRINT(" o: Added client %d (UDP) on socket %d with RTP port %d", i, client_socket, client_rtp_port);
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
        OPRINT(" o: Sending SETUP response: %zu bytes", strlen(response));
        send(client_socket, response, strlen(response), 0);
        OPRINT(" o: Sent SETUP response: %zu bytes", strlen(response));
    } else if (strcmp(method, "PLAY") == 0) {
        OPRINT("[RTSP DEBUG] Received PLAY request for socket %d, cseq=%d\n", client_socket, cseq);
        int i;
        pthread_mutex_lock(&clients_mutex);
        for (i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].active && clients[i].socket == client_socket) {
                clients[i].playing = 1;
                OPRINT("[RTSP] Client %d started playing (socket=%d, active=%d, rtp_port=%d)\n", 
                       i, clients[i].socket, clients[i].active, clients[i].rtp_port);
                break;
            }
        }
        if (i < MAX_CLIENTS) {
            OPRINT("[RTSP DEBUG] Client %d set to playing: active=%d, playing=%d, socket=%d, rtp_port=%d\n", i, clients[i].active, clients[i].playing, clients[i].socket, clients[i].rtp_port);
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
        OPRINT(" o: Sending PLAY response: %zu bytes", strlen(response));
        send(client_socket, response, strlen(response), 0);
        OPRINT(" o: Sent PLAY response: %zu bytes", strlen(response));
        /* Wake streaming thread AFTER PLAY response is sent */
        /* This ensures client is ready to receive RTP packets */
        /* Send immediately - delays may cause flickering in VLC */
        if (pglobal && i < MAX_CLIENTS) {
            OPRINT("[RTSP] PLAY: Broadcasting to wake streaming thread\n");
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
                OPRINT(" o: Client %d paused", i);
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
        OPRINT(" o: Sending PAUSE response: %zu bytes", strlen(response));
        send(client_socket, response, strlen(response), 0);
        OPRINT(" o: Sent PAUSE response: %zu bytes", strlen(response));
    } else if (strcmp(method, "TEARDOWN") == 0) {
        int i;
        pthread_mutex_lock(&clients_mutex);
        for (i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].active && clients[i].socket == client_socket) {
                clients[i].active = 0;
                clients[i].playing = 0;
                OPRINT(" o: Client %d disconnected", i);
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
        OPRINT(" o: Sending TEARDOWN response: %zu bytes", strlen(response));
        send(client_socket, response, strlen(response), 0);
        OPRINT(" o: Sent TEARDOWN response: %zu bytes", strlen(response));
    } else {
        snprintf(response, sizeof(response),
                 "RTSP/1.0 400 Bad Request\r\n"
                 "CSeq: %d\r\n"
                 "Server: MJPG-Streamer RTSP Server\r\n"
                 "\r\n", cseq);
        OPRINT(" o: Sending Bad Request response: %zu bytes", strlen(response));
        send(client_socket, response, strlen(response), 0);
        OPRINT(" o: Sent Bad Request response: %zu bytes", strlen(response));
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

    OPRINT("New client thread started for socket %d", client_socket);

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
            OPRINT("[TCP] Received interleaved binary data: channel=%d, length=%u (total=%d)\n", 
                   channel, length, bytes_read);
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
        OPRINT("Received RTSP request (%d bytes): %s", bytes_read, buffer);
        
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
            memset(&clients[i].addr, 0, sizeof(clients[i].addr));
            OPRINT("Removed client %d from list", i);
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
        OPRINT("Waiting for client connection...\n");
        OPRINT("Server socket: %d\n", server_socket);
        OPRINT("Server running: %d\n", server_running);
        OPRINT("Pglobal stop: %d\n", pglobal->stop);
        client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &client_len);
        if (client_socket < 0) {
            if (errno == EINTR) {
                OPRINT("Accept interrupted, continuing...\n");
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
            OPRINT("WARNING: Failed to set TCP_NODELAY on client socket %d: %s\n", 
                   client_socket, strerror(errno));
        }
        /* Increase send buffer size for VLC - helps with stable streaming */
        /* VLC may need larger buffer to handle packet bursts */
        int send_buf_size = 256 * 1024; /* 256KB send buffer */
        if (setsockopt(client_socket, SOL_SOCKET, SO_SNDBUF, &send_buf_size, sizeof(send_buf_size)) < 0) {
            OPRINT("WARNING: Failed to set SO_SNDBUF on client socket %d: %s\n", 
                   client_socket, strerror(errno));
        }
        
        OPRINT("Client connected! Socket: %d\n", client_socket);
        OPRINT("Client socket accepted: %d\n", client_socket);
        OPRINT("Client address: %s:%d\n", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
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
        
        OPRINT("Creating client thread for socket %d\n", client_socket);
        
        if (pthread_create(&client_thread, NULL, handle_client_thread, client_data) != 0) {
            OPRINT("Failed to create client thread\n");
            free(client_data);
            close(client_socket);
            continue;
        }
        
        OPRINT("Client thread created successfully\n");
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
            if (clients[i].active && clients[i].playing && (is_tcp_client || is_udp_client) && prepared_frame.baseline_jpeg != NULL && prepared_frame.baseline_size > 0) {
                if (clients[i].timestamp == 0) {
                    if (base_timestamp > 0) {
                        clients[i].timestamp = base_timestamp;
                    } else if (rtp_ts_increment > 0) {
                        clients[i].timestamp = rtp_ts_increment;
                    }
                }
                clients_count++;
                if (send_rtp_packet(rtp_socket, &clients[i], &prepared_frame, clients[i].timestamp) < 0) {
                    OPRINT("Failed to send to client %d\n", i);
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
    OPRINT("[TJ] Decompress handle initialized (via jpeg_utils cache)\n");
    
    /* Parse parameters */
    /* Parameters are already parsed by mjpg_streamer main */
    OPRINT("Parsing RTSP parameters: argc=%d\n", param->argc);
    if (param->argv && param->argc > 0) {
        for (int i = 0; i < param->argc; i++) {
            if (param->argv[i] && (!strcmp(param->argv[i], "-h") || !strcmp(param->argv[i], "--help"))) {
                OPRINT("RTSP output plugin options:\n");
                OPRINT("  -i, --input <num>   Input channel index (default from core)\n");
                OPRINT("  -t, --tcp           Force RTP over RTSP (TCP)\n");
                OPRINT("  -u, --udp           Use RTP over UDP (default)\n");
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
            } else if (param->argv[i] && (!strcmp(param->argv[i], "-t") || !strcmp(param->argv[i], "--tcp"))) {
                transport_mode = 1;
                OPRINT("Transport set to TCP (--tcp)\n");
            } else if (param->argv[i] && (!strcmp(param->argv[i], "-u") || !strcmp(param->argv[i], "--udp"))) {
                transport_mode = 0;
                OPRINT("Transport set to UDP (--udp)\n");
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