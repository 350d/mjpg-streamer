/*******************************************************************************
# Linuc-UVC streaming input-plugin for MJPG-streamer                           #
#                                                                              #
# This package work with the Logitech UVC based webcams with the mjpeg feature #
#                                                                              #
# Copyright (C) 2005 2006 Laurent Pinchart &&  Michel Xhaard                   #
#                    2007 Lucas van Staden                                     #
#                    2007 Tom Stöveken                                         #
#                                                                              #
# This program is free software; you can redistribute it and/or modify         #
# it under the terms of the GNU General Public License as published by         #
# the Free Software Foundation; either version 2 of the License, or            #
# (at your option) any later version.                                          #
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

#ifndef V4L2_UVC_H
#define V4L2_UVC_H


#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>

#include <linux/types.h>          /* for videodev2.h */
#include <linux/videodev2.h>

#include "../../mjpg_streamer.h"
#define NB_BUFFER 4


#define IOCTL_RETRY 4

/* ioctl with a number of retries in the case of I/O failure
* args:
* fd - device descriptor
* IOCTL_X - ioctl reference
* arg - pointer to ioctl data
* returns - ioctl result
*/
int xioctl(int fd, int IOCTL_X, void *arg);

#ifdef USE_LIBV4L2
#include <libv4l2.h>
#define IOCTL_VIDEO(fd, req, value) v4l2_ioctl(fd, req, value)
#define OPEN_VIDEO(fd, flags) v4l2_open(fd, flags)
#define CLOSE_VIDEO(fd) v4l2_close(fd)
#else
#define IOCTL_VIDEO(fd, req, value) ioctl(fd, req, value)
#define OPEN_VIDEO(fd, flags) open(fd, flags)
#define CLOSE_VIDEO(fd) close(fd)
#endif

typedef enum _streaming_state streaming_state;
enum _streaming_state {
    STREAMING_OFF = 0,
    STREAMING_ON = 1,
    STREAMING_PAUSED = 2,
};

struct vdIn {
    int fd;
    char *videodevice;
    char *status;
    char *pictName;
    struct v4l2_capability cap;
    struct v4l2_format fmt;
    struct v4l2_buffer buf;
    struct v4l2_requestbuffers rb;
    void *mem[NB_BUFFER];
    unsigned char *tmpbuffer;
    unsigned char *framebuffer;
    streaming_state streamingState;
    void *context_ptr; /* pointer to context for pause signaling */
    
    /* Static buffers for performance optimization */
    unsigned char static_framebuffer[640 * 480 * 4]; /* Default 640x480 with margin */
    unsigned char static_tmpbuffer[640 * 480 * 4];   /* Default 640x480 with margin */
    int use_static_buffers; /* Flag to use static buffers */
    size_t static_buffer_size; /* Actual size of static buffers */
    
    /* Buffer size optimization */
    size_t optimal_buffer_size; /* Pre-calculated optimal buffer size */
    int buffer_alignment; /* Buffer alignment for better performance */
    int grabmethod;
    int width;
    int height;
    int fps;
    int formatIn;
    int formatOut;
    int framesizeIn;
    int signalquit;
    int toggleAvi;
    int getPict;
    int rawFrameCapture;
    /* raw frame capture */
    unsigned int fileCounter;
    /* raw frame stream capture */
    unsigned int rfsFramesWritten;
    unsigned int rfsBytesWritten;
    /* raw stream capture */
    FILE *captureFile;
    unsigned int framesWritten;
    unsigned int bytesWritten;
    int framecount;
    int recordstart;
    int recordtime;
    uint32_t tmpbytesused;
    struct timeval tmptimestamp;
    v4l2_std_id vstd;
    unsigned long frame_period_time; // in ms
    unsigned char soft_framedrop;
    unsigned int dv_timings;
    unsigned char direct_copy_used; // flag for MJPEG direct copy optimization
};

/* optional initial settings */
typedef struct {
    int quality_set, quality,
        sh_set, sh,
        co_set, co,
        br_set, br_auto, br,
        sa_set, sa,
        wb_set, wb_auto, wb,
        ex_set, ex_auto, ex,
        bk_set, bk,
        rot_set, rot,
        hf_set, hf,
        vf_set, vf,
        pl_set, pl,
        gain_set, gain_auto, gain,
        cagc_set, cagc_auto, cagc,
        cb_set, cb_auto, cb;
} context_settings;

/* context of each camera thread */
typedef struct {
    int id;
    globals *pglobal;
    pthread_t threadID;
    pthread_mutex_t controls_mutex;
    pthread_cond_t pause_cond;
    pthread_mutex_t pause_mutex;
    struct vdIn *videoIn;
    context_settings *init_settings;
    
    /* Optimized select() loop - pre-allocated fd_sets */
    fd_set rd_fds;
    fd_set ex_fds;
    fd_set wr_fds;
    int max_fd;
    int fd_initialized;
    
    /* Optimized timestamp handling */
    struct timeval base_timestamp;
    unsigned long frame_counter;
    unsigned long timestamp_offset_us;
    
    /* TurboJPEG handle caching for YUV compression */
    void *tj_handle;
    int tj_handle_initialized;
    
    /* Pre-allocated buffers for YUV compression */
    unsigned char *yuv_line_buffer;
    unsigned char *yuv_rgb_buffer;
    int yuv_buffers_allocated;
} context;

int init_videoIn(struct vdIn *vd, char *device, int width, int height, int fps, int format, int grabmethod, globals *pglobal, int id, v4l2_std_id vstd);
void enumerateControls(struct vdIn *vd, globals *pglobal, int id);
void control_readed(struct vdIn *vd, struct v4l2_queryctrl *ctrl, globals *pglobal, int id);
int setResolution(struct vdIn *vd, int width, int height);

int memcpy_picture(unsigned char *out, unsigned char *buf, int size);
int uvcGrab(struct vdIn *vd);
int close_v4l2(struct vdIn *vd);

int video_enable(struct vdIn *vd);
int video_set_dv_timings(struct vdIn *vd);
int video_handle_event(struct vdIn *vd);

int v4l2GetControl(struct vdIn *vd, int control);
int v4l2SetControl(struct vdIn *vd, int control, int value, int plugin_number, globals *pglobal);
int v4l2UpControl(struct vdIn *vd, int control);
int v4l2DownControl(struct vdIn *vd, int control);
int v4l2ToggleControl(struct vdIn *vd, int control);
int v4l2ResetControl(struct vdIn *vd, int control);

/* MJPEG optimization functions */
int memcpy_mjpeg_direct(unsigned char *v4l2_buf, unsigned char *global_buf, int size, int minimum_size);

/* Select loop optimization functions */
int init_optimized_select(context *pcontext);
void cleanup_optimized_select(context *pcontext);
int optimized_select_wait(context *pcontext, int timeout);

/* Timestamp optimization functions */
void init_optimized_timestamp(context *pcontext, int fps);
void get_optimized_timestamp(context *pcontext, struct timeval *timestamp);

/* TurboJPEG handle caching functions */
int init_turbojpeg_handle(context *pcontext);
void cleanup_turbojpeg_handle(context *pcontext);
int init_yuv_buffers(context *pcontext, int width, int height);
void cleanup_yuv_buffers(context *pcontext);

/* Optimized YUV to JPEG compression */
int compress_yuv_to_jpeg_optimized(context *pcontext, struct vdIn *vd, unsigned char *buffer, int size, int quality);

#endif
