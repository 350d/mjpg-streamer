/*******************************************************************************
#                                                                              #
#      MJPG-streamer allows to stream JPG frames from an input-plugin          #
#      to several output plugins                                               #
#                                                                              #
#      Copyright (C) 2008 Tom Stöveken                                         #
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
#include <getopt.h>
#include <pthread.h>
#include <syslog.h>

#include <SDL/SDL.h>

#include "../../utils.h"
#include "../../jpeg_utils.h"
#include "../../mjpg_streamer.h"

#define OUTPUT_PLUGIN_NAME "VIEWER output plugin"

static pthread_t worker;
static globals *pglobal;
static unsigned char *frame = NULL;
static int input_number = 0;

/******************************************************************************
Description.: print a help message
Input Value.: -
Return Value: -
******************************************************************************/
void help(void)
{
    fprintf(stderr, " ---------------------------------------------------------------\n" \
            " Help for output plugin..: "OUTPUT_PLUGIN_NAME"\n" \
            " ---------------------------------------------------------------\n");
}

/******************************************************************************
Description.: clean up allocated resources
Input Value.: unused argument
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
    OPRINT("cleaning up resources allocated by worker thread\n");

    free(frame);
    SDL_Quit();
}

/* Use jpeg_utils for JPEG decompression */

/* Use jpeg_rgb_image from jpeg_utils.h */

/* Use jpeg_decompress_to_rgb from jpeg_utils */

/******************************************************************************
Description.: this is the main worker thread
              it loops forever, grabs a fresh frame, decompressed the JPEG
              and displays the decoded data using SDL
Input Value.:
Return Value:
******************************************************************************/
void *worker_thread(void *arg)
{
    int frame_size = 0, firstrun = 1;

    SDL_Surface *screen = NULL, *image = NULL;
    jpeg_rgb_image rgbimage;

    /* initialize the buffer for the decompressed image */
    rgbimage.buffersize = 0;
    rgbimage.buffer = NULL;

    /* initialze the SDL video subsystem */
    if(SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "Couldn't initialize SDL: %s\n", SDL_GetError());
        exit(EXIT_FAILURE);
    }

    /* just allocate a large buffer for the JPEGs */
    if((frame = malloc(4096 * 1024)) == NULL) {
        OPRINT("not enough memory for worker thread\n");
        exit(EXIT_FAILURE);
    }

    /* set cleanup handler to cleanup allocated resources */
    pthread_cleanup_push(worker_cleanup, NULL);

    /* Wait for fresh frame using sequence number */
    static unsigned int last_viewer_sequence = UINT_MAX;
    
    while(!pglobal->stop) {
        DBG("waiting for fresh frame\n");
        pthread_mutex_lock(&pglobal->in[input_number].db);
        
        if (!is_new_frame_available(&pglobal->in[input_number], &last_viewer_sequence)) {
            /* No new frame, wait for signal with timeout */
            struct timespec timeout;
            calculate_wait_timeout(&pglobal->in[input_number], &timeout);
            int ret = pthread_cond_timedwait(&pglobal->in[input_number].db_update, &pglobal->in[input_number].db, &timeout);
            if (ret == ETIMEDOUT) {
                pthread_mutex_unlock(&pglobal->in[input_number].db);
                continue;
            }
            /* Check again after signal */
            if (!is_new_frame_available(&pglobal->in[input_number], &last_viewer_sequence)) {
                pthread_mutex_unlock(&pglobal->in[input_number].db);
                continue;
            }
        }

        /* read buffer */
        frame_size = pglobal->in[input_number].size;
        memcpy(frame, pglobal->in[input_number].buf, frame_size);

        pthread_mutex_unlock(&pglobal->in[input_number].db);
        

        /* Use global metadata for dimensions */
        rgbimage.width = pglobal->in[input_number].width;
        rgbimage.height = pglobal->in[input_number].height;
        rgbimage.buffersize = rgbimage.width * rgbimage.height * 3;
        
        /* decompress the JPEG and store results in memory */
        if(jpeg_decompress_to_rgb(frame, frame_size, &rgbimage.buffer, &rgbimage.width, &rgbimage.height, pglobal->in[input_number].width, pglobal->in[input_number].height)) {
            DBG("could not properly decompress JPEG data\n");
            continue;
        }

        if(firstrun) {
            /* create the primary surface (the visible window) */
            screen = SDL_SetVideoMode(rgbimage.width, rgbimage.height, 0, SDL_ANYFORMAT | SDL_HWSURFACE);
            SDL_WM_SetCaption("MJPG-Streamer Viewer", NULL);

            /* create a SDL surface to display the data */
            image = SDL_AllocSurface(SDL_SWSURFACE, rgbimage.width, rgbimage.height, 24,
#if SDL_BYTEORDER == SDL_LIL_ENDIAN
                                     0x0000FF, 0x00FF00, 0xFF0000,
#else
                                     0xFF0000, 0x00FF00, 0x0000FF,
#endif
                                     0);

            firstrun = 0;
        }

        /* copy the decoded data to the SDL surface */
        memcpy(image->pixels, rgbimage.buffer, rgbimage.width * rgbimage.height * 3);
        free(rgbimage.buffer);

        /* copy the image to the primary surface */
        SDL_BlitSurface(image, NULL, screen, NULL);

        /* redraw the whole surface */
        SDL_Flip(screen);
    }

    pthread_cleanup_pop(1);

    /* get rid of the image */
    SDL_FreeSurface(image);

    return NULL;
}

/*** plugin interface functions ***/
/******************************************************************************
Description.: this function is called first, in order to initialise
              this plugin and pass a parameter string
Input Value.: parameters
Return Value: 0 if everything is ok, non-zero otherwise
******************************************************************************/
int output_init(output_parameter *param)
{
    int i;

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
            /* i, input */
        case 2:
        case 3:
            DBG("case 2,3\n");
            input_number = atoi(optarg);
            break;
        }
    }

    pglobal = param->global;
    if(!(input_number < pglobal->incnt)) {
        OPRINT("ERROR: the %d input_plugin number is too much only %d plugins loaded\n", input_number, pglobal->incnt);
        return 1;
    }
    OPRINT("input plugin.....: %d: %s\n", input_number, pglobal->in[input_number].plugin);

    return 0;
}

/******************************************************************************
Description.: calling this function stops the worker thread
Input Value.: -
Return Value: always 0
******************************************************************************/
int output_stop(int id)
{
    DBG("will cancel worker thread\n");
    pthread_cancel(worker);
    
    
    return 0;
}

/******************************************************************************
Description.: calling this function creates and starts the worker thread
Input Value.: -
Return Value: always 0
******************************************************************************/
int output_run(int id)
{
    DBG("launching worker thread\n");
    pthread_create(&worker, 0, worker_thread, NULL);
    pthread_detach(worker);
    return 0;
}

int output_cmd()
{


}

