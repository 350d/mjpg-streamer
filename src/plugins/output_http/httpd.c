/*******************************************************************************
#                                                                              #
#      MJPG-streamer allows to stream JPG frames from an input-plugin          #
#      to several output plugins                                               #
#                                                                              #
#      Copyright (C) 2007 busybox-project (base64 function)                    #
#      Copyright (C) 2007 Tom Stöveken                                         #
#                                                                              #
# This program is free software; you can redistribute it and/or modify         #
# it under the terms of the GNU General Public License as published by         #
# the Free Software Foundation; version 2 of the License.                      #
#                                                                              #
# This program is distributed in the hope that it will be useful,              #
# but WITHOUT ANY WARRANTY; without even the implied warranty of               #
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the                #
# GNU General Public License for more details.                                 #
#                                                                              #
# You should have received a copy of the GNU General Public License            #
# along with this program; if not, write to the Free Software                  #
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA    #
#                                                                              #
*******************************************************************************/
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/select.h>
#ifdef __linux__
#include <sys/epoll.h>
#endif
#include <arpa/inet.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <netdb.h>
#include <errno.h>
#include <limits.h>

#ifdef __linux__
#include <linux/version.h>
#include <linux/types.h>          /* for videodev2.h */
#include <linux/videodev2.h>
#define V4L2_CTRL_TYPE_STRING_SUPPORTED
#else
// Define KERNEL_VERSION for non-Linux systems
#define KERNEL_VERSION(a,b,c) (((a) << 16) + ((b) << 8) + (c))
#define LINUX_VERSION_CODE KERNEL_VERSION(3,0,0)
#endif

/* SIMD functions are now in utils.c */

#include "../../mjpg_streamer.h"
#include "../../utils.h"

#include "httpd.h"

#include "../output_file/output_file.h"

/* SIMD functions are now provided by utils.c */

/* Write buffer functions for I/O optimization */
static int flush_write_buffer(write_buffer *wb);

static void init_write_buffer(write_buffer *wb, int fd) {
    wb->buffer_pos = 0;
    wb->fd = fd;
    wb->use_buffering = 1;
    memset(wb->buffer, 0, sizeof(wb->buffer));
}

static int buffered_write(write_buffer *wb, const void *data, size_t len) {
    if (!wb->use_buffering) {
        return write(wb->fd, data, len);
    }
    
    const char *src = (const char*)data;
    size_t remaining = len;
    
    while (remaining > 0) {
        size_t space_in_buffer = sizeof(wb->buffer) - wb->buffer_pos;
        size_t to_copy = (remaining < space_in_buffer) ? remaining : space_in_buffer;
        
        simd_memcpy(wb->buffer + wb->buffer_pos, src, to_copy);
        wb->buffer_pos += to_copy;
        src += to_copy;
        remaining -= to_copy;
        
        /* Flush buffer if full */
        if (wb->buffer_pos >= sizeof(wb->buffer)) {
            if (flush_write_buffer(wb) < 0) {
                return -1;
            }
        }
    }
    
    return len;
}

static int flush_write_buffer(write_buffer *wb) {
    if (wb->buffer_pos == 0) {
        return 0;
    }
    
    int result = write(wb->fd, wb->buffer, wb->buffer_pos);
    if (result < 0) {
        return -1;
    }
    
    wb->buffer_pos = 0;
    return result;
}

/* HTTP Keep-Alive support functions */
static int should_use_keep_alive(int fd) {
    /* For now, use Keep-Alive for all connections */
    /* In future, could check client capabilities or configuration */
    return 1;
}

/* Stage 3: epoll async I/O functions */
#ifdef __linux__
int init_async_io(async_io_context *ctx, int max_events) {
    ctx->max_events = max_events;
    ctx->client_count = 0;
    ctx->server_socket_count = 0;
    
    ctx->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (ctx->epfd == -1) {
        perror("epoll_create1");
        return -1;
    }
    
    ctx->events = malloc(max_events * sizeof(struct epoll_event));
    if (!ctx->events) {
        close(ctx->epfd);
        return -1;
    }
    
    return 0;
}

void cleanup_async_io(async_io_context *ctx) {
    if (ctx->events) {
        free(ctx->events);
        ctx->events = NULL;
    }
    if (ctx->epfd != -1) {
        close(ctx->epfd);
        ctx->epfd = -1;
    }
}

int add_server_socket(async_io_context *ctx, int sockfd) {
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = sockfd;
    
    if (epoll_ctl(ctx->epfd, EPOLL_CTL_ADD, sockfd, &ev) == -1) {
        perror("epoll_ctl: ADD server socket");
        return -1;
    }
    
    ctx->server_sockets[ctx->server_socket_count] = sockfd;
    ctx->server_socket_count++;
    
    return 0;
}

int add_client_socket(async_io_context *ctx, int sockfd) {
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;  /* Edge-triggered for clients */
    ev.data.fd = sockfd;
    
    if (epoll_ctl(ctx->epfd, EPOLL_CTL_ADD, sockfd, &ev) == -1) {
        perror("epoll_ctl: ADD client socket");
        return -1;
    }
    
    ctx->client_count++;
    return 0;
}

int remove_client_socket(async_io_context *ctx, int sockfd) {
    if (epoll_ctl(ctx->epfd, EPOLL_CTL_DEL, sockfd, NULL) == -1) {
        perror("epoll_ctl: DEL client socket");
        return -1;
    }
    
    ctx->client_count--;
    return 0;
}
#else
/* Fallback implementations for non-Linux systems */
int init_async_io(async_io_context *ctx, int max_events) {
    ctx->max_events = max_events;
    ctx->client_count = 0;
    ctx->server_socket_count = 0;
    ctx->epfd = -1;
    ctx->events = NULL;
    return 0;
}

void cleanup_async_io(async_io_context *ctx) {
    /* No-op for non-Linux systems */
}

int add_server_socket(async_io_context *ctx, int sockfd) {
    /* No-op for non-Linux systems */
    return 0;
}

int add_client_socket(async_io_context *ctx, int sockfd) {
    /* No-op for non-Linux systems */
    return 0;
}

int remove_client_socket(async_io_context *ctx, int sockfd) {
    /* No-op for non-Linux systems */
    return 0;
}
#endif


static globals *pglobal;
extern context servers[MAX_OUTPUT_PLUGINS];

/* Forward declarations */
int unescape(char *string);
static int parse_short_path(const char *buffer, const char *path_prefix, int *number);

/* Helper function to check client status and set error if needed */
static int check_and_handle_client_status(cfd *lcfd, request *req, int *query_suffixed)
{
    return 0;
}

/* Helper function to parse parameter from buffer */
static int parse_parameter(const char *buffer, const char *prefix, char **parameter, const char *allowed_chars)
{
    const char *pb = strstr(buffer, prefix);
    if (pb == NULL) {
        return -1;
    }
    pb += strlen(prefix);
    int len = MIN(MAX(strspn(pb, allowed_chars), 0), 100);
    *parameter = malloc(len + 1);
    if (*parameter == NULL) {
        return -1;
    }
    memset(*parameter, 0, len + 1);
    strncpy(*parameter, pb, len);
    if (unescape(*parameter) == -1) {
        free(*parameter);
        *parameter = NULL;
        return -1;
    }
    return len;
}

/* Helper function to parse short path and extract action type and number */
static int parse_short_path(const char *buffer, const char *path_prefix, int *number)
{
    char path_pattern[32];
    // Try GET first
    snprintf(path_pattern, sizeof(path_pattern), "GET /%s", path_prefix);
    const char *pb = strstr(buffer, path_pattern);
    if (pb == NULL) {
        // Try POST for stream
        snprintf(path_pattern, sizeof(path_pattern), "POST /%s", path_prefix);
        pb = strstr(buffer, path_pattern);
        if (pb == NULL) {
            return 0; // Not found
        }
    }
    pb += strlen(path_pattern);
    
    // Check if there's a number after the path
    if (*pb >= '0' && *pb <= '9') {
        *number = atoi(pb);
        // Skip digits
        while (*pb >= '0' && *pb <= '9') pb++;
        // Check if there's a query string or end
        if (*pb == ' ' || *pb == '?' || *pb == '\r' || *pb == '\n' || *pb == '\0') {
            return 1; // Found with number
        }
    } else if (*pb == ' ' || *pb == '?' || *pb == '\r' || *pb == '\n' || *pb == '\0') {
        *number = 0; // Default to 0 if no number specified
        return 1; // Found without number
    }
    return 0; // Not a valid path
}

/******************************************************************************
Description.: initializes the iobuffer structure properly
Input Value.: pointer to already allocated iobuffer
Return Value: iobuf
******************************************************************************/
void init_iobuffer(iobuffer *iobuf)
{
    memset(iobuf->buffer, 0, sizeof(iobuf->buffer));
    iobuf->level = 0;
}

/******************************************************************************
Description.: initializes the request structure properly
Input Value.: pointer to already allocated req
Return Value: req
******************************************************************************/
void init_request(request *req)
{
    req->type        = A_UNKNOWN;
    req->parameter   = NULL;
    req->client      = NULL;
    req->credentials = NULL;
}

/******************************************************************************
Description.: If strings were assigned to the different members free them
              This will fail if strings are static, so always use strdup().
Input Value.: req: pointer to request structure
Return Value: -
******************************************************************************/
void free_request(request *req)
{
    if(req->parameter != NULL) free(req->parameter);
    if(req->client != NULL) free(req->client);
    if(req->credentials != NULL) free(req->credentials);
    if(req->query_string != NULL) free(req->query_string);
}

/******************************************************************************
Description.: read with timeout, implemented without using signals
              tries to read len bytes and returns if enough bytes were read
              or the timeout was triggered. In case of timeout the return
              value may differ from the requested bytes "len".
Input Value.: * fd.....: fildescriptor to read from
              * iobuf..: iobuffer that allows to use this functions from multiple
                         threads because the complete context is the iobuffer.
              * buffer.: The buffer to store values at, will be set to zero
                         before storing values.
              * len....: the length of buffer
              * timeout: seconds to wait for an answer
Return Value: * buffer.: will become filled with bytes read
              * iobuf..: May get altered to save the context for future calls.
              * func().: bytes copied to buffer or -1 in case of error
******************************************************************************/
int _read(int fd, iobuffer *iobuf, void *buffer, size_t len, int timeout)
{
    int copied = 0, rc, i;
    fd_set fds;
    struct timeval tv;

    memset(buffer, 0, len);

    while((copied < len)) {
        i = MIN(iobuf->level, len - copied);
        memcpy(buffer + copied, iobuf->buffer + IO_BUFFER - iobuf->level, i);

        iobuf->level -= i;
        copied += i;
        if(copied >= len)
            return copied;

        /* select will return in case of timeout or new data arrived */
        tv.tv_sec = timeout;
        tv.tv_usec = 0;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        if((rc = select(fd + 1, &fds, NULL, NULL, &tv)) <= 0) {
            if(rc < 0)
                exit(EXIT_FAILURE);

            /* this must be a timeout */
            return copied;
        }

        init_iobuffer(iobuf);

        /*
         * there should be at least one byte, because select signalled it.
         * But: It may happen (very seldomly), that the socket gets closed remotly between
         * the select() and the following read. That is the reason for not relying
         * on reading at least one byte.
         */
        if((iobuf->level = read(fd, &iobuf->buffer, IO_BUFFER)) <= 0) {
            /* an error occured */
            return -1;
        }

        /* align data to the end of the buffer if less than IO_BUFFER bytes were read */
        memmove(iobuf->buffer + (IO_BUFFER - iobuf->level), iobuf->buffer, iobuf->level);
    }

    return 0;
}

/******************************************************************************
Description.: Read a single line from the provided fildescriptor.
              This funtion will return under two conditions:
              * line end was reached
              * timeout occured
Input Value.: * fd.....: fildescriptor to read from
              * iobuf..: iobuffer that allows to use this functions from multiple
                         threads because the complete context is the iobuffer.
              * buffer.: The buffer to store values at, will be set to zero
                         before storing values.
              * len....: the length of buffer
              * timeout: seconds to wait for an answer
Return Value: * buffer.: will become filled with bytes read
              * iobuf..: May get altered to save the context for future calls.
              * func().: bytes copied to buffer or -1 in case of error
******************************************************************************/
/* read just a single line or timeout */
int _readline(int fd, iobuffer *iobuf, void *buffer, size_t len, int timeout)
{
    char c = '\0', *out = buffer;
    int i;

    memset(buffer, 0, len);

    for(i = 0; i < len && c != '\n'; i++) {
        if(_read(fd, iobuf, &c, 1, timeout) <= 0) {
            /* timeout or error occured */
            return -1;
        }
        *out++ = c;
    }

    return i;
}

/******************************************************************************
Description.: Decodes the data and stores the result to the same buffer.
              The buffer will be large enough, because base64 requires more
              space then plain text.
Hints.......: taken from busybox, but it is GPL code
Input Value.: base64 encoded data
Return Value: plain decoded data
******************************************************************************/
void decodeBase64(char *data)
{
    const unsigned char *in = (const unsigned char *)data;
    /* The decoded size will be at most 3/4 the size of the encoded */
    unsigned ch = 0;
    int i = 0;

    while(*in) {
        int t = *in++;

        if(t >= '0' && t <= '9')
            t = t - '0' + 52;
        else if(t >= 'A' && t <= 'Z')
            t = t - 'A';
        else if(t >= 'a' && t <= 'z')
            t = t - 'a' + 26;
        else if(t == '+')
            t = 62;
        else if(t == '/')
            t = 63;
        else if(t == '=')
            t = 0;
        else
            continue;

        ch = (ch << 6) | t;
        i++;
        if(i == 4) {
            *data++ = (char)(ch >> 16);
            *data++ = (char)(ch >> 8);
            *data++ = (char) ch;
            i = 0;
        }
    }
    *data = '\0';
}

/******************************************************************************
Description.: convert a hexadecimal ASCII character to integer
Input Value.: ASCII character
Return Value: corresponding value between 0 and 15, or -1 in case of error
******************************************************************************/
int hex_char_to_int(char in)
{
    if(in >= '0' && in <= '9')
        return in - '0';

    if(in >= 'a' && in <= 'f')
        return (in - 'a') + 10;

    if(in >= 'A' && in <= 'F')
        return (in - 'A') + 10;

    return -1;
}

/******************************************************************************
Description.: replace %XX with the character code it represents, URI
Input Value.: string to unescape
Return Value: 0 if everything is ok, -1 in case of error
******************************************************************************/
int unescape(char *string)
{
    char *dst = string;
    int length = strlen(string);
    int rc;

    for(int src = 0; src < length; src++) {
        if(string[src] != '%') {
            *dst++ = string[src];
            continue;
        }

        if(src + 2 >= length) {
            return -1;
        }

        if((rc = hex_char_to_int(string[src+1])) == -1) return -1;
        *dst = rc * 16;
        if((rc = hex_char_to_int(string[src+2])) == -1) return -1;
        *dst++ += rc;
        src += 2;
    }

    *dst = '\0';
    return 0;
}


/******************************************************************************
Description.: Send a complete HTTP response and a single JPG-frame.
Input Value.: fildescriptor fd to send the answer to
Return Value: -
******************************************************************************/
void send_snapshot(cfd *context_fd, int input_number)
{
    unsigned char *frame = NULL;
    int frame_size = 0;
    char *buffer = NULL;
    struct timeval timestamp;
    context *server_context = context_fd->pc;

    /* get current frame using timestamp mechanism */
    pthread_mutex_lock(&pglobal->in[input_number].db);
    
    /* wait for fresh frame using event-driven approach */
    static unsigned int last_snapshot_sequence = UINT_MAX;  /* Start with max value to ensure first frame is processed */
    
    /* check if frame is new using sequence number */
    unsigned int current_frame_sequence = pglobal->in[input_number].frame_sequence;
    if (current_frame_sequence == last_snapshot_sequence) {
        /* No new frame, unlock and return */
        pthread_mutex_unlock(&pglobal->in[input_number].db);
        return;
    }
    last_snapshot_sequence = current_frame_sequence;

    /* read buffer */
    frame_size = pglobal->in[input_number].size;
    
    /* Use static buffer if available and sufficient, otherwise fallback to dynamic */
    if (server_context && server_context->use_static_buffers && frame_size <= MAX_FRAME_SIZE) {
        frame = server_context->static_frame_buffer;
        buffer = (char*)server_context->static_header_buffer;
    } else {
        /* Fallback to dynamic allocation for large frames */
        if(frame == NULL || frame_size > 10*1024*1024) { // 10MB limit
            if(frame != NULL) free(frame);
            frame = malloc(frame_size);
            if(frame == NULL) {
                pthread_mutex_unlock(&pglobal->in[input_number].db);
                LOG("not enough memory for frame buffer\n");
                return;
            }
        }
        buffer = malloc(BUFFER_SIZE);
        if(buffer == NULL) {
            if(frame != server_context->static_frame_buffer) free(frame);
            pthread_mutex_unlock(&pglobal->in[input_number].db);
            LOG("not enough memory for header buffer\n");
            return;
        }
    }
    
    /* copy frame to our local buffer using SIMD optimization */
    simd_memcpy(frame, pglobal->in[input_number].buf, frame_size);

    /* copy v4l2_buffer timeval to user space */
    timestamp = pglobal->in[input_number].timestamp;

    DBG("got frame (size: %d kB)\n", frame_size / 1024);

    pthread_mutex_unlock(&pglobal->in[input_number].db);

    /* write the response header with dynamic values */
    char header_buffer[512];
    int header_len = snprintf(header_buffer, sizeof(header_buffer),
        "HTTP/1.0 200 OK\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n"
        "Server: MJPG-Streamer/0.2\r\n"
        "Cache-Control: no-store, no-cache, must-revalidate, pre-check=0, post-check=0, max-age=0\r\n"
        "Pragma: no-cache\r\n"
        "Expires: Mon, 3 Jan 2000 12:34:56 GMT\r\n"
        "Content-type: image/jpeg\r\n"
        "X-Timestamp: %d.%06d\r\n"
        "X-Framerate: 0\r\n"
        "\r\n",
        (int)timestamp.tv_sec, (int)timestamp.tv_usec);
    if (write(context_fd->fd, header_buffer, header_len) < 0) {
        if (!server_context->use_static_buffers || frame_size > MAX_FRAME_SIZE) {
            if (frame != server_context->static_frame_buffer) free(frame);
            if (buffer != (char*)server_context->static_header_buffer) free(buffer);
        }
        return;
    }

    /* send image data */
    if (write(context_fd->fd, frame, frame_size) < 0) {
        /* Clean up dynamic buffers if used */
        if (server_context && (!server_context->use_static_buffers || frame_size > MAX_FRAME_SIZE)) {
            if (frame != server_context->static_frame_buffer) free(frame);
            if (buffer != (char*)server_context->static_header_buffer) free(buffer);
        }
        return;
    }
    
    
    /* Clean up dynamic buffers if used */
    if (server_context && (!server_context->use_static_buffers || frame_size > MAX_FRAME_SIZE)) {
        if (frame != server_context->static_frame_buffer) free(frame);
        if (buffer != (char*)server_context->static_header_buffer) free(buffer);
    }
}

/******************************************************************************
Description.: Send a complete HTTP response and a stream of JPG-frames.
Input Value.: fildescriptor fd to send the answer to
Return Value: -
******************************************************************************/
void send_stream(cfd *context_fd, int input_number)
{
    unsigned char *frame = NULL, *tmp = NULL;
    int frame_size = 0, max_frame_size = 0;
    char buffer[BUFFER_SIZE] = {0};
    struct timeval timestamp;

    DBG("preparing header\n");
    /* Get initial timestamp and fps for stream header */
    pthread_mutex_lock(&pglobal->in[input_number].db);
    struct timeval initial_timestamp = pglobal->in[input_number].timestamp;
    int initial_fps = pglobal->in[input_number].fps;
    pthread_mutex_unlock(&pglobal->in[input_number].db);
    
    /* Write stream header with dynamic values */
    char header_buffer[512];
    int header_len = snprintf(header_buffer, sizeof(header_buffer),
        "HTTP/1.0 200 OK\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: keep-alive\r\n"
        "Keep-Alive: timeout=5, max=100\r\n"
        "Server: MJPG-Streamer/0.2\r\n"
        "Cache-Control: no-store, no-cache, must-revalidate, pre-check=0, post-check=0, max-age=0\r\n"
        "Pragma: no-cache\r\n"
        "Expires: Mon, 3 Jan 2000 12:34:56 GMT\r\n"
        "Content-Type: multipart/x-mixed-replace;boundary=" BOUNDARY "\r\n"
        "X-Timestamp: %d.%06d\r\n"
        "X-Framerate: %d\r\n"
        "\r\n"
        "--" BOUNDARY "\r\n",
        (int)initial_timestamp.tv_sec, (int)initial_timestamp.tv_usec, initial_fps);
    if (write(context_fd->fd, header_buffer, header_len) < 0) {
        free(frame);
        return;
    }

    DBG("Headers send, sending stream now\n");

    unsigned int last_frame_sequence = UINT_MAX;  /* Start with max value to ensure first frame is processed */
    
    while(!pglobal->stop) {

        /* wait for new frame using helper (mutex held on success) */
        if (!wait_for_fresh_frame(&pglobal->in[input_number], &last_frame_sequence)) {
            /* Add small delay to prevent busy waiting */
            usleep(1000); /* 1ms delay */
            continue;
        }
        last_frame_sequence = pglobal->in[input_number].frame_sequence;
        
        /* read buffer */
        frame_size = pglobal->in[input_number].size;

        /* check if framebuffer is large enough, increase it if necessary */
        if(frame_size > max_frame_size) {
            DBG("increasing buffer size to %d\n", frame_size);

            max_frame_size = frame_size + TEN_K;
            if((tmp = realloc(frame, max_frame_size)) == NULL) {
                free(frame);
                pthread_mutex_unlock(&pglobal->in[input_number].db);
                send_error(context_fd->fd, 500, "not enough memory");
                return;
            }

            frame = tmp;
        }

        /* copy v4l2_buffer timeval to user space */
        timestamp = pglobal->in[input_number].timestamp;

        simd_memcpy(frame, pglobal->in[input_number].buf, frame_size);
        DBG("got frame (size: %d kB)\n", frame_size / 1024);

        pthread_mutex_unlock(&pglobal->in[input_number].db);

        /*
         * print the individual mimetype and the length
         * sending the content-length fixes random stream disruption observed
         * with firefox
         */
        int fps = pglobal->in[input_number].fps;
        sprintf(buffer, "Content-Type: image/jpeg\r\n" \
                "Content-Length: %d\r\n" \
                "X-Timestamp: %d.%06d\r\n" \
                "X-Framerate: %d\r\n" \
                "\r\n", frame_size, (int)timestamp.tv_sec, (int)timestamp.tv_usec, fps);
        DBG("sending intemdiate header\n");
        if(write(context_fd->fd, buffer, strlen(buffer)) < 0) break;

        DBG("sending frame\n");
        if(write(context_fd->fd, frame, frame_size) < 0) break;

        DBG("sending boundary\n");
        sprintf(buffer, "\r\n--" BOUNDARY "\r\n");
        if(write(context_fd->fd, buffer, strlen(buffer)) < 0) break;
        
    }

    free(frame);
}


/******************************************************************************
Description.: Send error messages and headers.
Input Value.: * fd.....: is the filedescriptor to send the message to
              * which..: HTTP error code, most popular is 404
              * message: append this string to the displayed response
Return Value: -
******************************************************************************/
void send_error(int fd, int which, char *message)
{
    char buffer[BUFFER_SIZE] = {0};

    if(which == 401) {
        sprintf(buffer, "HTTP/1.0 401 Unauthorized\r\n" \
                "Content-type: text/plain\r\n" \
                STD_HEADER \
                "WWW-Authenticate: Basic realm=\"MJPG-Streamer\"\r\n" \
                "\r\n" \
                "401: Not Authenticated!\r\n" \
                "%s", message);
    } else if(which == 404) {
        sprintf(buffer, "HTTP/1.0 404 Not Found\r\n" \
                "Content-type: text/plain\r\n" \
                STD_HEADER \
                "\r\n" \
                "404: Not Found!\r\n" \
                "%s", message);
    } else if(which == 500) {
        sprintf(buffer, "HTTP/1.0 500 Internal Server Error\r\n" \
                "Content-type: text/plain\r\n" \
                STD_HEADER \
                "\r\n" \
                "500: Internal Server Error!\r\n" \
                "%s", message);
    } else if(which == 400) {
        sprintf(buffer, "HTTP/1.0 400 Bad Request\r\n" \
                "Content-type: text/plain\r\n" \
                STD_HEADER \
                "\r\n" \
                "400: Not Found!\r\n" \
                "%s", message);
    } else if (which == 403) {
        sprintf(buffer, "HTTP/1.0 403 Forbidden\r\n" \
                "Content-type: text/plain\r\n" \
                STD_HEADER \
                "\r\n" \
                "403: Forbidden!\r\n" \
                "%s", message);
    } else {
        sprintf(buffer, "HTTP/1.0 501 Not Implemented\r\n" \
                "Content-type: text/plain\r\n" \
                STD_HEADER \
                "\r\n" \
                "501: Not Implemented!\r\n" \
                "%s", message);
    }

    if(write(fd, buffer, strlen(buffer)) < 0) {
        DBG("write failed, done anyway\n");
    }
}

/******************************************************************************
Description.: Send HTTP header and copy the content of a file. To keep things
              simple, just a single folder gets searched for the file. Just
              files with known extension and supported mimetype get served.
              If no parameter was given, the file "index.html" will be copied.
Input Value.: * fd.......: filedescriptor to send data to
              * id.......: specifies which server-context is the right one
              * parameter: string that consists of the filename
Return Value: -
******************************************************************************/
void send_file(int id, int fd, char *parameter)
{
    char buffer[BUFFER_SIZE] = {0};
    char *extension, *mimetype = NULL;
    int i, lfd;
    config conf = servers[id].conf;

    /* in case no parameter was given */
    if(parameter == NULL || strlen(parameter) == 0)
        parameter = "index.html";

    /* find file-extension */
    char * pch;
    pch = strchr(parameter, '.');
    int lastDot = 0;
    while(pch != NULL) {
        lastDot = pch - parameter;
        pch = strchr(pch + 1, '.');
    }

    if(lastDot == 0) {
        send_error(fd, 400, "No file extension found");
        return;
    } else {
        extension = parameter + lastDot;
        DBG("%s EXTENSION: %s\n", parameter, extension);
    }

    /* determine mime-type */
    for(i = 0; i < LENGTH_OF(mimetypes); i++) {
        if(strcmp(mimetypes[i].dot_extension, extension) == 0) {
            mimetype = (char *)mimetypes[i].mimetype;
            break;
        }
    }

    /* in case of unknown mimetype or extension leave */
    if(mimetype == NULL) {
        send_error(fd, 404, "MIME-TYPE not known");
        return;
    }

    /* now filename, mimetype and extension are known */
    DBG("trying to serve file \"%s\", extension: \"%s\" mime: \"%s\"\n", parameter, extension, mimetype);

    /* build the absolute path to the file */
    strncat(buffer, conf.www_folder, sizeof(buffer) - 1);
    strncat(buffer, parameter, sizeof(buffer) - strlen(buffer) - 1);

    /* try to open that file */
    if((lfd = open(buffer, O_RDONLY)) < 0) {
        DBG("file %s not accessible\n", buffer);
        send_error(fd, 404, "Could not open file");
        return;
    }
    DBG("opened file: %s\n", buffer);

    /* prepare HTTP header */
    sprintf(buffer, "HTTP/1.0 200 OK\r\n" \
            "Content-type: %s\r\n" \
            STD_HEADER \
            "\r\n", mimetype);
    i = strlen(buffer);

    /* first transmit HTTP-header, afterwards transmit content of file */
    do {
        if(write(fd, buffer, i) < 0) {
            close(lfd);
            return;
        }
    } while((i = read(lfd, buffer, sizeof(buffer))) > 0);

    /* close file, job done */
    close(lfd);
}

/******************************************************************************
Description.: Serve a connected TCP-client. This thread function is called
              for each connect of a HTTP client like a webbrowser. It determines
              if it is a valid HTTP request and dispatches between the different
              response options.
Input Value.: arg is the filedescriptor and server-context of the connected TCP
              socket. It must have been allocated so it is freeable by this
              thread function.
Return Value: always NULL
******************************************************************************/
/* thread for clients that connected to this server */
void *client_thread(void *arg)
{
    int cnt;
    int query_suffixed = 0;
    int input_number = 0;
    char buffer[BUFFER_SIZE] = {0}, *pb = buffer;
    iobuffer iobuf;
    request req;
    cfd lcfd; /* local-connected-file-descriptor */

    /* we really need the fildescriptor and it must be freeable by us */
    if(arg != NULL) {
        memcpy(&lcfd, arg, sizeof(cfd));
        free(arg);
    } else
        return NULL;

    /* initializes the structures */
    init_iobuffer(&iobuf);
    init_request(&req);

    /* What does the client want to receive? Read the request. */
    memset(buffer, 0, sizeof(buffer));
    if((cnt = _readline(lcfd.fd, &iobuf, buffer, sizeof(buffer) - 1, 5)) == -1) {
        close(lcfd.fd);
        return NULL;
    }

    req.query_string = NULL;

    /* determine what to deliver - new short paths */
    if(parse_short_path(buffer, "snapshot", &input_number)) {
        req.type = A_SNAPSHOT;
        query_suffixed = 255;
        if (check_and_handle_client_status(&lcfd, &req, &query_suffixed)) {
            close(lcfd.fd);
            return NULL;
        }
    } else if(parse_short_path(buffer, "stream", &input_number)) {
        req.type = A_STREAM;
        query_suffixed = 255;
        if (check_and_handle_client_status(&lcfd, &req, &query_suffixed)) {
            close(lcfd.fd);
            return NULL;
        }
    } else if(parse_short_path(buffer, "take", &input_number)) {
        req.type = A_TAKE;
        query_suffixed = 255;
        // Parse parameters after ? or end of path
        const char *pb = strstr(buffer, "GET /take");
        if (pb == NULL) {
            pb = strstr(buffer, "POST /take");
        }
        if (pb != NULL) {
            if (strncmp(pb, "GET", 3) == 0) {
                pb += strlen("GET /take");
            } else {
                pb += strlen("POST /take");
            }
            // Skip number if present
            while (*pb >= '0' && *pb <= '9') pb++;
            if (*pb == '?') {
                pb++; // Skip ?
                int len = MIN(MAX(strspn(pb, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_-=&1234567890%./"), 0), 100);
                req.parameter = malloc(len + 1);
                if (req.parameter == NULL) {
                    send_error(lcfd.fd, 500, "could not allocate memory");
            close(lcfd.fd);
            return NULL;
        }
        memset(req.parameter, 0, len + 1);
        strncpy(req.parameter, pb, len);
                if (unescape(req.parameter) == -1) {
            free(req.parameter);
                    send_error(lcfd.fd, 500, "could not properly parse parameter string");
            close(lcfd.fd);
            return NULL;
        }
            }
        }
    } else {
        DBG("try to serve a file\n");
        req.type = A_FILE;

        if((pb = strstr(buffer, "GET /")) == NULL) {
            DBG("HTTP request seems to be malformed\n");
            send_error(lcfd.fd, 400, "Malformed HTTP request");
            close(lcfd.fd);
            return NULL;
        }

        pb += strlen("GET /");
        int len = MIN(MAX(strspn(pb, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ._-1234567890"), 0), 100);
        req.parameter = malloc(len + 1);
        if(req.parameter == NULL) {
            exit(EXIT_FAILURE);
        }
        memset(req.parameter, 0, len + 1);
        strncpy(req.parameter, pb, len);

        DBG("parameter (len: %d): \"%s\"\n", len, req.parameter);
    }

    /*
     * For short paths, input_number is already set by parse_short_path
     * For compatibility with old format, check for _[plugin number suffix]
     */
    if(query_suffixed && input_number == 0) {
        char *sch = strchr(buffer, '_');
        if(sch != NULL) {  // there is an _ in the url so the input number should be present
            DBG("Suffix character: %s\n", sch + 1); // FIXME if more than 10 input plugin is added
            char numStr[3];
            memset(numStr, 0, 3);
            strncpy(numStr, sch + 1, 1);
            input_number = atoi(numStr);

        }
        DBG("plugin_no: %d\n", input_number);
    }

    /*
     * parse the rest of the HTTP-request
     * the end of the request-header is marked by a single, empty line with "\r\n"
     */
    do {
        memset(buffer, 0, sizeof(buffer));

        if((cnt = _readline(lcfd.fd, &iobuf, buffer, sizeof(buffer) - 1, 5)) == -1) {
            free_request(&req);
            close(lcfd.fd);
            return NULL;
        }

        if(strcasestr(buffer, "User-Agent: ") != NULL) {
            req.client = strdup(buffer + strlen("User-Agent: "));
        } else if(strcasestr(buffer, "Authorization: Basic ") != NULL) {
            req.credentials = strdup(buffer + strlen("Authorization: Basic "));
            decodeBase64(req.credentials);
            DBG("username:password: %s\n", req.credentials);
        }

    } while(cnt > 2 && !(buffer[0] == '\r' && buffer[1] == '\n'));

    /* check for username and password if parameter -c was given */
    if(lcfd.pc->conf.credentials != NULL) {
        if(req.credentials == NULL || strcmp(lcfd.pc->conf.credentials, req.credentials) != 0) {
            DBG("access denied\n");
            send_error(lcfd.fd, 401, "username and password do not match to configuration");
            close(lcfd.fd);
            free_request(&req);
            return NULL;
        }
        DBG("access granted\n");
    }

    /* now it's time to answer */
    if (query_suffixed) {
            if(!(input_number < pglobal->incnt)) {
                DBG("Input number: %d out of range (valid: 0..%d)\n", input_number, pglobal->incnt-1);
                send_error(lcfd.fd, 404, "Invalid input plugin number");
                req.type = A_UNKNOWN;
        }
    }

    switch(req.type) {
    case A_SNAPSHOT:
        DBG("Request for snapshot from input: %d\n", input_number);
        send_snapshot(&lcfd, input_number);
        break;
    case A_STREAM:
        DBG("Request for stream from input: %d\n", input_number);
        send_stream(&lcfd, input_number);
        break;
    case A_FILE:
        if(lcfd.pc->conf.www_folder == NULL)
            send_error(lcfd.fd, 501, "no www-folder configured");
        else
            send_file(lcfd.pc->id, lcfd.fd, req.parameter);
        break;
    /*
        With the take argument we try to save the current image to file before we transmit it to the user.
        This is done trough the output_file plugin.
        If it not loaded, or the file could not be saved then we won't transmit the frame.
    */
    case A_TAKE: {
        int i, ret = 0, found = 0;
        for (i = 0; i<pglobal->outcnt; i++) {
            if (pglobal->out[i].name != NULL) {
                if (strstr(pglobal->out[i].name, "FILE output plugin")) {
                    found = 255;
                    DBG("output_file found id: %d\n", i);
                    char *filename = NULL;
                    char *filenamearg = NULL;
                    int len = 0;
                    DBG("Buffer: %s \n", req.parameter);
                    if((filename = strstr(req.parameter, "filename=")) != NULL) {
                        filename += strlen("filename=");
                        char *fn = strchr(filename, '&');
                        if (fn == NULL)
                            len = strlen(filename);
                        else
                            len = (int)(fn - filename);
                        filenamearg = (char*)calloc(len, sizeof(char));
                        memcpy(filenamearg, filename, len);
                        DBG("Filename = %s\n", filenamearg);
                        //int output_cmd(int plugin_id, unsigned int control_id, unsigned int group, int value, char *valueStr)
                        ret = pglobal->out[i].cmd(i, OUT_FILE_CMD_TAKE, IN_CMD_GENERIC, 0, filenamearg);
                    } else {
                        DBG("filename is not specified int the URL\n");
                        send_error(lcfd.fd, 404, "The &filename= must present for the take command in the URL");
                    }
                    break;
                }
            }
        }

        if (found == 0) {
            LOG("FILE CHANGE TEST output plugin not loaded\n");
            send_error(lcfd.fd, 404, "FILE output plugin not loaded, taking snapshot not possible");
        } else {
            if (ret == 0) {
                send_snapshot(&lcfd, input_number);
            } else {
                send_error(lcfd.fd, 404, "Taking snapshot failed!");
            }
        }
        } break;
    default:
        DBG("unknown request\n");
    }

    close(lcfd.fd);
    
    free_request(&req);

    DBG("leaving HTTP client thread\n");
    return NULL;
}

/******************************************************************************
Description.: This function cleans up resources allocated by the server_thread
Input Value.: arg is not used
Return Value: -
******************************************************************************/
void server_cleanup(void *arg)
{
    context *pcontext = arg;
    int i;

    OPRINT("cleaning up resources allocated by server thread #%02d\n", pcontext->id);

    for(i = 0; i < MAX_SD_LEN; i++)
        close(pcontext->sd[i]);
}

/******************************************************************************
Description.: Open a TCP socket and wait for clients to connect. If clients
              connect, start a new thread for each accepted connection.
Input Value.: arg is a pointer to the globals struct
Return Value: always NULL, will only return on exit
******************************************************************************/
void *server_thread(void *arg)
{
    int on;
    pthread_t client;
    struct addrinfo *aip, *aip2;
    struct addrinfo hints;
    struct sockaddr_storage client_addr;
    socklen_t addr_len = sizeof(struct sockaddr_storage);
    fd_set selectfds;
    int max_fds = 0;
    char name[NI_MAXHOST];
    int err;
    int i;

    context *pcontext = arg;
    pglobal = pcontext->pglobal;

    /* Initialize SIMD capabilities on first server start */
    static int simd_initialized = 0;
    if (!simd_initialized) {
        detect_simd_capabilities();
        simd_initialized = 1;
    }

    /* set cleanup handler to cleanup resources */
    pthread_cleanup_push(server_cleanup, pcontext);

    bzero(&hints, sizeof(hints));
    hints.ai_family = PF_UNSPEC;
    hints.ai_flags = AI_PASSIVE;
    hints.ai_socktype = SOCK_STREAM;

    snprintf(name, sizeof(name), "%d", ntohs(pcontext->conf.port));
    if((err = getaddrinfo(pcontext->conf.hostname, name, &hints, &aip)) != 0) {
        perror(gai_strerror(err));
        exit(EXIT_FAILURE);
    }

    for(i = 0; i < MAX_SD_LEN; i++)
        pcontext->sd[i] = -1;

    /* open sockets for server (1 socket / address family) */
    i = 0;
    for(aip2 = aip; aip2 != NULL; aip2 = aip2->ai_next) {
        if((pcontext->sd[i] = socket(aip2->ai_family, aip2->ai_socktype, 0)) < 0) {
            continue;
        }

        /* ignore "socket already in use" errors */
        on = 1;
        if(setsockopt(pcontext->sd[i], SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
            perror("setsockopt(SO_REUSEADDR) failed\n");
        }

        /* IPv6 socket should listen to IPv6 only, otherwise we will get "socket already in use" */
        on = 1;
        if(aip2->ai_family == AF_INET6 && setsockopt(pcontext->sd[i], IPPROTO_IPV6, IPV6_V6ONLY,
                (const void *)&on , sizeof(on)) < 0) {
            perror("setsockopt(IPV6_V6ONLY) failed\n");
        }

        /* perhaps we will use this keep-alive feature oneday */
        /* setsockopt(sd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on)); */

        if(bind(pcontext->sd[i], aip2->ai_addr, aip2->ai_addrlen) < 0) {
            perror("bind");
            pcontext->sd[i] = -1;
            continue;
        }

        if(listen(pcontext->sd[i], 10) < 0) {
            perror("listen");
            pcontext->sd[i] = -1;
        } else {
            /* Add server socket to epoll for async I/O */
            if (add_server_socket(&pcontext->async_io, pcontext->sd[i]) < 0) {
                perror("add_server_socket");
                close(pcontext->sd[i]);
                pcontext->sd[i] = -1;
            } else {
                i++;
                if(i >= MAX_SD_LEN) {
                    OPRINT("%s(): maximum number of server sockets exceeded", __FUNCTION__);
                    i--;
                    break;
                }
            }
        }
    }

    pcontext->sd_len = i;

    if(pcontext->sd_len < 1) {
        OPRINT("%s(): bind(%d) failed\n", __FUNCTION__, htons(pcontext->conf.port));
        closelog();
        exit(EXIT_FAILURE);
    }

    /* create a child for every client that connects */
    while(!pglobal->stop) {
        //int *pfd = (int *)malloc(sizeof(int));
        cfd *pcfd = malloc(sizeof(cfd));

        if(pcfd == NULL) {
            fprintf(stderr, "failed to allocate (a very small amount of) memory\n");
            exit(EXIT_FAILURE);
        }

        DBG("waiting for clients to connect\n");
        
        /* Check for stop signal before waiting */
        if(pglobal->stop) {
            free(pcfd);
            break;
        }

#ifdef __linux__
        /* Use epoll for better performance with timeout for signal handling */
        int nfds = epoll_wait(pcontext->async_io.epfd, pcontext->async_io.events, 
                             pcontext->async_io.max_events, EPOLL_TIMEOUT_MS);
        
        if(nfds < 0) {
            if(errno == EINTR) continue;
            perror("epoll_wait");
            exit(EXIT_FAILURE);
        }
        
        /* Check for stop signal if no events */
        if(nfds == 0) {
            if(pglobal->stop) {
                DBG("stop signal received, exiting server thread\n");
                break;
            }
            continue;
        }
        
        /* Process all ready events */
        for(int n = 0; n < nfds; n++) {
            int ready_fd = pcontext->async_io.events[n].data.fd;
            
            /* Check if this is a server socket */
            for(i = 0; i < MAX_SD_LEN; i++) {
                if(pcontext->sd[i] == ready_fd) {
                    pcfd->fd = accept(ready_fd, (struct sockaddr *)&client_addr, &addr_len);
                    pcfd->pc = pcontext;
#else
        /* Fallback to select for non-Linux systems */
        do {
            FD_ZERO(&selectfds);

            for(i = 0; i < MAX_SD_LEN; i++) {
                if(pcontext->sd[i] != -1) {
                    FD_SET(pcontext->sd[i], &selectfds);

                    if(pcontext->sd[i] > max_fds)
                        max_fds = pcontext->sd[i];
                }
            }

            err = select(max_fds + 1, &selectfds, NULL, NULL, NULL);

            if(err < 0 && errno != EINTR) {
                perror("select");
                exit(EXIT_FAILURE);
            }
        } while(err <= 0);

        for(i = 0; i < max_fds + 1; i++) {
            if(pcontext->sd[i] != -1 && FD_ISSET(pcontext->sd[i], &selectfds)) {
                pcfd->fd = accept(pcontext->sd[i], (struct sockaddr *)&client_addr, &addr_len);
                pcfd->pc = pcontext;
#endif

                    /* start new thread that will handle this TCP connected client */
                    DBG("create thread to handle client that just established a connection\n");

                    if(getnameinfo((struct sockaddr *)&client_addr, addr_len, name, sizeof(name), NULL, 0, NI_NUMERICHOST) == 0) {
                        DBG("serving client: %s\n", name);
                    }

                    if(pthread_create(&client, NULL, &client_thread, pcfd) != 0) {
                        DBG("could not launch another client thread\n");
                        close(pcfd->fd);
                        free(pcfd);
                        continue;
                    }
                    pthread_detach(client);
                    pcfd = NULL; /* Prevent double-free, pcfd now owned by client_thread */
#ifdef __linux__
                    break; /* Found and processed the server socket */
                }
            }
        }
#else
                }
            }
#endif
        
        /* Free pcfd if no connection was accepted */
        if (pcfd != NULL) {
            free(pcfd);
        }
        
        /* Check for stop signal after processing events */
        if(pglobal->stop) {
            DBG("stop signal received, exiting server thread\n");
            break;
        }
    }

    DBG("leaving server thread, calling cleanup function now\n");
    pthread_cleanup_pop(1);

    return NULL;
}

