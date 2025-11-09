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
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <getopt.h>
#include <pthread.h>
#include <syslog.h>

#ifdef __linux__
#include <linux/types.h>          /* for videodev2.h */
#endif
#ifdef __linux__
#include <linux/videodev2.h>
#endif

#include "../../mjpg_streamer.h"
#include "../../utils.h"
#include "httpd.h"

/* Forward declaration */
void *worker_thread(void *arg);

/* Global variables */
static int input_number = 0;

/* Forward declarations for Stage 3 functions */
int init_async_io(async_io_context *ctx, int max_events);
void cleanup_async_io(async_io_context *ctx);

#define OUTPUT_PLUGIN_NAME "HTTP output plugin"

/*
 * keep context for each server
 */
context servers[MAX_OUTPUT_PLUGINS];

/******************************************************************************
Description.: print help for this plugin to stdout
Input Value.: -
Return Value: -
******************************************************************************/
void help(void)
{
    fprintf(stderr, " ---------------------------------------------------------------\n" \
            " Help for output plugin..: "OUTPUT_PLUGIN_NAME"\n" \
            " ---------------------------------------------------------------\n" \
            " The following parameters can be passed to this plugin:\n\n" \
            " [-w | --www ]...........: folder that contains webpages in \n" \
            "                           flat hierarchy (no subfolders)\n" \
            " [-p | --port ]..........: TCP port for this HTTP server\n" \
	        " [-l ] --listen ]........: Listen on Hostname / IP\n" \
            " [-c | --credentials ]...: ask for \"username:password\" on connect\n" \
            " [-i | --input ]........: input plugin number (default: 0)\n"
            " ---------------------------------------------------------------\n");
}

/*** plugin interface functions ***/
/******************************************************************************
Description.: Initialize this plugin.
              parse configuration parameters,
              store the parsed values in global variables
Input Value.: All parameters to work with.
              Among many other variables the "param->id" is quite important -
              it is used to distinguish between several server instances
Return Value: 0 if everything is OK, other values signal an error
******************************************************************************/
int output_init(output_parameter *param, int id)
{
    int i;
    int  port;
    char *credentials, *www_folder, *hostname = NULL;

    DBG("output #%02d\n", param->id);

    port = htons(8080);
    credentials = NULL;
    www_folder = NULL;

    param->argv[0] = OUTPUT_PLUGIN_NAME;
    

    /* show all parameters for DBG purposes */
    for(i = 0; i < param->argc; i++) {
        DBG("argv[%d]=%s\n", i, param->argv[i]);
    }

    reset_getopt();
    while(1) {
        int option_index = 0, c = 0;
        static struct option long_options[] = {
            {"h", no_argument, 0, 0
            },
            {"help", no_argument, 0, 0},
            {"p", required_argument, 0, 0},
            {"port", required_argument, 0, 0},
            {"l", required_argument , 0, 0},
	    {"listen", required_argument, 0, 0},
            {"c", required_argument, 0, 0},
            {"credentials", required_argument, 0, 0},
            {"w", required_argument, 0, 0},
            {"www", required_argument, 0, 0},
            {"i", required_argument, 0, 0},
            {"input", required_argument, 0, 0},
            {0, 0, 0, 0}
        };

        c = getopt_long_only(param->argc, param->argv, "", long_options, &option_index);

        /* no more options to parse */
        if(c == -1) break;

        /* unrecognized option */
        if(c == '?') {
            help();
            return 1;
        }

        switch(option_index) {
            /* h, help */
        case 0:
        case 1:
            DBG("case 0,1\n");
            help();
            return 1;
            break;

            /* p, port */
        case 2:
        case 3:
            DBG("case 2,3\n");
            port = htons(atoi(optarg));
            break;
       
            /* Interface name */
	case 4:
	case 5:
            DBG("case 4,5\n");
	    hostname = strdup(optarg);
	    break;

            /* c, credentials */
        case 6:
        case 7:
            DBG("case 6,7\n");
            credentials = strdup(optarg);
            break;

            /* w, www */
        case 8:
        case 9:
            DBG("case 8,9\n");
            www_folder = malloc(strlen(optarg) + 2);
            strcpy(www_folder, optarg);
            if(optarg[strlen(optarg)-1] != '/')
                strcat(www_folder, "/");
            break;

            /* i, input */
        case 10:
        case 11:
            DBG("case 12,13\n");
            input_number = atoi(optarg);
            break;
        }
    }

    servers[param->id].id = param->id;
    servers[param->id].pglobal = param->global;
    servers[param->id].conf.port = port;
    servers[param->id].conf.hostname = hostname;
    servers[param->id].conf.credentials = credentials;
    servers[param->id].conf.www_folder = www_folder;
    
    /* Initialize static buffers for performance optimization */
    servers[param->id].use_static_buffers = 1;
    servers[param->id].current_buffer_size = 0;
    memset(servers[param->id].static_frame_buffer, 0, MAX_FRAME_SIZE);
    memset(servers[param->id].static_header_buffer, 0, BUFFER_SIZE);
    
    /* Initialize write buffer for I/O optimization */
    servers[param->id].write_buf.buffer_pos = 0;
    servers[param->id].write_buf.fd = -1;  /* Will be set when client connects */
    
    /* Validate input plugin number */
    if(!(input_number < param->global->incnt)) {
        OPRINT("ERROR: the %d input_plugin number is too much only %d plugins loaded\n", input_number, param->global->incnt);
        return 1;
    }
    OPRINT("input plugin.....: %d: %s\n", input_number, param->global->in[input_number].plugin);
    servers[param->id].write_buf.use_buffering = 1;
    
    /* Initialize Stage 3 optimizations: async I/O and header caching */
    if (init_async_io(&servers[param->id].async_io, MAX_EPOLL_EVENTS) != 0) {
        OPRINT("Failed to initialize async I/O\n");
        return -1;
    }
    OPRINT("www-folder-path......: %s\n", (www_folder == NULL) ? "disabled" : www_folder);
    OPRINT("HTTP TCP port........: %d\n", ntohs(port));
    OPRINT("HTTP Listen Address..: %s\n", hostname);
    OPRINT("username:password....: %s\n", (credentials == NULL) ? "disabled" : credentials);

    param->global->out[id].name = malloc((strlen(OUTPUT_PLUGIN_NAME) + 1) * sizeof(char));
    sprintf(param->global->out[id].name, OUTPUT_PLUGIN_NAME);


    return 0;
}

/******************************************************************************
Description.: this will stop the server thread, client threads
              will not get cleaned properly, because they run detached and
              no pointer is kept. This is not a huge issue, because this
              funtion is intended to clean up the biggest mess on shutdown.
Input Value.: id determines which server instance to send commands to
Return Value: always 0
******************************************************************************/
int output_stop(int id)
{

    DBG("will cancel server thread #%02d\n", id);
    pthread_cancel(servers[id].threadID);
    
    /* Clean up Stage 3 optimizations */
    cleanup_async_io(&servers[id].async_io);
    

    return 0;
}

/******************************************************************************
Description.: This creates and starts the server thread
Input Value.: id determines which server instance to send commands to
Return Value: always 0
******************************************************************************/
int output_run(int id)
{
    DBG("launching server thread #%02d\n", id);

    /* create thread and pass context to thread function */
    pthread_create(&(servers[id].threadID), NULL, server_thread, &(servers[id]));
    pthread_detach(servers[id].threadID);

    return 0;
}

/******************************************************************************
Description.: This is just an example function, to show how the output
              plugin could implement some special command.
              If you want to control some GPIO Pin this is a good place to
              implement it. Dont forget to add command types and a mapping.
Input Value.: cmd is the command type
              id determines which server instance to send commands to
Return Value: 0 indicates success, other values indicate an error
******************************************************************************/
int output_cmd(int plugin, unsigned int control_id, unsigned int group, int value)
{
    DBG("command (%d, value: %d) for group %d triggered for plugin instance #%02d\n", control_id, value, group, plugin);
    return 0;
}

/******************************************************************************
Description.: Worker thread for output_http plugin
Input Value.: arg - pointer to output parameter structure
Return Value: NULL
******************************************************************************/
void *worker_thread(void *arg)
{
    output_parameter *param = (output_parameter *)arg;
    
    fprintf(stderr, "output_http: worker_thread started for input %d\n", input_number);
    
    while(!param->global->stop) {
        /* wait for fresh frames */
        fprintf(stderr, "output_http: worker_thread waiting for fresh frame\n");
        pthread_mutex_lock(&param->global->in[input_number].db);
        pthread_cond_wait(&param->global->in[input_number].db_update, &param->global->in[input_number].db);
        fprintf(stderr, "output_http: worker_thread received db_update signal\n");
        
        /* read buffer */
        int frame_size = param->global->in[input_number].size;
        fprintf(stderr, "output_http: worker_thread got frame_size=%d\n", frame_size);
        
        /* allow others to access the global buffer again */
        pthread_mutex_unlock(&param->global->in[input_number].db);
        
    }
    
    fprintf(stderr, "output_http: worker_thread exiting\n");
    return NULL;
}
