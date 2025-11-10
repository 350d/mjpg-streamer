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
#include <limits.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <spawn.h>
#include <signal.h>
#include <sys/wait.h>
#include <libgen.h>
#include <dlfcn.h>

#include "../../utils.h"
#include "../../mjpg_streamer.h"

#define OUTPUT_PLUGIN_NAME "OSX VIEWER output plugin"
#define FRAME_FILENAME "output_viewer_osx_frame.jpg"
#define FRAME_TMP_FILENAME "output_viewer_osx_frame.tmp"
#define DISABLE_FLAG_FILENAME "output_viewer_osx_disabled.flag"

static pthread_t worker;
static globals *pglobal;
static unsigned char *frame = NULL;
static size_t frame_capacity = 0;
static int input_number = 0;

static pid_t viewer_pid = -1;
static int helper_available = 0;
static char helper_path[PATH_MAX];
static char frame_path[PATH_MAX];
static char frame_tmp_path[PATH_MAX];
static char tmp_dir_path[PATH_MAX];
static char disable_flag_path[PATH_MAX];
static int viewer_disabled = 0;
static int viewer_disabled_warned = 0;

extern char **environ;

static int parent_directory(char *path)
{
    size_t len;
    char *slash;

    if (path == NULL) {
        return -1;
    }

    len = strlen(path);
    while (len > 1 && path[len - 1] == '/') {
        path[len - 1] = '\0';
        len--;
    }

    slash = strrchr(path, '/');
    if (!slash) {
        return -1;
    }

    if (slash == path) {
        slash[1] = '\0';
    } else {
        *slash = '\0';
    }

    return 0;
}

static int ensure_directory(const char *path)
{
    struct stat st;

    if (!path) {
        return -1;
    }

    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            return 0;
        }
        OPRINT("path exists but is not a directory: %s\n", path);
        return -1;
    }

    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        OPRINT("failed to create directory %s: %s\n", path, strerror(errno));
        return -1;
    }

    return 0;
}

static void refresh_viewer_disabled_state(void)
{
    int exists = (access(disable_flag_path, F_OK) == 0);
    if (exists && !viewer_disabled) {
        viewer_disabled = 1;
        viewer_disabled_warned = 0;
        OPRINT("macOS viewer disabled by user (flag detected at %s)\n", disable_flag_path);
    } else if (!exists && viewer_disabled) {
        viewer_disabled = 0;
        viewer_disabled_warned = 0;
        OPRINT("macOS viewer re-enabled (flag removed)\n");
    }
}

static int resolve_paths(void)
{
    Dl_info info;
    char plugin_real[PATH_MAX];
    char plugins_dir[PATH_MAX];
    char build_dir[PATH_MAX];
    char project_root[PATH_MAX];
    char candidate[PATH_MAX];
    const char *env_helper;

    if (dladdr((void *)resolve_paths, &info) == 0 || !info.dli_fname) {
        OPRINT("failed to resolve plugin path using dladdr\n");
        return -1;
    }

    if (realpath(info.dli_fname, plugin_real) == NULL) {
        OPRINT("realpath failed for %s: %s\n", info.dli_fname, strerror(errno));
        return -1;
    }

    strncpy(plugins_dir, plugin_real, sizeof(plugins_dir));
    plugins_dir[sizeof(plugins_dir) - 1] = '\0';
    if (parent_directory(plugins_dir) != 0) {
        OPRINT("failed to resolve plugins directory from %s\n", plugin_real);
        return -1;
    }

    strncpy(build_dir, plugins_dir, sizeof(build_dir));
    build_dir[sizeof(build_dir) - 1] = '\0';
    if (parent_directory(build_dir) != 0) {
        OPRINT("failed to resolve build directory from %s\n", plugins_dir);
        return -1;
    }

    strncpy(project_root, build_dir, sizeof(project_root));
    project_root[sizeof(project_root) - 1] = '\0';
    if (parent_directory(project_root) != 0) {
        OPRINT("failed to resolve project root from %s\n", build_dir);
        return -1;
    }

    if (snprintf(tmp_dir_path, sizeof(tmp_dir_path), "%s/tmp", project_root) >= (int)sizeof(tmp_dir_path)) {
        OPRINT("tmp directory path is too long\n");
        return -1;
    }

    if (ensure_directory(tmp_dir_path) != 0) {
        return -1;
    }

    if (snprintf(frame_path, sizeof(frame_path), "%s/%s", tmp_dir_path, FRAME_FILENAME) >= (int)sizeof(frame_path)) {
        OPRINT("frame path is too long\n");
        return -1;
    }

    if (snprintf(frame_tmp_path, sizeof(frame_tmp_path), "%s/%s", tmp_dir_path, FRAME_TMP_FILENAME) >= (int)sizeof(frame_tmp_path)) {
        OPRINT("temporary frame path is too long\n");
        return -1;
    }

    if (snprintf(disable_flag_path, sizeof(disable_flag_path), "%s/%s", tmp_dir_path, DISABLE_FLAG_FILENAME) >= (int)sizeof(disable_flag_path)) {
        OPRINT("disable flag path is too long\n");
        return -1;
    }

    helper_available = 0;

    env_helper = getenv("MJPG_STREAMER_OSX_VIEWER");
    if (env_helper && access(env_helper, X_OK) == 0) {
        strncpy(helper_path, env_helper, sizeof(helper_path));
        helper_path[sizeof(helper_path) - 1] = '\0';
        helper_available = 1;
        return 0;
    }

    if (snprintf(candidate, sizeof(candidate), "%s/src/plugins/output_viewer_osx_app/OSXViewer", build_dir) < (int)sizeof(candidate)
        && access(candidate, X_OK) == 0) {
        strncpy(helper_path, candidate, sizeof(helper_path));
        helper_path[sizeof(helper_path) - 1] = '\0';
        helper_available = 1;
        return 0;
    }

    if (snprintf(candidate, sizeof(candidate), "%s/src/plugins/output_viewer_osx_app/OSXViewer.app/Contents/MacOS/OSXViewer", build_dir) < (int)sizeof(candidate)
        && access(candidate, X_OK) == 0) {
        strncpy(helper_path, candidate, sizeof(helper_path));
        helper_path[sizeof(helper_path) - 1] = '\0';
        helper_available = 1;
        return 0;
    }

    if (snprintf(candidate, sizeof(candidate), "%s/src/plugins/output_viewer_osx_app/OSXViewer", project_root) < (int)sizeof(candidate)
        && access(candidate, X_OK) == 0) {
        strncpy(helper_path, candidate, sizeof(helper_path));
        helper_path[sizeof(helper_path) - 1] = '\0';
        helper_available = 1;
        return 0;
    }

    if (snprintf(candidate, sizeof(candidate), "%s/src/plugins/output_viewer_osx_app/OSXViewer.app/Contents/MacOS/OSXViewer", project_root) < (int)sizeof(candidate)
        && access(candidate, X_OK) == 0) {
        strncpy(helper_path, candidate, sizeof(helper_path));
        helper_path[sizeof(helper_path) - 1] = '\0';
        helper_available = 1;
        return 0;
    }

    OPRINT("viewer helper binary not found. Set MJPG_STREAMER_OSX_VIEWER to the helper path or build OSXViewer.\n");
    return 0;
}

static void stop_helper(void)
{
    int status = 0;
    int attempts = 0;

    if (viewer_pid <= 0) {
        return;
    }

    OPRINT("stopping macOS viewer helper (pid %d)\n", viewer_pid);
    kill(viewer_pid, SIGTERM);

    while (attempts < 50) {
        pid_t ret = waitpid(viewer_pid, &status, WNOHANG);
        if (ret == viewer_pid || ret == -1) {
            break;
        }
        usleep(100000);
        attempts++;
    }

    if (attempts == 50) {
        OPRINT("helper did not exit, sending SIGKILL\n");
        kill(viewer_pid, SIGKILL);
        (void)waitpid(viewer_pid, &status, 0);
    }

    viewer_pid = -1;
}

static int start_helper_if_needed(void)
{
    int spawn_result;
    char *argv_local[3];

    refresh_viewer_disabled_state();

    if (viewer_disabled) {
        if (!viewer_disabled_warned) {
            OPRINT("macOS viewer helper launch suppressed; remove %s to reopen window\n", disable_flag_path);
            viewer_disabled_warned = 1;
        }
        return -1;
    }

    if (!helper_available) {
        return -1;
    }

    if (viewer_pid > 0) {
        if (kill(viewer_pid, 0) == 0) {
            return 0;
        }
        stop_helper();
    }

    argv_local[0] = helper_path;
    argv_local[1] = frame_path;
    argv_local[2] = NULL;

    spawn_result = posix_spawn(&viewer_pid, helper_path, NULL, NULL, argv_local, environ);
    if (spawn_result != 0) {
        viewer_pid = -1;
        OPRINT("failed to launch viewer helper %s: %s\n", helper_path, strerror(spawn_result));
        return -1;
    }

    OPRINT("launched macOS viewer helper (pid %d)\n", viewer_pid);
    viewer_disabled_warned = 0;
    return 0;
}

static int ensure_frame_capacity(size_t size)
{
    unsigned char *new_buf;

    if (frame_capacity >= size) {
        return 0;
    }

    new_buf = realloc(frame, size);
    if (!new_buf) {
        OPRINT("not enough memory to allocate %zu bytes for frame buffer\n", size);
        return -1;
    }

    frame = new_buf;
    frame_capacity = size;
    return 0;
}

static int write_jpeg_frame(const unsigned char *data, size_t size)
{
    int fd;
    size_t written = 0;
    static int warned = 0;

    if (!data || size == 0) {
        return 0;
    }

    fd = open(frame_tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        if (!warned) {
            OPRINT("failed to open temporary frame file %s: %s\n", frame_tmp_path, strerror(errno));
            warned = 1;
        }
        return -1;
    }
    warned = 0;

    while (written < size) {
        ssize_t chunk = write(fd, data + written, size - written);
        if (chunk < 0) {
            if (errno == EINTR) {
                continue;
            }
            OPRINT("failed to write frame data: %s\n", strerror(errno));
            close(fd);
            unlink(frame_tmp_path);
            return -1;
        }
        written += (size_t)chunk;
    }

    if (fsync(fd) != 0) {
        OPRINT("fsync failed for %s: %s\n", frame_tmp_path, strerror(errno));
        close(fd);
        unlink(frame_tmp_path);
        return -1;
    }

    if (close(fd) != 0) {
        OPRINT("close failed for %s: %s\n", frame_tmp_path, strerror(errno));
        unlink(frame_tmp_path);
        return -1;
    }

    if (rename(frame_tmp_path, frame_path) != 0) {
        OPRINT("failed to replace frame file %s: %s\n", frame_path, strerror(errno));
        unlink(frame_tmp_path);
        return -1;
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

    stop_helper();

    if (frame) {
        free(frame);
        frame = NULL;
        frame_capacity = 0;
    }
}

/******************************************************************************
Description.: this is the main worker thread
              it loops forever, grabs a fresh frame and stores it for the helper
Input Value.:
Return Value:
******************************************************************************/
void *worker_thread(void *arg)
{
    static unsigned int last_viewer_sequence = UINT_MAX;

    pthread_cleanup_push(worker_cleanup, NULL);

    while(!pglobal->stop) {
        int frame_size = 0;
        int helper_status;

        DBG("waiting for fresh frame\n");

        if (!wait_for_fresh_frame(&pglobal->in[input_number], &last_viewer_sequence)) {
            usleep(10000);
            continue;
        }

        frame_size = pglobal->in[input_number].size;
        if (frame_size <= 0) {
            pthread_mutex_unlock(&pglobal->in[input_number].db);
            continue;
        }

        if (ensure_frame_capacity((size_t)frame_size) != 0) {
            pthread_mutex_unlock(&pglobal->in[input_number].db);
            break;
        }

        simd_memcpy(frame, pglobal->in[input_number].buf, (size_t)frame_size);

        pthread_mutex_unlock(&pglobal->in[input_number].db);

        helper_status = start_helper_if_needed();

        if (write_jpeg_frame(frame, (size_t)frame_size) != 0) {
            usleep(20000);
        }

        if (helper_status != 0) {
            usleep(500000);
        }
    }

    pthread_cleanup_pop(1);
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
            {"h", no_argument, 0, 0},
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
    stop_helper();

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

    if (resolve_paths() != 0) {
        OPRINT("failed to initialise viewer paths\n");
        return 1;
    }

    unlink(disable_flag_path);
    viewer_disabled = 0;
    viewer_disabled_warned = 0;

    if (helper_available) {
        start_helper_if_needed();
    }

    pthread_create(&worker, 0, worker_thread, NULL);
    pthread_detach(worker);
    return 0;
}

int output_cmd(int plugin_id, unsigned int control_id, unsigned int group, int value, char *valueStr)
{
    return 0;
}

