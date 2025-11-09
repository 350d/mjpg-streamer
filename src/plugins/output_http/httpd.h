/*******************************************************************************
#                                                                              #
#      MJPG-streamer allows to stream JPG frames from an input-plugin          #
#      to several output plugins                                               #
#                                                                              #
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

#ifdef __linux__
#include <sys/epoll.h>
#endif
#include <stddef.h>

#define IO_BUFFER 256
#define BUFFER_SIZE 1024

/* epoll constants for async I/O */
#define MAX_EPOLL_EVENTS 64
#define EPOLL_TIMEOUT_MS 1000

/* the boundary is used for the M-JPEG stream, it separates the multipart stream of pictures */
#define BOUNDARY "boundarydonotcross"

/*
 * this defines the buffer size for a JPG-frame
 * selecting to large values will allocate much wasted RAM for each buffer
 * selecting to small values will lead to crashes due to to small buffers
 */
#define MAX_FRAME_SIZE (256*1024)
#define TEN_K (10*1024)

/*
 * Standard header to be send along with other header information like mimetype.
 *
 * The parameters should ensure the browser does not cache our answer.
 * A browser should connect for each file and not serve files from his cache.
 * Using cached pictures would lead to showing old/outdated pictures
 * Many browser seem to ignore, or at least not always obey those headers
 * since i observed caching of files from time to time.
 */
#define STD_HEADER "Connection: close\r\n" \
    "Server: MJPG-Streamer/0.2\r\n" \
    "Cache-Control: no-store, no-cache, must-revalidate, pre-check=0, post-check=0, max-age=0\r\n" \
    "Pragma: no-cache\r\n" \
    "Expires: Mon, 3 Jan 2000 12:34:56 GMT\r\n"

/* Keep-Alive header for persistent connections */
#define KEEP_ALIVE_HEADER "Connection: keep-alive\r\n" \
    "Keep-Alive: timeout=5, max=100\r\n" \
    "Server: MJPG-Streamer/0.2\r\n" \
    "Cache-Control: no-store, no-cache, must-revalidate, pre-check=0, post-check=0, max-age=0\r\n" \
    "Pragma: no-cache\r\n" \
    "Expires: Mon, 3 Jan 2000 12:34:56 GMT\r\n"

/*
 * Maximum number of server sockets (i.e. protocol families) to listen.
 */
#define MAX_SD_LEN 50

/* epoll async I/O context */
typedef struct {
    int epfd;
    struct epoll_event *events;
    int max_events;
    int client_count;
    int server_sockets[MAX_SD_LEN];
    int server_socket_count;
} async_io_context;

/* Cached HTTP header */
typedef struct {
    char *data;
    size_t len;
    int timestamp_pos;  /* Position for timestamp insertion (-1 if no timestamp) */
    int timestamp_len;  /* Length of timestamp placeholder */
    int framerate_pos;  /* Position for framerate insertion (-1 if no framerate) */
    int framerate_len;  /* Length of framerate placeholder */
    int content_length_pos;  /* Position for content-length insertion (-1 if no content-length) */
} cached_header;

/* HTTP header cache */
typedef struct {
    cached_header snapshot_200;
    cached_header stream_200;
    cached_header error_400;
    cached_header error_401;
    cached_header error_403;
    cached_header error_404;
    cached_header error_500;
    cached_header error_501;
    cached_header json_200;
    int initialized;
} header_cache;

/*
 * Only the following fileypes are supported.
 *
 * Other filetypes are simply ignored!
 * This table is a 1:1 mapping of files extension to a certain mimetype.
 */
static const struct {
    const char *dot_extension;
    const char *mimetype;
} mimetypes[] = {
    { ".html", "text/html" },
    { ".htm",  "text/html" },
    { ".css",  "text/css" },
    { ".js",   "text/javascript" },
    { ".txt",  "text/plain" },
    { ".jpg",  "image/jpeg" },
    { ".jpeg", "image/jpeg" },
    { ".png",  "image/png"},
    { ".gif",  "image/gif" },
    { ".ico",  "image/x-icon" },
    { ".swf",  "application/x-shockwave-flash" },
    { ".cab",  "application/x-shockwave-flash" },
    { ".jar",  "application/java-archive" },
    { ".json", "application/json" }
};

/* the webserver determines between these values for an answer */
typedef enum {
    A_UNKNOWN,
    A_SNAPSHOT,
    A_STREAM,
    A_FILE,
    A_CGI,
    A_TAKE
} answer_t;

/*
 * the client sends information with each request
 * this structure is used to store the important parts
 */
typedef struct {
    answer_t type;
    char *parameter;
    char *client;
    char *credentials;
    char *query_string;
} request;

/* the iobuffer structure is used to read from the HTTP-client */
typedef struct {
    int level;              /* how full is the buffer */
    char buffer[IO_BUFFER]; /* the data */
} iobuffer;

/* store configuration for each server instance */
typedef struct {
    int port;
    char *hostname;
    char *credentials;
    char *www_folder;
} config;

/* Write buffer for I/O optimization */
typedef struct {
    char buffer[BUFFER_SIZE * 4];  /* 4KB write buffer */
    size_t buffer_pos;
    int fd;
    int use_buffering;
} write_buffer;

/* context of each server thread */
typedef struct {
    int sd[MAX_SD_LEN];
    int sd_len;
    int id;
    globals *pglobal;
    pthread_t threadID;

    config conf;
    
    /* Performance optimization: static buffers */
    unsigned char static_frame_buffer[MAX_FRAME_SIZE];
    unsigned char static_header_buffer[BUFFER_SIZE];
    int use_static_buffers;
    size_t current_buffer_size;
    
    /* I/O optimization: write buffering */
    write_buffer write_buf;
    
    /* Stage 3 optimizations: async I/O and header caching */
    async_io_context async_io;
    header_cache headers;
} context;



/*
 * this struct is just defined to allow passing all necessary details to a worker thread
 * "cfd" is for connected/accepted filedescriptor
 */
typedef struct {
    context *pc;
    int fd;
} cfd;



/* prototypes */
void *server_thread(void *arg);
void send_error(int fd, int which, char *message);









