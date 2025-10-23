/*******************************************************************************
# RTSP HTTP Proxy for MJPG-streamer                                          #
#                                                                              #
# This plugin implements a simple RTSP server that proxies to HTTP stream     #
# It provides RTSP protocol but streams via HTTP internally                   #
#                                                                              #
# Copyright (C) 2024                                                           #
#                                                                              #
# This program is free software; you can redistribute it and/or modify         #
# it under the terms of the GNU General Public License as published by         #
# the Free Software Foundation; either version 2 of the License, or            #
# (at your option) any later version.                                          #
#                                                                              #
# This program is distributed in the hope that it will be useful,              #
# but WITHOUT ANY WARRANTY; without even the implied warranty of               #
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the                #
# GNU General Public License for more details.                                #
#                                                                              #
# You should have received a copy of the GNU General Public License            #
# along with this program; if not, write to the Free Software                  #
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA    #
#                                                                              #
*******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <sys/time.h>
#include <getopt.h>
#include <limits.h>

#include "../../utils.h"
#include "../../mjpg_streamer.h"

#define OUTPUT_PLUGIN_NAME "RTSP HTTP Proxy"
#define RTSP_VERSION "1.0"
#define MAX_CLIENTS 5

/* Client connection */
typedef struct {
    int socket;
    struct sockaddr_in address;
    int active;
    pthread_t thread;
} rtsp_client_t;

/* Global variables */
static globals *pglobal;
static int rtsp_port = 554;
static int input_number = 0;
static int server_socket;
static rtsp_client_t clients[MAX_CLIENTS];
static pthread_t server_thread;
static int server_running = 0;

/* Function prototypes */
void *rtsp_server_thread(void *arg);
void *rtsp_client_thread(void *arg);
int handle_rtsp_request(rtsp_client_t *client, char *request);
int send_rtsp_response(rtsp_client_t *client, int code, char *message);
void help(void);

/******************************************************************************
Description.: print a help message
Input Value.: -
Return Value: -
******************************************************************************/
void help(void)
{
    fprintf(stderr, " ---------------------------------------------------------------\n" \
            " Help for output plugin..: "OUTPUT_PLUGIN_NAME"\n" \
            " ---------------------------------------------------------------\n" \
            " The following parameters can be passed to this plugin:\n\n" \
            " [-p | --port ]..........: RTSP server port (default: 554)\n" \
            " [-i | --input ]........: input plugin number (default: 0)\n" \
            " [-h | --help ].........: show this help\n\n" \
            " ---------------------------------------------------------------\n");
}

/******************************************************************************
Description.: Handle RTSP request
Input Value.: client, request
Return Value: 0 on success, -1 on error
******************************************************************************/
int handle_rtsp_request(rtsp_client_t *client, char *request)
{
    char method[16], uri[256], version[16];
    char response[1024];
    int session_id = rand() % 1000000;
    int cseq = 1;
    
    /* Parse CSeq from request */
    char *cseq_line = strstr(request, "CSeq:");
    if (cseq_line) {
        sscanf(cseq_line, "CSeq: %d", &cseq);
    }
    
    /* Parse request */
    if (sscanf(request, "%15s %255s %15s", method, uri, version) != 3) {
        return send_rtsp_response(client, 400, "Bad Request");
    }
    
    if (strcmp(method, "OPTIONS") == 0) {
        snprintf(response, sizeof(response),
                "RTSP/%s 200 OK\r\n"
                "CSeq: %d\r\n"
                "Public: OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN\r\n"
                "Server: MJPG-Streamer RTSP Server\r\n"
                "\r\n", RTSP_VERSION, cseq);
        return send(client->socket, response, strlen(response), 0);
    }
    
    if (strcmp(method, "DESCRIBE") == 0) {
        char sdp[1024];
        snprintf(sdp, sizeof(sdp),
                "v=0\r\n"
                "o=- 0 0 IN IP4 127.0.0.1\r\n"
                "s=MJPG-Streamer Session\r\n"
                "c=IN IP4 127.0.0.1\r\n"
                "t=0 0\r\n"
                "m=video 0 RTP/AVP 26\r\n"
                "a=rtpmap:26 JPEG/90000\r\n"
                "a=control:track0\r\n");
        
        snprintf(response, sizeof(response),
                "RTSP/%s 200 OK\r\n"
                "CSeq: %d\r\n"
                "Content-Type: application/sdp\r\n"
                "Content-Length: %zu\r\n"
                "\r\n"
                "%s", RTSP_VERSION, cseq, strlen(sdp), sdp);
        return send(client->socket, response, strlen(response), 0);
    }
    
    if (strcmp(method, "SETUP") == 0) {
        snprintf(response, sizeof(response),
                "RTSP/%s 200 OK\r\n"
                "CSeq: %d\r\n"
                "Session: %d\r\n"
                "Transport: RTP/AVP;unicast;client_port=5004-5005\r\n"
                "\r\n", RTSP_VERSION, cseq, session_id);
        return send(client->socket, response, strlen(response), 0);
    }
    
    if (strcmp(method, "PLAY") == 0) {
        /* Redirect to HTTP stream */
        snprintf(response, sizeof(response),
                "RTSP/%s 302 Found\r\n"
                "CSeq: %d\r\n"
                "Location: http://127.0.0.1:8080/?action=stream\r\n"
                "Server: MJPG-Streamer RTSP Server\r\n"
                "\r\n", RTSP_VERSION, cseq);
        return send(client->socket, response, strlen(response), 0);
    }
    
    if (strcmp(method, "TEARDOWN") == 0) {
        client->active = 0;
        snprintf(response, sizeof(response),
                "RTSP/%s 200 OK\r\n"
                "CSeq: %d\r\n"
                "Session: %d\r\n"
                "\r\n", RTSP_VERSION, cseq, session_id);
        return send(client->socket, response, strlen(response), 0);
    }
    
    return send_rtsp_response(client, 501, "Not Implemented");
}

/******************************************************************************
Description.: Send RTSP response
Input Value.: client, code, message
Return Value: 0 on success, -1 on error
******************************************************************************/
int send_rtsp_response(rtsp_client_t *client, int code, char *message)
{
    char response[512];
    snprintf(response, sizeof(response),
            "RTSP/%s %d %s\r\n"
            "CSeq: 1\r\n"
            "Server: MJPG-Streamer RTSP Server\r\n"
            "\r\n", RTSP_VERSION, code, message);
    return send(client->socket, response, strlen(response), 0);
}

/******************************************************************************
Description.: RTSP client thread
Input Value.: client
Return Value: NULL
******************************************************************************/
void *rtsp_client_thread(void *arg)
{
    rtsp_client_t *client = (rtsp_client_t*)arg;
    char buffer[4096];
    int bytes_received;
    
    OPRINT("RTSP client connected from %s:%d\n", 
           inet_ntoa(client->address.sin_addr), ntohs(client->address.sin_port));
    
    while (client->active && !pglobal->stop) {
        bytes_received = recv(client->socket, buffer, sizeof(buffer) - 1, 0);
        if (bytes_received <= 0) {
            break;
        }
        
        buffer[bytes_received] = '\0';
        handle_rtsp_request(client, buffer);
    }
    
    OPRINT("RTSP client disconnected\n");
    close(client->socket);
    client->active = 0;
    return NULL;
}

/******************************************************************************
Description.: RTSP server thread
Input Value.: unused
Return Value: NULL
******************************************************************************/
void *rtsp_server_thread(void *arg)
{
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_socket;
    int i;
    
    /* Setup server socket */
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        OPRINT("Failed to create server socket\n");
        return NULL;
    }
    
    int opt = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(rtsp_port);
    
    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        OPRINT("Failed to bind to port %d\n", rtsp_port);
        close(server_socket);
        return NULL;
    }
    
    if (listen(server_socket, MAX_CLIENTS) < 0) {
        OPRINT("Failed to listen on port %d\n", rtsp_port);
        close(server_socket);
        return NULL;
    }
    
    OPRINT("RTSP server listening on port %d\n", rtsp_port);
    
    /* Initialize clients */
    for (i = 0; i < MAX_CLIENTS; i++) {
        clients[i].active = 0;
    }
    
    while (server_running && !pglobal->stop) {
        client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &client_len);
        if (client_socket < 0) {
            if (errno == EINTR) continue;
            break;
        }
        
        /* Find free client slot */
        for (i = 0; i < MAX_CLIENTS; i++) {
            if (!clients[i].active) {
                clients[i].socket = client_socket;
                clients[i].address = client_addr;
                clients[i].active = 1;
                
                pthread_create(&clients[i].thread, NULL, rtsp_client_thread, &clients[i]);
                pthread_detach(clients[i].thread);
                break;
            }
        }
        
        if (i >= MAX_CLIENTS) {
            OPRINT("Maximum clients reached\n");
            close(client_socket);
        }
    }
    
    close(server_socket);
    return NULL;
}

/*** plugin interface functions ***/
/******************************************************************************
Description.: Initialize RTSP output plugin
Input Value.: parameters
Return Value: 0 if everything is ok, non-zero otherwise
******************************************************************************/
int output_init(output_parameter *param)
{
    int i;
    
    param->argv[0] = OUTPUT_PLUGIN_NAME;
    
    /* Parse parameters */
    for(i = 0; i < param->argc; i++) {
        DBG("argv[%d]=%s\n", i, param->argv[i]);
    }
    
    reset_getopt();
    while(1) {
        int option_index = 0, c = 0;
        static struct option long_options[] = {
            {"port", required_argument, 0, 'p'},
            {"input", required_argument, 0, 'i'},
            {"help", no_argument, 0, 'h'},
            {0, 0, 0, 0}
        };
        
        c = getopt_long(param->argc, param->argv, "p:i:h", long_options, &option_index);
        if(c == -1) break;
        
        switch(c) {
            case 'p':
                rtsp_port = atoi(optarg);
                break;
            case 'i':
                input_number = atoi(optarg);
                break;
            case 'h':
                help();
                return 1;
        }
    }
    
    pglobal = param->global;
    if(input_number >= pglobal->incnt) {
        OPRINT("ERROR: input plugin %d not available (only %d loaded)\n", 
               input_number, pglobal->incnt);
        return 1;
    }
    
    OPRINT("RTSP server port: %d\n", rtsp_port);
    OPRINT("Input plugin: %d\n", input_number);
    
    return 0;
}

/******************************************************************************
Description.: Start RTSP server
Input Value.: id
Return Value: 0 on success
******************************************************************************/
int output_run(int id)
{
    server_running = 1;
    
    /* Start server thread */
    if (pthread_create(&server_thread, NULL, rtsp_server_thread, NULL) != 0) {
        OPRINT("Failed to create server thread\n");
        return -1;
    }
    
    return 0;
}

/******************************************************************************
Description.: Stop RTSP server
Input Value.: id
Return Value: 0 on success
******************************************************************************/
int output_stop(int id)
{
    server_running = 0;
    
    /* Close all client connections */
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active) {
            clients[i].active = 0;
            close(clients[i].socket);
        }
    }
    
    /* Stop server thread */
    pthread_join(server_thread, NULL);
    
    return 0;
}
