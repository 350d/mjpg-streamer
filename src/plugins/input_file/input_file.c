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
#include <getopt.h>
#include <pthread.h>
#include <syslog.h>
#include <sys/types.h>
#ifdef __linux__
#include <sys/inotify.h>
#endif
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <strings.h>  /* for strcasecmp */
#include <sys/select.h>  /* for select */

#include "../../mjpg_streamer.h"
#include "../../utils.h"

#define INPUT_PLUGIN_NAME "FILE input plugin"
#define MAX_FILE_SIZE (10*1024*1024)  /* 10MB max file size */

typedef enum _read_mode {
    NewFilesOnly,
    ExistingFiles
} read_mode;

/* Buffered I/O structure */
typedef struct {
    char buffer[8192];  /* 8KB read buffer */
    size_t buffer_pos;
    size_t buffer_size;
    int fd;
    int eof;
} file_read_buffer;

/* private functions and variables to this plugin */
static pthread_t   worker;
static globals     *pglobal;

void *worker_thread(void *);
void worker_cleanup(void *);
void help(void);

/* Buffered I/O functions */
static void init_file_read_buffer(file_read_buffer *buf, int fd);
static ssize_t buffered_read(file_read_buffer *buf, void *data, size_t size);
static void close_file_read_buffer(file_read_buffer *buf);

static double delay = 1.0;
static char *folder = NULL;
static char *filename = NULL;
static int rm = 0;
static int plugin_number;
static read_mode mode = NewFilesOnly;

/* global variables for this plugin */
static int fd, rc, wd, size;
static struct inotify_event *ev;

/* Static buffer for file data to avoid malloc/free in hot path */
static unsigned char static_file_buffer[MAX_FILE_SIZE];
static int use_static_buffers = 1;

/*** plugin interface functions ***/
int input_init(input_parameter *param, int id)
{
    int i;
    plugin_number = id;

    param->argv[0] = INPUT_PLUGIN_NAME;

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
            {"d", required_argument, 0, 0},
            {"delay", required_argument, 0, 0},
            {"f", required_argument, 0, 0},
            {"folder", required_argument, 0, 0},
            {"r", no_argument, 0, 0},
            {"remove", no_argument, 0, 0},
            {"n", required_argument, 0, 0},
            {"name", required_argument, 0, 0},
            {"e", no_argument, 0, 0},
            {"existing", no_argument, 0, 0},
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

            /* d, delay */
        case 2:
        case 3:
            DBG("case 2,3\n");
            delay = atof(optarg);
            break;

            /* f, folder */
        case 4:
        case 5:
            DBG("case 4,5\n");
            folder = malloc(strlen(optarg) + 2);
            strcpy(folder, optarg);
            if(optarg[strlen(optarg)-1] != '/')
                strcat(folder, "/");
            break;

            /* r, remove */
        case 6:
        case 7:
            DBG("case 6,7\n");
            rm = 1;
            break;

            /* n, name */
        case 8:
        case 9:
            DBG("case 8,9\n");
            filename = malloc(strlen(optarg) + 1);
            strcpy(filename, optarg);
            break;
            /* e, existing */
        case 10:
        case 11:
            DBG("case 10,11\n");
            mode = ExistingFiles;
            break;
        default:
            DBG("default case\n");
            help();
            return 1;
        }
    }

    pglobal = param->global;

    /* check for required parameters */
    if(folder == NULL) {
        IPRINT("ERROR: no folder specified\n");
        return 1;
    }

    IPRINT("folder to watch...: %s\n", folder);
    IPRINT("forced delay......: %.4f\n", delay);
    IPRINT("delete file.......: %s\n", (rm) ? "yes, delete" : "no, do not delete");
    IPRINT("filename must be..: %s\n", (filename == NULL) ? "-no filter for certain filename set-" : filename);

    param->global->in[id].name = malloc((strlen(INPUT_PLUGIN_NAME) + 1) * sizeof(char));
    sprintf(param->global->in[id].name, INPUT_PLUGIN_NAME);

    return 0;
}

int input_stop(int id)
{
    DBG("will cancel input thread\n");
    
    /* Set stop flag first */
    if(pglobal) {
        pglobal->stop = 1;
    }
    
    /* Give thread multiple chances to see the stop flag */
    for(int i = 0; i < 10; i++) {
        usleep(1000); /* 1ms */
        if(pglobal && pglobal->stop) {
            DBG("stop flag set, thread should exit\n");
            break;
        }
    }
    
    /* Force cancel if still running */
    pthread_cancel(worker);
    
    /* Note: pthread_join removed to avoid blocking on unresponsive threads */
    
    return 0;
}

int input_run(int id)
{
    pglobal->in[id].buf = NULL;

    if (mode == NewFilesOnly) {
#ifdef __linux__
        rc = fd = inotify_init();
        if(rc == -1) {
            perror("could not initilialize inotify");
            return 1;
        }

        rc = wd = inotify_add_watch(fd, folder, IN_CLOSE_WRITE | IN_MOVED_TO | IN_ONLYDIR);
        if(rc == -1) {
            perror("could not add watch");
            return 1;
        }

        size = sizeof(struct inotify_event) + (1 << 16);
        ev = malloc(size);
        if(ev == NULL) {
            perror("not enough memory");
            return 1;
        }
#else
        IPRINT("inotify not available on this platform, using ExistingFiles mode\n");
        mode = ExistingFiles;
#endif
    }

    if(pthread_create(&worker, 0, worker_thread, NULL) != 0) {
        free(pglobal->in[id].buf);
        fprintf(stderr, "could not start worker thread\n");
        exit(EXIT_FAILURE);
    }

    /* Keep thread joinable for proper cleanup */

    return 0;
}

/*** private functions for this plugin below ***/
void help(void)
{
    fprintf(stderr, " ---------------------------------------------------------------\n" \
    " Help for input plugin..: "INPUT_PLUGIN_NAME"\n" \
    " ---------------------------------------------------------------\n" \
    " The following parameters can be passed to this plugin:\n\n" \
    " [-d | --delay ]........: delay (in seconds) to pause between frames\n" \
    " [-f | --folder ].......: folder to watch for new JPEG files\n" \
    " [-r | --remove ].......: remove/delete JPEG file after reading\n" \
    " [-n | --name ].........: ignore changes unless filename matches\n" \
    " [-e | --existing ].....: serve the existing *.jpg files from the specified directory\n" \
    " ---------------------------------------------------------------\n");
}

/* the single writer thread */
void *worker_thread(void *arg)
{
    char buffer[1<<16];
    int file;
    size_t filesize = 0;
    struct stat stats;
    struct dirent **fileList;
    int fileCount = 0;
    int currentFileNumber = 0;
    char hasJpgFile = 0;
    struct timeval timestamp;
    
    /* Initialize SIMD capabilities */
    static int simd_initialized = 0;
    if (!simd_initialized) {
        detect_simd_capabilities();
        simd_initialized = 1;
    }

    if (mode == ExistingFiles) {
        fileCount = scandir(folder, &fileList, 0, alphasort);
        if (fileCount < 0) {
           perror("error during scandir\n");
           return NULL;
        }
    }

    /* set cleanup handler to cleanup allocated resources */
    pthread_cleanup_push(worker_cleanup, NULL);

    while(!pglobal->stop) {
        /* Check stop condition at the beginning of each iteration */
        if(pglobal->stop) {
            DBG("stop condition detected in main loop\n");
            break;
        }
        
        if (mode == NewFilesOnly) {
#ifdef __linux__
            /* Check if we should stop before blocking read */
            if(pglobal->stop) break;
            
            /* Use select to avoid blocking indefinitely */
            fd_set readfds;
            struct timeval timeout;
            FD_ZERO(&readfds);
            FD_SET(fd, &readfds);
            timeout.tv_sec = 1;  /* 1 second timeout */
            timeout.tv_usec = 0;
            
            int select_result = select(fd + 1, &readfds, NULL, NULL, &timeout);
            if(select_result == -1) {
                perror("select failed");
                break;
            } else if(select_result == 0) {
                /* Timeout - check stop condition and continue */
                if(pglobal->stop) break;
                continue;
            }
            
            /* Data available, read inotify event */
            rc = read(fd, ev, size);
            if(rc == -1) {
                perror("reading inotify events failed\n");
                break;
            }

            /* sanity check */
            if(wd != ev->wd) {
                fprintf(stderr, "This event is not for the watched directory (%d != %d)\n", wd, ev->wd);
                continue;
            }

            if(ev->mask & (IN_IGNORED | IN_Q_OVERFLOW | IN_UNMOUNT)) {
                fprintf(stderr, "event mask suggests to stop\n");
                break;
            }

            /* prepare filename */
            snprintf(buffer, sizeof(buffer), "%s%s", folder, ev->name);

            /* check if the filename matches specified parameter (if given) */
            if((filename != NULL) && (strcmp(filename, ev->name) != 0)) {
                DBG("ignoring this change (specified filename does not match)\n");
                continue;
            }
            DBG("new file detected: %s\n", buffer);
#else
            /* Fallback for non-Linux systems */
            usleep(100000); /* 100ms delay */
            continue;
#endif
        } else {
            /* Check stop condition in ExistingFiles mode */
            if(pglobal->stop) break;
            
            /* Optimized file extension check */
            const char *filename = fileList[currentFileNumber]->d_name;
            const char *ext = strrchr(filename, '.');
            int is_jpg = 0;
            
            if (ext != NULL) {
                /* Check for .jpg or .JPG extension (case insensitive) */
                if ((strcasecmp(ext, ".jpg") == 0) || (strcasecmp(ext, ".jpeg") == 0)) {
                    is_jpg = 1;
                }
            }
            
            if (is_jpg) {
                hasJpgFile = 1;
                DBG("serving file: %s\n", fileList[currentFileNumber]->d_name);
                snprintf(buffer, sizeof(buffer), "%s%s", folder, fileList[currentFileNumber]->d_name);
                currentFileNumber++;
                if (currentFileNumber == fileCount)
                    currentFileNumber = 0;
            } else {
                currentFileNumber++;
                if (currentFileNumber == fileCount) {
                    if(hasJpgFile == 0) {
                        perror("No files with jpg/JPG extension in the folder\n");
                        goto thread_quit;
                    } else {
                        // There are some jpeg files, the last one just happens not to be one
                        currentFileNumber = 0;
                    }
                }
                /* Check stop condition during file iteration */
                if(pglobal->stop) break;
                continue;
            }
        }

        /* Check stop condition before file operations */
        if(pglobal->stop) break;

        /* open file for reading */
        rc = file = open(buffer, O_RDONLY);
        if(rc == -1) {
            perror("could not open file for reading");
            break;
        }

        /* approximate size of file */
        rc = fstat(file, &stats);
        if(rc == -1) {
            perror("could not read statistics of file");
            close(file);
            break;
        }

        filesize = stats.st_size;

        /* Check stop condition after file operations */
        if(pglobal->stop) {
            close(file);
            break;
        }

        /* copy frame from file to global buffer */
        /* Check stop condition before blocking mutex lock */
        if(pglobal->stop) break;
        
        /* Try to lock mutex without blocking */
        int lock_result = pthread_mutex_trylock(&pglobal->in[plugin_number].db);
        if(lock_result != 0) {
            close(file);
            DBG("mutex lock failed, checking stop condition\n");
            if(pglobal->stop) break;
            usleep(10000); /* 10ms delay before retry */
            continue;
        }

        /* allocate memory for frame - use static buffer if possible */
        if(pglobal->in[plugin_number].buf != NULL && pglobal->in[plugin_number].buf != static_file_buffer)
            free(pglobal->in[plugin_number].buf);

        if (use_static_buffers && filesize <= MAX_FILE_SIZE) {
            /* Use static buffer for files up to MAX_FILE_SIZE */
            pglobal->in[plugin_number].buf = static_file_buffer;
        } else {
            /* Fallback to dynamic allocation for larger files */
            pglobal->in[plugin_number].buf = malloc(filesize + (1 << 16));
            if(pglobal->in[plugin_number].buf == NULL) {
                fprintf(stderr, "could not allocate memory\n");
                pthread_mutex_unlock(&pglobal->in[plugin_number].db);
                close(file);
                break;
            }
        }

        /* Check stop condition before read operation */
        if(pglobal->stop) {
            close(file);
            pthread_mutex_unlock(&pglobal->in[plugin_number].db);
            break;
        }
        
        /* Use direct read to avoid blocking in buffered_read */
        ssize_t bytes_read = read(file, pglobal->in[plugin_number].buf, filesize);
        if(bytes_read == -1) {
            perror("could not read from file");
            if (pglobal->in[plugin_number].buf != static_file_buffer) {
                free(pglobal->in[plugin_number].buf);
            }
            pglobal->in[plugin_number].buf = NULL; 
            pglobal->in[plugin_number].size = 0;
            pthread_mutex_unlock(&pglobal->in[plugin_number].db);
            close(file);
            break;
        }
        
        pglobal->in[plugin_number].size = bytes_read;

        gettimeofday(&timestamp, NULL);
        pglobal->in[plugin_number].timestamp = timestamp;
        DBG("new frame copied (size: %d)\n", pglobal->in[plugin_number].size);
        /* signal fresh_frame */
        pthread_cond_broadcast(&pglobal->in[plugin_number].db_update);
        pthread_mutex_unlock(&pglobal->in[plugin_number].db);

        close(file);

        /* delete file if necessary */
        if(rm) {
            rc = unlink(buffer);
            if(rc == -1) {
                perror("could not remove/delete file");
            }
        }

        if(delay != 0)
            usleep(1000 * 1000 * delay);
        
        /* Add small delay in ExistingFiles mode to allow stop signal processing */
        if (mode == ExistingFiles) {
            usleep(5000); /* 5ms delay - even faster response to stop signal */
        }
    }

thread_quit:
    while (fileCount--) {
       free(fileList[fileCount]);
    }
    free(fileList);

    DBG("leaving input thread, calling cleanup function now\n");
    /* call cleanup handler, signal with the parameter */
    pthread_cleanup_pop(1);

    return NULL;
}

void worker_cleanup(void *arg)
{
    static unsigned char first_run = 1;

    if(!first_run) {
        DBG("already cleaned up resources\n");
        return;
    }

    first_run = 0;
    DBG("cleaning up resources allocated by input thread\n");

    if(pglobal->in[plugin_number].buf != NULL && pglobal->in[plugin_number].buf != static_file_buffer) {
        free(pglobal->in[plugin_number].buf);
    }

    free(ev);

    if (mode == NewFilesOnly) {
#ifdef __linux__
        rc = inotify_rm_watch(fd, wd);
        if(rc == -1) {
            perror("could not close watch descriptor");
        }

        rc = close(fd);
        if(rc == -1) {
            perror("could not close filedescriptor");
        }
#endif
    }
}

/* Buffered I/O implementation */
static void init_file_read_buffer(file_read_buffer *buf, int fd)
{
    buf->fd = fd;
    buf->buffer_pos = 0;
    buf->buffer_size = 0;
    buf->eof = 0;
}

static ssize_t buffered_read(file_read_buffer *buf, void *data, size_t size)
{
    size_t total_read = 0;
    size_t remaining = size;
    char *dest = (char *)data;
    
    while (remaining > 0 && !buf->eof) {
        /* Refill buffer if needed */
        if (buf->buffer_pos >= buf->buffer_size) {
            ssize_t bytes_read = read(buf->fd, buf->buffer, sizeof(buf->buffer));
            if (bytes_read <= 0) {
                buf->eof = 1;
                break;
            }
            buf->buffer_size = bytes_read;
            buf->buffer_pos = 0;
        }
        
        /* Copy from buffer to destination */
        size_t available = buf->buffer_size - buf->buffer_pos;
        size_t to_copy = (remaining < available) ? remaining : available;
        
        simd_memcpy(dest + total_read, buf->buffer + buf->buffer_pos, to_copy);
        
        total_read += to_copy;
        remaining -= to_copy;
        buf->buffer_pos += to_copy;
    }
    
    return total_read;
}

static void close_file_read_buffer(file_read_buffer *buf)
{
    /* Nothing to do for buffered read */
    buf->fd = -1;
    buf->eof = 1;
}



