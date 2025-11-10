/*******************************************************************************
#                                                                              #
#      MJPG-streamer allows to stream JPG frames from an input-plugin          #
#      to several output plugins                                               #
#                                                                              #
#      Original code Copyright (C) 2007 Tom Stöveken                          #
#                    2011 Eugene Katsevman                                     #
#                                                                              #
#      Optimizations Copyright (C) 2024 350d                                   #
#                                                                              #
#      Licensed under MIT License - see LICENSE file for details               #
#                                                                              #
*******************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <getopt.h>
#include <pthread.h>
#include <syslog.h>
#include <sys/time.h>

#include "../../mjpg_streamer.h"
#include "../../utils.h"

#include "mjpg-proxy.h"

/* private functions and variables to this plugin */
static pthread_t   worker;
static globals     *pglobal;
static pthread_mutex_t controls_mutex;
static int plugin_number;

void *worker_thread(void *);
void worker_cleanup(void *);

#define INPUT_PLUGIN_NAME "HTTP Input plugin"

struct extractor_state  proxy;

/*** plugin interface functions ***/

/******************************************************************************
Description.: parse input parameters
Input Value.: param contains the command line string and a pointer to globals
Return Value: 0 if everything is ok
******************************************************************************/


int input_init(input_parameter *param, int plugin_no)
{
    int i;

    if(pthread_mutex_init(&controls_mutex, NULL) != 0) {
        IPRINT("could not initialize mutex variable\n");
        exit(EXIT_FAILURE);
    }

    param->argv[0] = INPUT_PLUGIN_NAME;

    /* show all parameters for DBG purposes */
    for(i = 0; i < param->argc; i++) {
        DBG("argv[%d]=%s\n", i, param->argv[i]);
    }
    init_mjpg_proxy( &proxy );

    reset_getopt();
    if (parse_cmd_line(&proxy, param->argc, param->argv))
       return 1;

    pglobal = param->global;

    IPRINT("host.............: %s\n", proxy.hostname);
    IPRINT("port.............: %s\n", proxy.port);

    return 0;
}

/******************************************************************************
Description.: stops the execution of the worker thread
Input Value.: -
Return Value: 0
******************************************************************************/
int input_stop(int id)
{
    DBG("will cancel input thread\n");
    pthread_cancel(worker);
    return 0;
}

/******************************************************************************
Description.: starts the worker thread and allocates memory
Input Value.: -
Return Value: 0
******************************************************************************/
int input_run(int id)
{
    plugin_number = id; // Set plugin number for callback
    
    // Initialize frame metadata
    pglobal->in[id].current_size = 0;
    pglobal->in[id].prev_size = 0;
    pglobal->in[id].frame_sequence = 0;
    
    // OPTIMIZATION: Allocate smaller initial buffer
    // Will be reallocated if needed, but starts smaller
    pglobal->in[id].buf = malloc(128 * 1024); // Reduced from 256KB to 128KB
    if(pglobal->in[id].buf == NULL) {
        fprintf(stderr, "could not allocate memory\n");
        exit(EXIT_FAILURE);
    }

    if(pthread_create(&worker, 0, worker_thread, NULL) != 0) {
        free(pglobal->in[id].buf);
        fprintf(stderr, "could not start worker thread\n");
        exit(EXIT_FAILURE);
    }
    pthread_detach(worker);

    return 0;
}


void on_image_received(char * data, int length){
        /* copy JPG picture to global buffer */
        pthread_mutex_lock(&pglobal->in[plugin_number].db);

        /* Update frame metadata */
        pglobal->in[plugin_number].prev_size = pglobal->in[plugin_number].current_size;
        pglobal->in[plugin_number].current_size = length;
        pglobal->in[plugin_number].size = length;
        pglobal->in[plugin_number].frame_sequence++;
        
        /* Set timestamp */
        struct timeval timestamp;
        gettimeofday(&timestamp, NULL);
        pglobal->in[plugin_number].timestamp = timestamp;
        pglobal->in[plugin_number].frame_timestamp_ms = (timestamp.tv_sec * 1000LL) + (timestamp.tv_usec / 1000);

        simd_memcpy(pglobal->in[plugin_number].buf, data, length);

        /* signal fresh_frame */
        pthread_cond_broadcast(&pglobal->in[plugin_number].db_update);
        pthread_mutex_unlock(&pglobal->in[plugin_number].db);

}

void *worker_thread(void *arg)
{

    /* set cleanup handler to cleanup allocated resources */
    pthread_cleanup_push(worker_cleanup, NULL);

    proxy.on_image_received = on_image_received;
    proxy.should_stop =  & pglobal->stop;
    connect_and_stream(&proxy);

    IPRINT("leaving input thread, calling cleanup function now\n");
    pthread_cleanup_pop(1);

    return NULL;
}

/******************************************************************************
Description.: this functions cleans up allocated resources
Input Value.: arg is unused
Return Value: -
******************************************************************************/
void worker_cleanup(void *arg)
{
    static unsigned char first_run = 1;

    if(!first_run) {
        DBG("already cleaned up resources\n");
        return;
    }

    first_run = 0;
    DBG("cleaning up resources allocated by input thread\n");
    close_mjpg_proxy(&proxy);
    if(pglobal->in[plugin_number].buf != NULL) free(pglobal->in[plugin_number].buf);
}




