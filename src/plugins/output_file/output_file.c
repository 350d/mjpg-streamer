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
#include <limits.h>
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
#include <fcntl.h>
#include <time.h>
#include <syslog.h>
#include <dirent.h>

#include "output_file.h"

#include "../../utils.h"
#include "../../mjpg_streamer.h"

#define OUTPUT_PLUGIN_NAME "FILE output plugin"

static pthread_t worker;
static globals *pglobal;
static int fd, delay, ringbuffer_size = -1, ringbuffer_exceed = 0;
static char *folder = "/tmp";
static char *command = NULL;
static int input_number = 0;
static char *mjpgFileName = NULL;
static char *linkFileName = NULL;
static char *fixedFileName = NULL;

/* Performance optimization: static buffers */
#define MAX_FRAME_SIZE (256*1024)  /* 256KB for typical JPEG frames */
#define BUFFER_SIZE 1024           /* 1KB buffer size */
static unsigned char static_framebuffer[MAX_FRAME_SIZE];
static int use_static_buffers = 1;

/* File I/O buffering for better performance */
typedef struct {
    char buffer[BUFFER_SIZE * 4];  /* 4KB write buffer */
    size_t buffer_pos;
    int fd;
    int use_buffering;
} file_write_buffer;

static file_write_buffer file_buf = {0};

/* File I/O buffering functions */
static void init_file_buffer(file_write_buffer *buf, int fd) {
    buf->buffer_pos = 0;
    buf->fd = fd;
    buf->use_buffering = 1;
}

static int buffered_write(file_write_buffer *buf, const void *data, size_t len) {
    if (!buf->use_buffering) {
        return write(buf->fd, data, len);
    }
    
    const char *src = (const char*)data;
    size_t remaining = len;
    
    while (remaining > 0) {
        size_t space_in_buffer = sizeof(buf->buffer) - buf->buffer_pos;
        size_t to_copy = (remaining < space_in_buffer) ? remaining : space_in_buffer;
        
        simd_memcpy(buf->buffer + buf->buffer_pos, src, to_copy);
        buf->buffer_pos += to_copy;
        src += to_copy;
        remaining -= to_copy;
        
        /* Flush buffer if full */
        if (buf->buffer_pos >= sizeof(buf->buffer)) {
            if (write(buf->fd, buf->buffer, buf->buffer_pos) < 0) return -1;
            buf->buffer_pos = 0;
        }
    }
    
    return len;
}

static int flush_file_buffer(file_write_buffer *buf) {
    if (buf->buffer_pos > 0) {
        int result = write(buf->fd, buf->buffer, buf->buffer_pos);
        buf->buffer_pos = 0;
        return result;
    }
    return 0;
}

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
            " [-f | --folder ]........: folder to save pictures\n" \
            " [-m | --mjpeg ].........: save the frames to an mjpg file \n" \
            " [-l | --link ]..........: link the last picture in ringbuffer as this fixed named file\n" \
            " [-d | --delay ].........: delay after saving pictures in ms\n" \
            " [-i | --input ].........: read frames from the specified input plugin\n" \
            " The following arguments are takes effect only if the current mode is not MJPG\n" \
            " [-s | --size ]..........: size of ring buffer (max number of pictures to hold)\n" \
            " [-e | --exceed ]........: allow ringbuffer to exceed limit by this amount\n" \
            " [-c | --command ].......: execute command after saving picture\n"\
            " [-n | --name ]..........: use fixed filename instead of timestamp-based name\n"\
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

    if (mjpgFileName != NULL) {
        close(fd);
    }

    if(!first_run) {
        DBG("already cleaned up resources\n");
        return;
    }

    first_run = 0;
    OPRINT("cleaning up resources allocated by worker thread\n");

    close(fd);
}

/******************************************************************************
Description.: compares a directory entry with a pattern
Input Value.: directory entry
Return Value: 0 if string do not match, 1 if they match
******************************************************************************/
int check_for_filename(const struct dirent *entry)
{
    int rc;

    int year, month, day, hour, minute, second;
    unsigned long long number;

    /*
     * try to scan the string using scanf
     * I would like to use a define for this format string later...
     */
    rc = sscanf(entry->d_name, "%d_%d_%d_%d_%d_%d_picture_%09llu.jpg", &year, \
                &month, \
                &day, \
                &hour, \
                &minute, \
                &second, \
                &number);

    DBG("%s, rc is %d (%d, %d, %d, %d, %d, %d, %llu)\n", entry->d_name, \
        rc, \
        year, \
        month, \
        day, \
        hour, \
        minute, \
        second, \
        number);

    /* if scanf could find all values, it matches our filenames */
    if(rc != 7) return 0;

    return 1;
}

/******************************************************************************
Description.: delete oldest files, just keep "size" most recent files
              This funtion MAY delete the wrong files if the time is not valid
Input Value.: how many files to keep
Return Value: -
******************************************************************************/
void maintain_ringbuffer(int size)
{
    struct dirent **namelist;
    int n, i;
    char buffer[1<<16];
    struct stat file_stat;
    time_t oldest_time = 0;
    char *oldest_file = NULL;

    /* do nothing if ringbuffer is not set or wrong value is set */
    if(size < 0) return;

    /* get a sorted list of directory items */
    n = scandir(folder, &namelist, check_for_filename, alphasort);
    if(n < 0) {
        perror("scandir");
        return;
    }

    DBG("found %d directory entries\n", n);

    /* If we have more files than the ringbuffer size, delete oldest ones */
    if(n > size) {
        int files_to_delete = n - size;
        
        /* Find and delete the oldest files */
        for(int delete_count = 0; delete_count < files_to_delete; delete_count++) {
            oldest_time = 0;
            oldest_file = NULL;
            int oldest_index = -1;
            
            /* Find the oldest file */
            for(i = 0; i < n; i++) {
                if(namelist[i] == NULL) continue; /* Already processed */
                
                snprintf(buffer, sizeof(buffer), "%s/%s", folder, namelist[i]->d_name);
                if(stat(buffer, &file_stat) == 0) {
                    if(oldest_time == 0 || file_stat.st_mtime < oldest_time) {
                        oldest_time = file_stat.st_mtime;
                        oldest_file = namelist[i]->d_name;
                        oldest_index = i;
                    }
                }
            }
            
            /* Delete the oldest file */
            if(oldest_index >= 0) {
                snprintf(buffer, sizeof(buffer), "%s/%s", folder, namelist[oldest_index]->d_name);
                DBG("delete: %s\n", buffer);
                
                if(unlink(buffer) == -1) {
                    perror("could not delete file");
                }
                
                free(namelist[oldest_index]);
                namelist[oldest_index] = NULL;
            }
        }
    }

    /* free remaining allocated memory */
    for(i = 0; i < n; i++) {
        if(namelist[i] != NULL) {
            DBG("keep: %s\n", namelist[i]->d_name);
            free(namelist[i]);
        }
    }

    /* free the namelist array */
    free(namelist);
}

/******************************************************************************
Description.: this is the main worker thread
              it loops forever, grabs a fresh frame and stores it to file
Input Value.:
Return Value:
******************************************************************************/
void *worker_thread(void *arg)
{
    int ok = 1, frame_size = 0, rc = 0;
    char buffer1[1024] = {0}, buffer2[1024] = {0};
    unsigned long long counter = 0;
    time_t t;
    struct tm *now;
    unsigned char *tmp_framebuffer = NULL;
    
    /* Initialize SIMD capabilities for optimal performance */
    static int simd_initialized = 0;
    if (!simd_initialized) {
        detect_simd_capabilities();
        simd_initialized = 1;
    }

    /* set cleanup handler to cleanup allocated resources */
    pthread_cleanup_push(worker_cleanup, NULL);

    /* Wait for fresh frame using sequence number */
    static unsigned int last_file_sequence = UINT_MAX;
    
    while(ok >= 0 && !pglobal->stop) {
        DBG("waiting for fresh frame\n");

        pthread_mutex_lock(&pglobal->in[input_number].db);
        
        if (!is_new_frame_available(&pglobal->in[input_number], &last_file_sequence)) {
            /* No new frame, wait for signal with timeout */
            struct timespec timeout;
            calculate_wait_timeout(&pglobal->in[input_number], &timeout);
            int ret = pthread_cond_timedwait(&pglobal->in[input_number].db_update, &pglobal->in[input_number].db, &timeout);
            if (ret == ETIMEDOUT) {
                pthread_mutex_unlock(&pglobal->in[input_number].db);
                continue;
            }
            /* Check again after signal */
            if (!is_new_frame_available(&pglobal->in[input_number], &last_file_sequence)) {
                pthread_mutex_unlock(&pglobal->in[input_number].db);
                continue;
            }
        }

        /* read buffer */
        frame_size = pglobal->in[input_number].size;

        /* Use static buffer if available and sufficient, otherwise fallback to dynamic */
        unsigned char *current_frame = NULL;
        if (use_static_buffers && frame_size <= MAX_FRAME_SIZE) {
            /* Use static buffer for better performance */
            current_frame = static_framebuffer;
        } else {
            /* Fallback to dynamic allocation for large frames */
            if(tmp_framebuffer == NULL || frame_size > 1024*1024) { // 1MB limit
                if(tmp_framebuffer != NULL) free(tmp_framebuffer);
                tmp_framebuffer = malloc(frame_size);
                if(tmp_framebuffer == NULL) {
                    pthread_mutex_unlock(&pglobal->in[input_number].db);
                    LOG("not enough memory for frame buffer\n");
                    break;
                }
            }
            current_frame = tmp_framebuffer;
        }
        
        /* copy frame to our local buffer using SIMD optimization */
        simd_memcpy(current_frame, pglobal->in[input_number].buf, frame_size);

        /* allow others to access the global buffer again */
        pthread_mutex_unlock(&pglobal->in[input_number].db);
        

        if (mjpgFileName == NULL) { // single files with ringbuffer mode
            /* prepare filename */
            memset(buffer1, 0, sizeof(buffer1));
            memset(buffer2, 0, sizeof(buffer2));

            if (fixedFileName != NULL) {
                /* use fixed filename */
                snprintf(buffer2, sizeof(buffer2), "%s/%s", folder, fixedFileName);
            } else {
                /* get current time */
                t = time(NULL);
                now = localtime(&t);
                if(now == NULL) {
                    perror("localtime");
                    return NULL;
                }

                /* prepare string, add time and date values */
                if(strftime(buffer1, sizeof(buffer1), "%%s/%Y_%m_%d_%H_%M_%S_picture_%%09llu.jpg", now) == 0) {
                    OPRINT("strftime returned 0\n");
                    return NULL;
                }

                /* finish filename by adding the foldername and a counter value */
                snprintf(buffer2, sizeof(buffer2), buffer1, folder, counter);
                counter++;
            }

            DBG("writing file: %s\n", buffer2);

            /* open file for write */
            if((fd = open(buffer2, O_CREAT | O_RDWR | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)) < 0) {
                OPRINT("could not open the file %s\n", buffer2);
                return NULL;
            }

            /* Initialize file buffer for better I/O performance */
            init_file_buffer(&file_buf, fd);

            /* save picture to file using buffered write */
            if(buffered_write(&file_buf, current_frame, frame_size) < 0) {
                OPRINT("could not write to file %s\n", buffer2);
                perror("write()");
                flush_file_buffer(&file_buf);
                close(fd);
                return NULL;
            }

            /* Flush any remaining data and close file */
            flush_file_buffer(&file_buf);
            close(fd);

            /* link the picture as fixed name file */
            if (linkFileName) {
                snprintf(buffer1, sizeof(buffer1), "%s/%s", folder, linkFileName);
                unlink(buffer1);
                if (link(buffer2, buffer1) != 0) {
                    OPRINT("Failed to create link: %s -> %s\n", buffer2, buffer1);
                }
            }

            /* call the command if user specified one, pass current filename as argument */
            if(command != NULL) {
                memset(buffer1, 0, sizeof(buffer1));

                /* buffer2 still contains the filename, pass it to the command as parameter */
                snprintf(buffer1, sizeof(buffer1), "%s \"%s\"", command, buffer2);
                DBG("calling command %s", buffer1);

                /* in addition provide the filename as environment variable */
                if((rc = setenv("MJPG_FILE", buffer2, 1)) != 0) {
                    LOG("setenv failed (return value %d)\n", rc);
                }

                /* execute the command now */
                if((rc = system(buffer1)) != 0) {
                    LOG("command failed (return value %d)\n", rc);
                }
            }

            /*
             * maintain ringbuffer
             * do not maintain ringbuffer for each picture, this saves resources since
             * each run of the maintainance function involves sorting/malloc/free operations
             * skip ringbuffer maintenance if using fixed filename
             */
            if (fixedFileName == NULL) {
                if(ringbuffer_exceed <= 0) {
                    /* keep ringbuffer excactly at specified siOUTPUT_PLUGIN_NAMEze */
                    maintain_ringbuffer(ringbuffer_size);
                } else if(counter == 1 || counter % (ringbuffer_exceed + 1) == 0) {
                    DBG("counter: %llu, will clean-up now\n", counter);
                    maintain_ringbuffer(ringbuffer_size);
                }
            }
        } else { // recording to MJPG file
            /* Initialize file buffer if not already done */
            if (file_buf.fd != fd) {
                init_file_buffer(&file_buf, fd);
            }
            
            /* save picture to file using buffered write */
            if(buffered_write(&file_buf, current_frame, frame_size) < 0) {
                OPRINT("could not write to file %s\n", buffer2);
                perror("write()");
                flush_file_buffer(&file_buf);
                close(fd);
                return NULL;
            }
        }

        /* if specified, wait now */
        if(delay > 0) {
            usleep(1000 * delay);
        }
    }

    /* cleanup now */
    pthread_cleanup_pop(1);

    return NULL;
}

/*** plugin interface functions ***/
/******************************************************************************
Description.: this function is called first, in order to initialize
              this plugin and pass a parameter string
Input Value.: parameters
Return Value: 0 if everything is OK, non-zero otherwise
******************************************************************************/
int output_init(output_parameter *param, int id)
{
	int i;
    delay = 0;
    pglobal = param->global;
    pglobal->out[id].name = malloc((1+strlen(OUTPUT_PLUGIN_NAME))*sizeof(char));
    sprintf(pglobal->out[id].name, "%s", OUTPUT_PLUGIN_NAME);
    DBG("OUT plugin %d name: %s\n", id, pglobal->out[id].name);

    param->argv[0] = OUTPUT_PLUGIN_NAME;
    

    /* show all parameters for DBG purposes */
    for(i = 0; i < param->argc; i++) {
        DBG("argv[%d]=%s\n", i, param->argv[i]);
    }

    reset_getopt();
    while(1) {
        int option_index = 0, c = 0;
        static struct option long_options[] = {
            {"h", no_argument, 0, 0},
            {"help", no_argument, 0, 0},
            {"f", required_argument, 0, 0},
            {"folder", required_argument, 0, 0},
            {"d", required_argument, 0, 0},
            {"delay", required_argument, 0, 0},
            {"s", required_argument, 0, 0},
            {"size", required_argument, 0, 0},
            {"e", required_argument, 0, 0},
            {"exceed", required_argument, 0, 0},
            {"i", required_argument, 0, 0},
            {"input", required_argument, 0, 0},
            {"m", required_argument, 0, 0},
            {"mjpeg", required_argument, 0, 0},
            {"l", required_argument, 0, 0},
            {"link", required_argument, 0, 0},
            {"c", required_argument, 0, 0},
            {"command", required_argument, 0, 0},
            {"n", required_argument, 0, 0},
            {"name", required_argument, 0, 0},
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

            /* f, folder */
        case 2:
        case 3:
            DBG("case 2,3\n");
            folder = malloc(strlen(optarg) + 1);
            strcpy(folder, optarg);
            if(folder[strlen(folder)-1] == '/')
                folder[strlen(folder)-1] = '\0';
            break;

            /* d, delay */
        case 4:
        case 5:
            DBG("case 4,5\n");
            delay = atoi(optarg);
            break;

            /* s, size */
        case 6:
        case 7:
            DBG("case 6,7\n");
            ringbuffer_size = atoi(optarg);
            break;

            /* e, exceed */
        case 8:
        case 9:
            DBG("case 8,9\n");
            ringbuffer_exceed = atoi(optarg);
            break;
            /* i, input*/
        case 10:
        case 11:
            DBG("case 12,13\n");
            input_number = atoi(optarg);
            break;
            /* m mjpeg */
        case 12:
        case 13:
            DBG("case 12,13\n");
            mjpgFileName = strdup(optarg);
            break;
            /* l link */
        case 14:
        case 15:
            DBG("case 14,15\n");
            linkFileName = strdup(optarg);
            break;
            /* c command */
        case 16:
        case 17:
            DBG("case 16,17\n");
            command = strdup(optarg);
            break;
            /* n name */
        case 18:
        case 19:
            DBG("case 18,19\n");
            fixedFileName = strdup(optarg);
            break;
        }
    }

    if(!(input_number < pglobal->incnt)) {
        OPRINT("ERROR: the %d input_plugin number is too much only %d plugins loaded\n", input_number, param->global->incnt);
        return 1;
    }

    OPRINT("output folder.....: %s\n", folder);
    OPRINT("input plugin.....: %d: %s\n", input_number, pglobal->in[input_number].plugin);
    OPRINT("delay after save..: %d\n", delay);
    if (fixedFileName != NULL) {
        OPRINT("fixed filename....: %s\n", fixedFileName);
    }
    if  (mjpgFileName == NULL) {
        if(ringbuffer_size > 0) {
            OPRINT("ringbuffer size...: %d to %d\n", ringbuffer_size, ringbuffer_size + ringbuffer_exceed);
        } else {
            OPRINT("ringbuffer size...: %s\n", "no ringbuffer");
        }
    } else {
        char *fnBuffer = malloc(strlen(mjpgFileName) + strlen(folder) + 3);
        sprintf(fnBuffer, "%s/%s", folder, mjpgFileName);

        OPRINT("output file.......: %s\n", fnBuffer);
        if((fd = open(fnBuffer, O_CREAT | O_RDWR | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)) < 0) {
            OPRINT("could not open the file %s\n", fnBuffer);
            free(fnBuffer);
            return 1;
        }
        free(fnBuffer);
    }

    param->global->out[id].parametercount = 2;

    param->global->out[id].out_parameters = (control*) calloc(2, sizeof(control));

    control take_ctrl;
	take_ctrl.group = IN_CMD_GENERIC;
	take_ctrl.menuitems = NULL;
	take_ctrl.value = 1;
	take_ctrl.class_id = 0;

	take_ctrl.ctrl.id = OUT_FILE_CMD_TAKE;
	take_ctrl.ctrl.type = V4L2_CTRL_TYPE_BUTTON;
	strcpy((char*) take_ctrl.ctrl.name, "Take snapshot");
	take_ctrl.ctrl.minimum = 0;
	take_ctrl.ctrl.maximum = 1;
	take_ctrl.ctrl.step = 1;
	take_ctrl.ctrl.default_value = 0;

	param->global->out[id].out_parameters[0] = take_ctrl;

    control filename_ctrl;
	filename_ctrl.group = IN_CMD_GENERIC;
	filename_ctrl.menuitems = NULL;
	filename_ctrl.value = 1;
	filename_ctrl.class_id = 0;

	filename_ctrl.ctrl.id = OUT_FILE_CMD_FILENAME;
	filename_ctrl.ctrl.type = V4L2_CTRL_TYPE_STRING;
	strcpy((char*) filename_ctrl.ctrl.name, "Filename");
	filename_ctrl.ctrl.minimum = 0;
	filename_ctrl.ctrl.maximum = 32;
	filename_ctrl.ctrl.step = 1;
	filename_ctrl.ctrl.default_value = 0;

	param->global->out[id].out_parameters[1] = filename_ctrl;


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

int output_cmd(int plugin_id, unsigned int control_id, unsigned int group, int value, char *valueStr)
{
    int i = 0;
    DBG("command (%d, value: %d) for group %d triggered for plugin instance #%02d\n", control_id, value, group, plugin_id);
    switch(group) {
		case IN_CMD_GENERIC:
			for(i = 0; i < pglobal->out[plugin_id].parametercount; i++) {
				if((pglobal->out[plugin_id].out_parameters[i].ctrl.id == control_id) && (pglobal->out[plugin_id].out_parameters[i].group == IN_CMD_GENERIC)) {
					DBG("Generic control found (id: %d): %s\n", control_id, pglobal->out[plugin_id].out_parameters[i].ctrl.name);
					switch(control_id) {
                            case OUT_FILE_CMD_TAKE: {
                                unsigned char *tmp_framebuffer = NULL;
                                if (valueStr != NULL) {
                                    int frame_size = 0;
                                    unsigned char *current_frame = NULL;

                                    if(pthread_mutex_lock(&pglobal->in[input_number].db)) {
                                        DBG("Unable to lock mutex\n");
                                        return -1;
                                    }
                                    /* read buffer */
                                    frame_size = pglobal->in[input_number].size;

                                    /* allocate buffer for frame copy */
                                    if(tmp_framebuffer == NULL || frame_size > 1024*1024) { // 1MB limit
                                        if(tmp_framebuffer != NULL) free(tmp_framebuffer);
                                        tmp_framebuffer = malloc(frame_size);
                                        if(tmp_framebuffer == NULL) {
                                            pthread_mutex_unlock(&pglobal->in[input_number].db);
                                            DBG("not enough memory for frame buffer\n");
                                            return -1;
                                        }
                                    }
                                    
                                    /* copy frame to our local buffer */
                                    simd_memcpy(tmp_framebuffer, pglobal->in[input_number].buf, frame_size);
                                    current_frame = tmp_framebuffer;

                                    /* allow others to access the global buffer again */
                                    pthread_mutex_unlock(&pglobal->in[input_number].db);

                                    DBG("writing file: %s\n", valueStr);

                                    int fd;
                                    /* open file for write */
                                    if((fd = open(valueStr, O_CREAT | O_RDWR | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)) < 0) {
                                        OPRINT("could not open the file %s\n", valueStr);
                                        return -1;
                                    }

                                    /* save picture to file */
                                    if(write(fd, current_frame, frame_size) < 0) {
                                        OPRINT("could not write to file %s\n", valueStr);
                                        perror("write()");
                                        close(fd);
                                        return -1;
                                    }

                                    close(fd);
                                } else {
                                    DBG("No filename specified\n");
                                    return -1;
                                }
                                
                                /* cleanup */
                                if(tmp_framebuffer != NULL) {
                                    free(tmp_framebuffer);
                                }
                            } break;
                            case OUT_FILE_CMD_FILENAME: {
                                DBG("Not yet implemented\n");
                                return -1;
                            } break;
                            default: {
                                DBG("Unknown command\n");
                                return -1;
                            } break;
					}
					DBG("Ctrl %s new value: %d\n", pglobal->out[plugin_id].out_parameters[i].ctrl.name, value);
					return 0;
				}
			}
			DBG("Requested generic control (%d) did not found\n", control_id);
			return -1;
			break;
	}
    return 0;
}
