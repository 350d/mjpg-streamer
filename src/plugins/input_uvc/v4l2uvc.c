/*******************************************************************************
# Linux-UVC streaming input-plugin for MJPG-streamer                           #
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

#include <stdlib.h>
#include <errno.h>
#include <turbojpeg.h>
#include "v4l2uvc.h"
#include "huffman.h"
#include "dynctrl.h"

/* TurboJPEG constants fallback for older versions */
#ifndef TJPF_RGB
#define TJPF_RGB 2
#endif
#ifndef TJSAMP_444
#define TJSAMP_444 0
#endif
#ifndef TJPF_GRAY
#define TJPF_GRAY 1
#endif

/* Forward declarations removed - context is defined in v4l2uvc.h */
#include "../../utils.h"

/* SIMD functions are now in utils.c */

static int debug = 0;

/* SIMD functions are now provided by utils.c */

/* Signal pause condition when streaming state changes */
static void signal_pause_condition(struct vdIn *vd) {
    if (vd->context_ptr) {
        context *pctx = (context*)vd->context_ptr;
        pthread_mutex_lock(&pctx->pause_mutex);
        pthread_cond_signal(&pctx->pause_cond);
        pthread_mutex_unlock(&pctx->pause_mutex);
    }
}

/* fcc2s - convert pixelformat to string
* (Obtained from vtl-utils: v4l2-ctl.cpp)
* args:
* fmsString - char* to hold string
* size - size of allocated memory for string
* pixelformat - v4l2 pixel format identidifier
*/
void fcc2s(char* fmtString, unsigned int size, unsigned int pixelformat)
{
  if ( size < 8 )
  {
    fmtString[0] = '\0';
    return;
  }
  

  fmtString[0] = pixelformat & 0x7f;
  fmtString[1] = (pixelformat >>  8 ) & 0x7f;
  fmtString[2] = (pixelformat >>  16 ) & 0x7f;
  fmtString[3] = (pixelformat >> 24 ) & 0x7f;
  if (pixelformat & (1 << 31))
  {
    fmtString[4] = '-';
    fmtString[5] = 'B';
    fmtString[6] = 'E';
    fmtString[7] = '\0';
  }
  else
  {
    fmtString[4] = '\0';
  }
  return;
}

/* ioctl with a number of retries in the case of failure
* args:
* fd - device descriptor
* IOCTL_X - ioctl reference
* arg - pointer to ioctl data
* returns - ioctl result
*/
int xioctl(int fd, int IOCTL_X, void *arg)
{
    int ret = 0;
    int tries = IOCTL_RETRY;
    do {
        ret = IOCTL_VIDEO(fd, IOCTL_X, arg);
    } while(ret && tries-- &&
            ((errno == EINTR) || (errno == EAGAIN) || (errno == ETIMEDOUT)));

    if(ret && (tries <= 0)) fprintf(stderr, "ioctl (%i) retried %i times - giving up: %s)\n", IOCTL_X, IOCTL_RETRY, strerror(errno));

    return (ret);
}

static int init_v4l2(struct vdIn *vd);
static int init_framebuffer(struct vdIn *vd);
static void free_framebuffer(struct vdIn *vd);

int init_videoIn(struct vdIn *vd, char *device, int width,
                 int height, int fps, int format, int grabmethod, globals *pglobal, int id, v4l2_std_id vstd)
{
    if(vd == NULL || device == NULL)
        return -1;
    if(width == 0 || height == 0)
        return -1;
    if(grabmethod < 0 || grabmethod > 1)
        grabmethod = 1;     //mmap by default;
    
    /* Initialize SIMD capabilities on first call */
    static int simd_initialized = 0;
    if (!simd_initialized) {
        detect_simd_capabilities();
        simd_initialized = 1;
    }
    vd->videodevice = NULL;
    vd->status = NULL;
    vd->pictName = NULL;
    vd->videodevice = (char *) calloc(1, 16 * sizeof(char));
    vd->status = (char *) calloc(1, 100 * sizeof(char));
    vd->pictName = (char *) calloc(1, 80 * sizeof(char));
    snprintf(vd->videodevice, (16 - 1), "%s", device);
    vd->toggleAvi = 0;
    vd->getPict = 0;
    vd->signalquit = 1;
    vd->width = width;
    vd->height = height;
    vd->fps = fps;
    vd->formatIn = format;
	vd->vstd = vstd;
    vd->grabmethod = grabmethod;
    vd->soft_framedrop = 0;

    if(init_v4l2(vd) < 0) {
        goto error;
    }

    // getting the name of the input source
    struct v4l2_input in_struct;
    memset(&in_struct, 0, sizeof(struct v4l2_input));
    in_struct.index = 0;
    if (xioctl(vd->fd, VIDIOC_ENUMINPUT,  &in_struct) == 0) {
        int nameLength = strlen((char*)&in_struct.name);
        pglobal->in[id].name = malloc((1+nameLength)*sizeof(char));
        sprintf(pglobal->in[id].name, "%s", in_struct.name);
        DBG("Input name: %s\n", in_struct.name);
    } else {
        DBG("VIDIOC_ENUMINPUT failed\n");
    }

    // enumerating formats

    struct v4l2_format currentFormat;
    memset(&currentFormat, 0, sizeof(struct v4l2_format));
    currentFormat.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(vd->fd, VIDIOC_G_FMT, &currentFormat) == 0) {
        DBG("Current size: %dx%d\n",
             currentFormat.fmt.pix.width,
             currentFormat.fmt.pix.height);
    }

    pglobal->in[id].in_formats = NULL;
    for(pglobal->in[id].formatCount = 0; 1; pglobal->in[id].formatCount++) {
        struct v4l2_fmtdesc fmtdesc;
        memset(&fmtdesc, 0, sizeof(struct v4l2_fmtdesc));
        fmtdesc.index = pglobal->in[id].formatCount;
        fmtdesc.type  = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if(xioctl(vd->fd, VIDIOC_ENUM_FMT, &fmtdesc) < 0) {
            break;
        }

        if (pglobal->in[id].in_formats == NULL) {
            pglobal->in[id].in_formats = (input_format*)calloc(1, sizeof(input_format));
        } else {
            pglobal->in[id].in_formats = (input_format*)realloc(pglobal->in[id].in_formats, (pglobal->in[id].formatCount + 1) * sizeof(input_format));
        }

        if (pglobal->in[id].in_formats == NULL) {
            LOG("Calloc/realloc failed: %s\n", strerror(errno));
            return -1;
        }

        memcpy(&pglobal->in[id].in_formats[pglobal->in[id].formatCount], &fmtdesc, sizeof(struct v4l2_fmtdesc));

        if(fmtdesc.pixelformat == format)
            pglobal->in[id].currentFormat = pglobal->in[id].formatCount;

        DBG("Supported format: %s\n", fmtdesc.description);
        struct v4l2_frmsizeenum fsenum;
        memset(&fsenum, 0, sizeof(struct v4l2_frmsizeenum));
        fsenum.pixel_format = fmtdesc.pixelformat;
        int j = 0;
        pglobal->in[id].in_formats[pglobal->in[id].formatCount].supportedResolutions = NULL;
        pglobal->in[id].in_formats[pglobal->in[id].formatCount].resolutionCount = 0;
        pglobal->in[id].in_formats[pglobal->in[id].formatCount].currentResolution = -1;
        while(1) {
            fsenum.index = j;
            j++;
            if(xioctl(vd->fd, VIDIOC_ENUM_FRAMESIZES, &fsenum) == 0) {
                pglobal->in[id].in_formats[pglobal->in[id].formatCount].resolutionCount++;

                if (pglobal->in[id].in_formats[pglobal->in[id].formatCount].supportedResolutions == NULL) {
                    pglobal->in[id].in_formats[pglobal->in[id].formatCount].supportedResolutions = (input_resolution*)
                            calloc(1, sizeof(input_resolution));
                } else {
                    pglobal->in[id].in_formats[pglobal->in[id].formatCount].supportedResolutions = (input_resolution*)
                            realloc(pglobal->in[id].in_formats[pglobal->in[id].formatCount].supportedResolutions, j * sizeof(input_resolution));
                }

                if (pglobal->in[id].in_formats[pglobal->in[id].formatCount].supportedResolutions == NULL) {
                    LOG("Calloc/realloc failed\n");
                    return -1;
                }

                pglobal->in[id].in_formats[pglobal->in[id].formatCount].supportedResolutions[j-1].width = fsenum.discrete.width;
                pglobal->in[id].in_formats[pglobal->in[id].formatCount].supportedResolutions[j-1].height = fsenum.discrete.height;
                if(format == fmtdesc.pixelformat) {
                    pglobal->in[id].in_formats[pglobal->in[id].formatCount].currentResolution = (j - 1);
                    DBG("\tSupported size with the current format: %dx%d\n", fsenum.discrete.width, fsenum.discrete.height);
                } else {
                    DBG("\tSupported size: %dx%d\n", fsenum.discrete.width, fsenum.discrete.height);
                }
            } else {
                break;
            }
        }
    }

    if (init_framebuffer(vd) < 0) {
        goto error;
    }

    return 0;
error:
    free_framebuffer(vd);
    free(pglobal->in[id].in_parameters);
    free(vd->videodevice);
    free(vd->status);
    free(vd->pictName);
    CLOSE_VIDEO(vd->fd);
    return -1;
}

/* Calculate optimal buffer size with alignment for better performance */
static size_t calculate_optimal_buffer_size(int width, int height, int format) {
    size_t base_size = 0;
    
    switch (format) {
        case V4L2_PIX_FMT_JPEG:
        case V4L2_PIX_FMT_MJPEG:
            base_size = (size_t) width * (height + 8) * 2;
            break;
        case V4L2_PIX_FMT_RGB24:
            base_size = (size_t) width * height * 3;
            break;
        case V4L2_PIX_FMT_RGB565:
        case V4L2_PIX_FMT_YUYV:
        case V4L2_PIX_FMT_UYVY:
            base_size = (size_t) width * height * 2;
            break;
        default:
            base_size = (size_t) width * height * 2;
            break;
    }
    
    /* Align to 16-byte boundary for better memory access performance */
    return (base_size + 15) & ~15;
}

/* Get format name for debugging */
static const char* get_format_name(int format) {
    switch (format) {
        case V4L2_PIX_FMT_MJPEG: return "MJPEG";
        case V4L2_PIX_FMT_JPEG: return "JPEG";
        case V4L2_PIX_FMT_RGB24: return "RGB24";
        case V4L2_PIX_FMT_RGB565: return "RGB565";
        case V4L2_PIX_FMT_YUYV: return "YUYV";
        case V4L2_PIX_FMT_UYVY: return "UYVY";
        default: return "UNKNOWN";
    }
}

/* Get recommended static buffer size based on actual camera settings */
static size_t get_recommended_static_buffer_size(struct vdIn *vd) {
    size_t required = calculate_optimal_buffer_size(vd->width, vd->height, vd->formatIn);
    size_t static_buffer_size = sizeof(vd->static_framebuffer);
    
    /* Check if static buffer is sufficient for current settings */
    if (required <= static_buffer_size) {
        /* Static buffer is large enough, use it */
        return static_buffer_size;
    } else {
        /* Static buffer too small, recommend dynamic allocation */
        return 0;
    }
}

static int init_framebuffer(struct vdIn *vd) {
    /* alloc a temp buffer to reconstruct the pict */
    vd->framesizeIn = (vd->width * vd->height << 1);
    size_t required_size = 0;
    
    /* Calculate optimal buffer size with alignment */
    vd->optimal_buffer_size = calculate_optimal_buffer_size(vd->width, vd->height, vd->formatIn);
    required_size = vd->optimal_buffer_size;
    
    switch (vd->formatIn) {
        case V4L2_PIX_FMT_JPEG:
            // Fall-through intentional
        case V4L2_PIX_FMT_MJPEG: // in JPG mode the frame size is varies at every frame, so we allocate a bit bigger buffer
            break;
        case V4L2_PIX_FMT_RGB24:
            vd->framesizeIn = (vd->width * vd->height) * 3;
            break;
        case V4L2_PIX_FMT_RGB565: // buffer allocation for non varies on frame size formats
        case V4L2_PIX_FMT_YUYV:
        case V4L2_PIX_FMT_UYVY:
            break;
        default:
            fprintf(stderr, "Unknown vd->formatIn\n");
            return -1;
    }
    
    /* Set static buffer size based on actual camera settings */
    size_t recommended_size = get_recommended_static_buffer_size(vd);
    vd->static_buffer_size = (recommended_size > 0) ? recommended_size : sizeof(vd->static_framebuffer);
    
    /* Try to use static buffers first for better performance */
    if (required_size <= vd->static_buffer_size) {
        vd->framebuffer = vd->static_framebuffer;
        vd->tmpbuffer = vd->static_tmpbuffer;
        vd->use_static_buffers = 1;
        
        /* For MJPEG, point tmpbuffer to same memory as framebuffer */
        if (vd->formatIn == V4L2_PIX_FMT_MJPEG || vd->formatIn == V4L2_PIX_FMT_JPEG) {
            vd->tmpbuffer = vd->framebuffer;
        }
        
        DBG("Using static buffers: %zu bytes for %dx%d %s (requested: %zu bytes)\n", 
            vd->static_buffer_size, vd->width, vd->height, 
            get_format_name(vd->formatIn), required_size);
    } else {
        /* Fallback to dynamic allocation for very large buffers */
        DBG("Static buffer too small (%zu < %zu) for %dx%d %s, using dynamic allocation\n", 
            vd->static_buffer_size, required_size, vd->width, vd->height,
            get_format_name(vd->formatIn));
        vd->framebuffer = (unsigned char *) calloc(1, required_size);
        if(!vd->framebuffer)
            return -1;
        vd->tmpbuffer = vd->framebuffer; // Point to same buffer to avoid double allocation
        vd->use_static_buffers = 0;
    }
    
    return 0;
}

static void free_framebuffer(struct vdIn *vd) {
    // Only free dynamic buffers, static buffers are automatically freed
    if (vd->framebuffer && !vd->use_static_buffers) {
        free(vd->framebuffer);
    }
    vd->framebuffer = NULL;
    vd->tmpbuffer = NULL;
    vd->use_static_buffers = 0;
}

static int init_v4l2(struct vdIn *vd)
{
    int i;
    int ret = 0;
    if((vd->fd = OPEN_VIDEO(vd->videodevice, O_RDWR)) == -1) {
        perror("ERROR opening V4L interface");
        DBG("errno: %d", errno);
        return -1;
    }

    memset(&vd->cap, 0, sizeof(struct v4l2_capability));
    ret = xioctl(vd->fd, VIDIOC_QUERYCAP, &vd->cap);
    if(ret < 0) {
        fprintf(stderr, "Error opening device %s: unable to query device.\n", vd->videodevice);
        goto fatal;
    }

    if((vd->cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) == 0) {
        fprintf(stderr, "Error opening device %s: video capture not supported.\n",
                vd->videodevice);
        goto fatal;;
    }

    if(vd->grabmethod) {
        if(!(vd->cap.capabilities & V4L2_CAP_STREAMING)) {
            fprintf(stderr, "%s does not support streaming i/o\n", vd->videodevice);
            goto fatal;
        }
    } else {
        if(!(vd->cap.capabilities & V4L2_CAP_READWRITE)) {
            fprintf(stderr, "%s does not support read i/o\n", vd->videodevice);
            goto fatal;
        }
    }

    if (vd->vstd != V4L2_STD_UNKNOWN) {
        if (ioctl(vd->fd, VIDIOC_S_STD, &vd->vstd) == -1) {
            fprintf(stderr, "Can't set video standard: %s\n",strerror(errno));
            goto fatal;
        }
    }

    if (vd->dv_timings) {
        if (video_set_dv_timings(vd)) {
            goto fatal;
        }

        struct v4l2_event_subscription sub;
        memset(&sub, 0, sizeof(sub));
        sub.type = V4L2_EVENT_SOURCE_CHANGE;
        if (ioctl(vd->fd, VIDIOC_SUBSCRIBE_EVENT, &sub) < 0) {
            IPRINT("Can\'t subscribe to V4L2_EVENT_SOURCE_CHANGE: %s\n", strerror(errno));
        }
    }

    /*
     * set format in
     */
    memset(&vd->fmt, 0, sizeof(struct v4l2_format));
    vd->fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    vd->fmt.fmt.pix.width = vd->width;
    vd->fmt.fmt.pix.height = vd->height;
    vd->fmt.fmt.pix.pixelformat = vd->formatIn;
    vd->fmt.fmt.pix.field = V4L2_FIELD_ANY;
    ret = xioctl(vd->fd, VIDIOC_S_FMT, &vd->fmt);
    if(ret < 0) {
        fprintf(stderr, "Unable to set format: %d res: %dx%d\n", vd->formatIn, vd->width, vd->height);
        goto fatal;
    }

    /* 
     * Check reoslution 
     */
    if((vd->fmt.fmt.pix.width != vd->width) ||
            (vd->fmt.fmt.pix.height != vd->height)) {
       fprintf(stderr, " i: The specified resolution is unavailable, using: width %d height %d instead \n", vd->fmt.fmt.pix.width, vd->fmt.fmt.pix.height);
        vd->width = vd->fmt.fmt.pix.width;
        vd->height = vd->fmt.fmt.pix.height;
    }
    /*
     * Check format
     */
    if(vd->formatIn != vd->fmt.fmt.pix.pixelformat) {
      char fmtStringRequested[8];
      char fmtStringObtained[8];
      fcc2s(fmtStringObtained,8,vd->fmt.fmt.pix.pixelformat);
      fcc2s(fmtStringRequested,8,vd->formatIn);
      fprintf(stderr, " i: Could not obtain the requested pixelformat: %s , driver gave us: %s\n",fmtStringRequested,fmtStringObtained);
      fprintf(stderr, "    ... will try to handle this by checking against supported formats. \n");

      switch(vd->fmt.fmt.pix.pixelformat){
      case V4L2_PIX_FMT_JPEG:
	// Fall-through intentional
      case V4L2_PIX_FMT_MJPEG:
	fprintf(stderr, "    ... Falling back to the faster MJPG mode (consider changing cmd line options).\n");
	vd->formatIn = vd->fmt.fmt.pix.pixelformat;
	break;
      case V4L2_PIX_FMT_YUYV:
	fprintf(stderr, "    ... Falling back to YUV mode (consider using -yuv option). Note that this requires much more CPU power\n");
	vd->formatIn = vd->fmt.fmt.pix.pixelformat;
        break;
      case V4L2_PIX_FMT_UYVY:
	fprintf(stderr, "    ... Falling back to UYVY mode (consider using -uyvy option). Note that this requires much more CPU power\n");
	vd->formatIn = vd->fmt.fmt.pix.pixelformat;
        break;
      case V4L2_PIX_FMT_RGB24:
	fprintf(stderr, "    ... Falling back to RGB24 mode (consider using -fourcc RGB24 option). Note that this requires much more CPU power\n");
	vd->formatIn = vd->fmt.fmt.pix.pixelformat;
	break;
      case V4L2_PIX_FMT_RGB565:
	fprintf(stderr, "    ... Falling back to RGB565 mode (consider using -fourcc RGBP option). Note that this requires much more CPU power\n");
	vd->formatIn = vd->fmt.fmt.pix.pixelformat;
	break;
      default:
	goto fatal;
	break;
      }
    }
 
    /*
     * set framerate
     */

    if (vd->fps != -1) {
        struct v4l2_streamparm *setfps;
        setfps = (struct v4l2_streamparm *) calloc(1, sizeof(struct v4l2_streamparm));
        memset(setfps, 0, sizeof(struct v4l2_streamparm));
        setfps->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

        /*
        * first query streaming parameters to determine that the FPS selection is supported
        */
        ret = xioctl(vd->fd, VIDIOC_G_PARM, setfps);
        if (ret == 0) {
            if (setfps->parm.capture.capability & V4L2_CAP_TIMEPERFRAME) {
                memset(setfps, 0, sizeof(struct v4l2_streamparm));
                setfps->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                setfps->parm.capture.timeperframe.numerator = 1;
                setfps->parm.capture.timeperframe.denominator = vd->fps==-1?255:vd->fps; // if no default fps set set it to maximum

                ret = xioctl(vd->fd, VIDIOC_S_PARM, setfps);
                if (ret) {
                    perror("Unable to set the FPS\n");
                } else {
                    if (vd->fps != setfps->parm.capture.timeperframe.denominator) {
                        IPRINT("FPS coerced ......: from %d to %d\n", vd->fps, setfps->parm.capture.timeperframe.denominator);
                    }
                }
            } else {
                perror("Setting FPS on the capture device is not supported, fallback to software framedropping\n");
                vd->soft_framedrop = 1;
                vd->frame_period_time = 1000/vd->fps; // calcualate frame period time in ms
                IPRINT("Frame period time ......: %ld ms\n", vd->frame_period_time);
            }
        } else {
            perror("Unable to query that the FPS change is supported\n");
        }
    }

    /*
     * request buffers
     */
    memset(&vd->rb, 0, sizeof(struct v4l2_requestbuffers));
    vd->rb.count = NB_BUFFER;
    vd->rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    vd->rb.memory = V4L2_MEMORY_MMAP;

    ret = xioctl(vd->fd, VIDIOC_REQBUFS, &vd->rb);
    if(ret < 0) {
        perror("Unable to allocate buffers");
        goto fatal;
    }

    /*
     * map the buffers
     */
    for(i = 0; i < NB_BUFFER; i++) {
        memset(&vd->buf, 0, sizeof(struct v4l2_buffer));
        vd->buf.index = i;
        vd->buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        vd->buf.memory = V4L2_MEMORY_MMAP;
        ret = xioctl(vd->fd, VIDIOC_QUERYBUF, &vd->buf);
        if(ret < 0) {
            perror("Unable to query buffer");
            goto fatal;
        }

        if(debug)
            fprintf(stderr, "length: %u offset: %u\n", vd->buf.length, vd->buf.m.offset);

        vd->mem[i] = mmap(0 /* start anywhere */ ,
                          vd->buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, vd->fd,
                          vd->buf.m.offset);
        if(vd->mem[i] == MAP_FAILED) {
            perror("Unable to map buffer");
            goto fatal;
        }
        if(debug)
            fprintf(stderr, "Buffer mapped at address %p.\n", vd->mem[i]);
    }

    /*
     * Queue the buffers.
     */
    for(i = 0; i < NB_BUFFER; ++i) {
        memset(&vd->buf, 0, sizeof(struct v4l2_buffer));
        vd->buf.index = i;
        vd->buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        vd->buf.memory = V4L2_MEMORY_MMAP;
        ret = xioctl(vd->fd, VIDIOC_QBUF, &vd->buf);
        if(ret < 0) {
            perror("Unable to queue buffer");
            goto fatal;;
        }
    }
    return 0;
fatal:
    fprintf(stderr, "Init v4L2 failed !! exit fatal\n");
    return -1;

}

int video_enable(struct vdIn *vd)
{
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    int ret;

    ret = xioctl(vd->fd, VIDIOC_STREAMON, &type);
    if(ret < 0) {
        perror("Unable to start capture");
        return ret;
    }
    vd->streamingState = STREAMING_ON;
    signal_pause_condition(vd);
    return 0;
}

static int video_disable(struct vdIn *vd, streaming_state disabledState)
{
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    int ret;
    DBG("STopping capture\n");
    ret = xioctl(vd->fd, VIDIOC_STREAMOFF, &type);
    if(ret != 0) {
        perror("Unable to stop capture");
        return ret;
    }
    DBG("STopping capture done\n");
    vd->streamingState = disabledState;
    signal_pause_condition(vd);
    return 0;
}

int video_set_dv_timings(struct vdIn *vd)
{
    struct v4l2_dv_timings timings;
    v4l2_std_id std;

    memset(&timings, 0, sizeof(timings));
    if (xioctl(vd->fd, VIDIOC_QUERY_DV_TIMINGS, &timings) >= 0) {
        IPRINT("QUERY_DV_TIMINGS returned %ux%u pixclk %llu\n", timings.bt.width, timings.bt.height, timings.bt.pixelclock);
        // Can read DV timings, so set them.
        if (xioctl(vd->fd, VIDIOC_S_DV_TIMINGS, &timings) < 0) {
            perror("Failed to set DV timings");
            return -1;
        } else {
            vd->width = timings.bt.width;
            vd->height = timings.bt.height;
        }
    } else {
        memset(&std, 0, sizeof(std));
        if (xioctl(vd->fd, VIDIOC_QUERYSTD, &std) >= 0) {
            // Can read standard, so set it.
            if (xioctl(vd->fd, VIDIOC_S_STD, &std) < 0) {
                perror("Failed to set standard");
                return -1;
            }
        }
    }
    return 0;
}

int video_handle_event(struct vdIn *vd)
{
    struct v4l2_event ev;
    if (!ioctl(vd->fd, VIDIOC_DQEVENT, &ev)) {
        switch (ev.type) {
            case V4L2_EVENT_SOURCE_CHANGE:
                IPRINT("V4L2_EVENT_SOURCE_CHANGE: Source changed\n");
                if (setResolution(vd, vd->width, vd->height) < 0) {
                    return -1;
                }
                break;
            case V4L2_EVENT_EOS:
                IPRINT("V4L2_EVENT_EOS\n");
                break;
        }
    }
    return 0;
}

/******************************************************************************
Description.:
Input Value.:
Return Value:
******************************************************************************/
int is_huffman(unsigned char *buf)
{
    unsigned char *ptbuf;
    int i = 0;
    ptbuf = buf;
    while(((ptbuf[0] << 8) | ptbuf[1]) != 0xffda) {
        if(i++ > 2048)
            return 0;
        if(((ptbuf[0] << 8) | ptbuf[1]) == 0xffc4)
            return 1;
        ptbuf++;
    }
    return 0;
}

/******************************************************************************
Description.: Initialize optimized select() loop
Input Value.: context pointer
Return Value: 0 on success, -1 on error
******************************************************************************/
int init_optimized_select(context *pcontext)
{
    if (pcontext == NULL || pcontext->videoIn == NULL) {
        return -1;
    }
    
    /* Initialize fd_sets once */
    FD_ZERO(&pcontext->rd_fds);
    FD_ZERO(&pcontext->ex_fds);
    FD_ZERO(&pcontext->wr_fds);
    
    FD_SET(pcontext->videoIn->fd, &pcontext->rd_fds);
    FD_SET(pcontext->videoIn->fd, &pcontext->ex_fds);
    FD_SET(pcontext->videoIn->fd, &pcontext->wr_fds);
    
    pcontext->max_fd = pcontext->videoIn->fd + 1;
    pcontext->fd_initialized = 1;
    
    return 0;
}

/******************************************************************************
Description.: Cleanup optimized select() loop
Input Value.: context pointer
Return Value: -
******************************************************************************/
void cleanup_optimized_select(context *pcontext)
{
    if (pcontext != NULL) {
        pcontext->fd_initialized = 0;
    }
}

/******************************************************************************
Description.: Optimized select() wait with pre-initialized fd_sets
Input Value.: context pointer, timeout in seconds
Return Value: select() result
******************************************************************************/
int optimized_select_wait(context *pcontext, int timeout)
{
    if (pcontext == NULL || !pcontext->fd_initialized) {
        return -1;
    }
    
    struct timeval tv;
    tv.tv_sec = timeout;
    tv.tv_usec = 0;
    
    return select(pcontext->max_fd, &pcontext->rd_fds, &pcontext->wr_fds, &pcontext->ex_fds, &tv);
}

/******************************************************************************
Description.: Initialize optimized timestamp handling
Input Value.: context pointer, FPS
Return Value: -
******************************************************************************/
void init_optimized_timestamp(context *pcontext, int fps)
{
    if (pcontext == NULL) {
        return;
    }
    
    /* Get base timestamp once */
    gettimeofday(&pcontext->base_timestamp, NULL);
    pcontext->frame_counter = 0;
    
    /* Calculate timestamp offset per frame in microseconds */
    if (fps > 0) {
        pcontext->timestamp_offset_us = 1000000 / fps; // 1 second = 1,000,000 microseconds
    } else {
        pcontext->timestamp_offset_us = 200000; // Default 5 FPS
    }
}

/******************************************************************************
Description.: Get optimized timestamp using base + frame offset
Input Value.: context pointer, timestamp output
Return Value: -
******************************************************************************/
void get_optimized_timestamp(context *pcontext, struct timeval *timestamp)
{
    if (pcontext == NULL || timestamp == NULL) {
        return;
    }
    
    /* Calculate timestamp based on base + frame offset */
    unsigned long total_offset_us = pcontext->frame_counter * pcontext->timestamp_offset_us;
    
    timestamp->tv_sec = pcontext->base_timestamp.tv_sec + (total_offset_us / 1000000);
    timestamp->tv_usec = pcontext->base_timestamp.tv_usec + (total_offset_us % 1000000);
    
    /* Handle overflow in microseconds */
    if (timestamp->tv_usec >= 1000000) {
        timestamp->tv_sec += 1;
        timestamp->tv_usec -= 1000000;
    }
    
    /* Increment frame counter for next frame */
    pcontext->frame_counter++;
}

/******************************************************************************
Description.: Initialize TurboJPEG handle for caching
Input Value.: context pointer
Return Value: 0 on success, -1 on error
******************************************************************************/
int init_turbojpeg_handle(context *pcontext)
{
    if (pcontext == NULL) {
        return -1;
    }
    
    /* Initialize TurboJPEG handle once */
    pcontext->tj_handle = tjInitCompress();
    if (pcontext->tj_handle == NULL) {
        return -1;
    }
    
    pcontext->tj_handle_initialized = 1;
    return 0;
}

/******************************************************************************
Description.: Cleanup TurboJPEG handle
Input Value.: context pointer
Return Value: -
******************************************************************************/
void cleanup_turbojpeg_handle(context *pcontext)
{
    if (pcontext != NULL && pcontext->tj_handle_initialized && pcontext->tj_handle != NULL) {
        tjDestroy(pcontext->tj_handle);
        pcontext->tj_handle = NULL;
        pcontext->tj_handle_initialized = 0;
    }
}

/******************************************************************************
Description.: Initialize pre-allocated YUV buffers
Input Value.: context pointer, width, height
Return Value: 0 on success, -1 on error
******************************************************************************/
int init_yuv_buffers(context *pcontext, int width, int height)
{
    if (pcontext == NULL || width <= 0 || height <= 0) {
        return -1;
    }
    
    /* Allocate line buffer for YUV conversion */
    pcontext->yuv_line_buffer = calloc(width * 3, 1);
    if (pcontext->yuv_line_buffer == NULL) {
        return -1;
    }
    
    /* Allocate RGB buffer for YUV to RGB conversion */
    pcontext->yuv_rgb_buffer = malloc(width * height * 3);
    if (pcontext->yuv_rgb_buffer == NULL) {
        free(pcontext->yuv_line_buffer);
        pcontext->yuv_line_buffer = NULL;
        return -1;
    }
    
    pcontext->yuv_buffers_allocated = 1;
    return 0;
}

/******************************************************************************
Description.: Cleanup pre-allocated YUV buffers
Input Value.: context pointer
Return Value: -
******************************************************************************/
void cleanup_yuv_buffers(context *pcontext)
{
    if (pcontext != NULL && pcontext->yuv_buffers_allocated) {
        if (pcontext->yuv_line_buffer != NULL) {
            free(pcontext->yuv_line_buffer);
            pcontext->yuv_line_buffer = NULL;
        }
        if (pcontext->yuv_rgb_buffer != NULL) {
            free(pcontext->yuv_rgb_buffer);
            pcontext->yuv_rgb_buffer = NULL;
        }
        pcontext->yuv_buffers_allocated = 0;
    }
}

/******************************************************************************
Description.: Optimized YUV to JPEG compression using cached handle and buffers
Input Value.: context pointer, video structure, buffer, size, quality
Return Value: compressed size on success, -1 on error
******************************************************************************/
int compress_yuv_to_jpeg_optimized(context *pcontext, struct vdIn *vd, unsigned char *buffer, int size, int quality)
{
    if (pcontext == NULL || vd == NULL || buffer == NULL || size <= 0) {
        return -1;
    }
    
    /* Check if TurboJPEG handle is initialized */
    if (!pcontext->tj_handle_initialized || pcontext->tj_handle == NULL) {
        return -1;
    }
    
    /* Check if YUV buffers are allocated */
    if (!pcontext->yuv_buffers_allocated || pcontext->yuv_line_buffer == NULL || pcontext->yuv_rgb_buffer == NULL) {
        return -1;
    }
    
    unsigned char *rgb_buffer = pcontext->yuv_rgb_buffer;
    unsigned long jpeg_size = 0;
    int result = -1;
    
    /* Convert YUV to RGB using pre-allocated buffer */
    if (vd->formatIn == V4L2_PIX_FMT_YUYV || vd->formatIn == V4L2_PIX_FMT_UYVY) {
        unsigned char *yuv = vd->framebuffer;
        unsigned char *rgb = rgb_buffer;
        int i;
        
        for (i = 0; i < vd->width * vd->height / 2; i++) {
            int y1, y2, u, v;
            
            if (vd->formatIn == V4L2_PIX_FMT_YUYV) {
                y1 = yuv[0]; u = yuv[1]; y2 = yuv[2]; v = yuv[3];
            } else { /* UYVY */
                u = yuv[0]; y1 = yuv[1]; v = yuv[2]; y2 = yuv[3];
            }
            
            /* Convert YUV to RGB for first pixel */
            int r1 = y1 + 1.402 * (v - 128);
            int g1 = y1 - 0.344136 * (u - 128) - 0.714136 * (v - 128);
            int b1 = y1 + 1.772 * (u - 128);
            
            /* Convert YUV to RGB for second pixel */
            int r2 = y2 + 1.402 * (v - 128);
            int g2 = y2 - 0.344136 * (u - 128) - 0.714136 * (v - 128);
            int b2 = y2 + 1.772 * (u - 128);
            
            /* Clamp values */
            rgb[0] = (r1 < 0) ? 0 : (r1 > 255) ? 255 : r1;
            rgb[1] = (g1 < 0) ? 0 : (g1 > 255) ? 255 : g1;
            rgb[2] = (b1 < 0) ? 0 : (b1 > 255) ? 255 : b1;
            rgb[3] = (r2 < 0) ? 0 : (r2 > 255) ? 255 : r2;
            rgb[4] = (g2 < 0) ? 0 : (g2 > 255) ? 255 : g2;
            rgb[5] = (b2 < 0) ? 0 : (b2 > 255) ? 255 : b2;
            
            yuv += 4;
            rgb += 6;
        }
    } else if (vd->formatIn == V4L2_PIX_FMT_RGB24) {
        rgb_buffer = vd->framebuffer;
    } else {
        return -1;
    }
    
    /* Compress to JPEG using cached handle */
    result = tjCompress2(pcontext->tj_handle, rgb_buffer, vd->width, 0, vd->height, TJPF_RGB, 
                        &buffer, &jpeg_size, TJSAMP_444, quality, 0);
    
    return (result == 0) ? (int)jpeg_size : -1;
}

/******************************************************************************
Description.: Direct copy for MJPEG with validation and DHT insertion
Input Value.: V4L2 buffer, global buffer, size, minimum size
Return Value: actual copied size
******************************************************************************/
int memcpy_mjpeg_direct(unsigned char *v4l2_buf, unsigned char *global_buf, int size, int minimum_size)
{
    /* Early validation - same as original */
    if (size <= minimum_size || !v4l2_buf || !global_buf) {
        return 0;
    }
    
    /* Check for Huffman tables - same logic as memcpy_picture */
    if(!is_huffman(v4l2_buf)) {
        unsigned char *ptcur = v4l2_buf;
        unsigned char *ptlimit = v4l2_buf + size;
        int pos = 0;
        
        /* Find Start of Frame marker (0xffc0) */
        while((((ptcur[0] << 8) | ptcur[1]) != 0xffc0) && (ptcur < ptlimit))
            ptcur++;
        if(ptcur >= ptlimit)
            return 0;
            
        int sizein = ptcur - v4l2_buf;
        
        /* Copy header part */
        if (sizein > 0) {
            simd_memcpy(global_buf + pos, v4l2_buf, sizein);
            pos += sizein;
        }
        
        /* Insert DHT data if needed */
        if (sizeof(dht_data) > 0) {
            simd_memcpy(global_buf + pos, dht_data, sizeof(dht_data));
            pos += sizeof(dht_data);
        }
        
        /* Copy remaining data */
        if ((size - sizein) > 0) {
            simd_memcpy(global_buf + pos, ptcur, size - sizein);
            pos += size - sizein;
        }
        
        return pos;
    } else {
        /* Direct copy for huffman data */
        simd_memcpy(global_buf, v4l2_buf, size);
        return size;
    }
}

/******************************************************************************
Description.:
Input Value.:
Return Value:
******************************************************************************/
int memcpy_picture(unsigned char *out, unsigned char *buf, int size)
{
    unsigned char *ptdeb, *ptlimit, *ptcur = buf;
    int sizein, pos = 0;

    /* Early return for empty or invalid data */
    if (size <= 0 || !buf || !out) {
        return 0;
    }

    if(!is_huffman(buf)) {
        ptdeb = ptcur = buf;
        ptlimit = buf + size;
        while((((ptcur[0] << 8) | ptcur[1]) != 0xffc0) && (ptcur < ptlimit))
            ptcur++;
        if(ptcur >= ptlimit)
            return pos;
        sizein = ptcur - ptdeb;

        /* SIMD-optimized memory copy with early exit */
        if (sizein > 0) {
            simd_memcpy(out + pos, buf, sizein);
            pos += sizein;
        }
        
        /* Only copy DHT data if needed */
        if (sizeof(dht_data) > 0) {
            simd_memcpy(out + pos, dht_data, sizeof(dht_data));
            pos += sizeof(dht_data);
        }
        
        /* Copy remaining data if any */
        if ((size - sizein) > 0) {
            simd_memcpy(out + pos, ptcur, size - sizein);
            pos += size - sizein;
        }
    } else {
        /* Direct copy for huffman data */
        simd_memcpy(out + pos, ptcur, size);
        pos += size;
    }
    return pos;
}

int uvcGrab(struct vdIn *vd)
{
#define HEADERFRAME1 0xaf
    int ret;

    if(vd->streamingState == STREAMING_OFF) {
        if(video_enable(vd))
            goto err;
    }
    memset(&vd->buf, 0, sizeof(struct v4l2_buffer));
    vd->buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    vd->buf.memory = V4L2_MEMORY_MMAP;

                    
    ret = xioctl(vd->fd, VIDIOC_DQBUF, &vd->buf);
    if(ret < 0) {
        perror("Unable to dequeue buffer");
        goto err;
    }

    switch(vd->formatIn) {
    case V4L2_PIX_FMT_JPEG:
        // Fall-through intentional
    case V4L2_PIX_FMT_MJPEG:
            
        if(vd->buf.bytesused <= HEADERFRAME1) {
            /* Prevent crash
             * on empty image */
            fprintf(stderr, "Ignoring empty buffer ...\n");
            break;
        }

        /* For MJPEG: use direct copy optimization if global buffer is available */
        if (vd->context_ptr != NULL) {
            
            /* Try direct copy to global buffer - this eliminates tmpbuffer copy */
            context *pcontext = (context *)vd->context_ptr;
            if (pcontext->id >= 0 && pcontext->pglobal != NULL) {
                
                /* Lock global buffer for direct copy */
                pthread_mutex_lock(&pcontext->pglobal->in[pcontext->id].db);
                
                
                /* Use direct MJPEG copy with validation */
                int copied_size = memcpy_mjpeg_direct(
                    vd->mem[vd->buf.index], 
                    pcontext->pglobal->in[pcontext->id].buf, 
                    vd->buf.bytesused, 
                    HEADERFRAME1
                );
                
                
                if (copied_size > 0) {
                    
                    /* Success - set size and timestamp */
                    pcontext->pglobal->in[pcontext->id].prev_size = pcontext->pglobal->in[pcontext->id].current_size;
                    pcontext->pglobal->in[pcontext->id].current_size = copied_size;
                    pcontext->pglobal->in[pcontext->id].size = copied_size;
                    pcontext->pglobal->in[pcontext->id].timestamp = vd->buf.timestamp;
                    /* Update global frame timestamp in ms and increment sequence for multi-consumer */
                    pcontext->pglobal->in[pcontext->id].frame_timestamp_ms = (vd->buf.timestamp.tv_sec * 1000LL) + (vd->buf.timestamp.tv_usec / 1000);
                    pcontext->pglobal->in[pcontext->id].frame_sequence++;
                    vd->tmpbytesused = copied_size;
                    vd->tmptimestamp = vd->buf.timestamp;
                    vd->direct_copy_used = 1; /* Mark as directly copied */
                    
                    
                    /* Signal fresh frame */
                
                    pthread_cond_broadcast(&pcontext->pglobal->in[pcontext->id].db_update);
                    
                    /* Unlock mutex before waiting for output plugins */
                    pthread_mutex_unlock(&pcontext->pglobal->in[pcontext->id].db);
                    
                    
                    if(debug) {
                        fprintf(stderr, "MJPEG direct copy: %d bytes\n", copied_size);
                    }
                    break; /* Skip fallback to tmpbuffer */
                } else {
                    /* Direct copy failed - unlock and fallback to tmpbuffer */
                    pthread_mutex_unlock(&pcontext->pglobal->in[pcontext->id].db);
                }
            }
        }

        /* Fallback: traditional tmpbuffer copy for compatibility */
        /* SIMD-optimized memory copy for large buffers with bounds checking */
        if (vd->buf.bytesused > 0 && vd->buf.bytesused <= vd->optimal_buffer_size) {
            simd_memcpy(vd->tmpbuffer, vd->mem[vd->buf.index], vd->buf.bytesused);
        } else {
            /* Fallback for oversized buffers */
            size_t copy_size = (vd->buf.bytesused > vd->optimal_buffer_size) ? 
                               vd->optimal_buffer_size : vd->buf.bytesused;
            simd_memcpy(vd->tmpbuffer, vd->mem[vd->buf.index], copy_size);
        }
        vd->tmpbytesused = vd->buf.bytesused;
        vd->tmptimestamp = vd->buf.timestamp;

        if(debug) {
            fprintf(stderr, "bytes in used %d \n", vd->buf.bytesused);
        }
        break;
    case V4L2_PIX_FMT_RGB24:
    case V4L2_PIX_FMT_RGB565:
    case V4L2_PIX_FMT_YUYV:
    case V4L2_PIX_FMT_UYVY:
        /* SIMD-optimized memory copy for frame buffers */
        if(vd->buf.bytesused > vd->framesizeIn) {
            simd_memcpy(vd->framebuffer, vd->mem[vd->buf.index], (size_t) vd->framesizeIn);
        } else {
            simd_memcpy(vd->framebuffer, vd->mem[vd->buf.index], (size_t) vd->buf.bytesused);
        }
        vd->tmpbytesused = vd->buf.bytesused;
        vd->tmptimestamp = vd->buf.timestamp;
        break;
    default:
        goto err;
        break;
    }

    ret = xioctl(vd->fd, VIDIOC_QBUF, &vd->buf);
    if(ret < 0) {
        perror("Unable to requeue buffer");
        goto err;
    }

    return 0;

err:
    vd->signalquit = 0;
    return -1;
}

int close_v4l2(struct vdIn *vd)
{
    if(vd->streamingState == STREAMING_ON)
        video_disable(vd, STREAMING_OFF);
    free_framebuffer(vd);
    free(vd->videodevice);
    free(vd->status);
    free(vd->pictName);
    vd->videodevice = NULL;
    vd->status = NULL;
    vd->pictName = NULL;

    return 0;
}

/* return >= 0 ok otherwhise -1 */
static int isv4l2Control(struct vdIn *vd, int control, struct v4l2_queryctrl *queryctrl)
{
    int err = 0;

    queryctrl->id = control;
    if((err = xioctl(vd->fd, VIDIOC_QUERYCTRL, queryctrl)) < 0) {
        //fprintf(stderr, "ioctl querycontrol error %d \n",errno);
        return -1;
    }

    if(queryctrl->flags & V4L2_CTRL_FLAG_DISABLED) {
        //fprintf(stderr, "control %s disabled \n", (char *) queryctrl->name);
        return -1;
    }

    if(queryctrl->type & V4L2_CTRL_TYPE_BOOLEAN) {
        return 1;
    }

    if(queryctrl->type & V4L2_CTRL_TYPE_INTEGER) {
        return 0;
    }

    fprintf(stderr, "contol %s unsupported  \n", (char *) queryctrl->name);
    return -1;
}

int v4l2GetControl(struct vdIn *vd, int control)
{
    struct v4l2_queryctrl queryctrl;
    struct v4l2_control control_s;
    int err;

    if((err = isv4l2Control(vd, control, &queryctrl)) < 0) {
        return -1;
    }

    control_s.id = control;
    if((err = xioctl(vd->fd, VIDIOC_G_CTRL, &control_s)) < 0) {
        return -1;
    }

    return control_s.value;
}

int v4l2SetControl(struct vdIn *vd, int control_id, int value, int plugin_number, globals *pglobal)
{
    struct v4l2_control control_s;
    int min, max;
    int ret = 0;
    int err;
    int i;
    int got = -1;
    DBG("Looking for the 0x%08x V4L2 control\n", control_id);
    for (i = 0; i<pglobal->in[plugin_number].parametercount; i++) {
        if (pglobal->in[plugin_number].in_parameters[i].ctrl.id == control_id) {
            got = 0;
            break;
        }
    }

    if (got == 0) { // we have found the control with the specified id
        DBG("V4L2 ctrl 0x%08x found\n", control_id);
        if (pglobal->in[plugin_number].in_parameters[i].class_id == V4L2_CTRL_CLASS_USER) {
            DBG("Control type: USER\n");
            min = pglobal->in[plugin_number].in_parameters[i].ctrl.minimum;
            max = pglobal->in[plugin_number].in_parameters[i].ctrl.maximum;

            if((value >= min) && (value <= max)) {
                control_s.id = control_id;
                control_s.value = value;
                if((err = xioctl(vd->fd, VIDIOC_S_CTRL, &control_s)) < 0) {
                    DBG("VIDIOC_S_CTRL failed\n");
                    return -1;
                } else {
                    DBG("V4L2 ctrl 0x%08x new value: %d\n", control_id, value);
                    pglobal->in[plugin_number].in_parameters[i].value = value;
                }
            } else {
                LOG("Value (%d) out of range (%d .. %d)\n", value, min, max);
            }
            return 0;
        } else { // not user class controls
            DBG("Control type: EXTENDED\n");
            struct v4l2_ext_controls ext_ctrls = {0};
            struct v4l2_ext_control ext_ctrl = {0};
            ext_ctrl.id = pglobal->in[plugin_number].in_parameters[i].ctrl.id;

            switch(pglobal->in[plugin_number].in_parameters[i].ctrl.type) {
#ifdef V4L2_CTRL_TYPE_STRING
                case V4L2_CTRL_TYPE_STRING:
                    //string gets set on VIDIOC_G_EXT_CTRLS
                    //add the maximum size to value
                    ext_ctrl.size = value;
                    DBG("STRING extended controls are currently broken\n");
                    //ext_ctrl.string = control->string; // FIXMEE
                    break;
#endif
                case V4L2_CTRL_TYPE_INTEGER64:
                    ext_ctrl.value64 = value;
                    break;
                default:
                    ext_ctrl.value = value;
                    break;
            }

            ext_ctrls.count = 1;
            ext_ctrls.controls = &ext_ctrl;
            ret = xioctl(vd->fd, VIDIOC_S_EXT_CTRLS, &ext_ctrls);
            if(ret) {
                LOG("control id: 0x%08x failed to set value (error %i)\n", ext_ctrl.id, ret);
                return -1;
            } else {
                DBG("control id: 0x%08x new value: %d\n", ext_ctrl.id, ext_ctrl.value);
            }
            return 0;
        }
    } else {
        LOG("Invalid V4L2_set_control request for the id: 0x%08x. Control cannot be found in the list\n", control_id);
        return -1;
    }
}

int v4l2ResetControl(struct vdIn *vd, int control)
{
    struct v4l2_control control_s;
    struct v4l2_queryctrl queryctrl;
    int val_def;
    int err;

    if(isv4l2Control(vd, control, &queryctrl) < 0)
        return -1;

    val_def = queryctrl.default_value;
    control_s.id = control;
    control_s.value = val_def;

    if((err = xioctl(vd->fd, VIDIOC_S_CTRL, &control_s)) < 0) {
        return -1;
    }

    return 0;
}

void control_readed(struct vdIn *vd, struct v4l2_queryctrl *ctrl, globals *pglobal, int id)
{
    struct v4l2_control c;
    memset(&c, 0, sizeof(struct v4l2_control));
    c.id = ctrl->id;

    if (pglobal->in[id].in_parameters == NULL) {
        pglobal->in[id].in_parameters = (control*)calloc(1, sizeof(control));
    } else {
        pglobal->in[id].in_parameters =
        (control*)realloc(pglobal->in[id].in_parameters,(pglobal->in[id].parametercount + 1) * sizeof(control));
    }

    if (pglobal->in[id].in_parameters == NULL) {
        DBG("Calloc failed\n");
        return;
    }

    memcpy(&pglobal->in[id].in_parameters[pglobal->in[id].parametercount].ctrl, ctrl, sizeof(struct v4l2_queryctrl));
    pglobal->in[id].in_parameters[pglobal->in[id].parametercount].group = IN_CMD_V4L2;
    pglobal->in[id].in_parameters[pglobal->in[id].parametercount].value = c.value;
    if(ctrl->type == V4L2_CTRL_TYPE_MENU) {
        pglobal->in[id].in_parameters[pglobal->in[id].parametercount].menuitems =
            (struct v4l2_querymenu*)malloc((ctrl->maximum + 1) * sizeof(struct v4l2_querymenu));
        int i;
        for(i = ctrl->minimum; i <= ctrl->maximum; i++) {
            struct v4l2_querymenu qm;
            memset(&qm, 0 , sizeof(struct v4l2_querymenu));
            qm.id = ctrl->id;
            qm.index = i;
            if(xioctl(vd->fd, VIDIOC_QUERYMENU, &qm) == 0) {
                memcpy(&pglobal->in[id].in_parameters[pglobal->in[id].parametercount].menuitems[i], &qm, sizeof(struct v4l2_querymenu));
                DBG("Menu item %d: %s\n", qm.index, qm.name);
            } else {
                DBG("Unable to get menu item for %s, index=%d\n", ctrl->name, qm.index);
            }
        }
    } else {
        pglobal->in[id].in_parameters[pglobal->in[id].parametercount].menuitems = NULL;
    }

    pglobal->in[id].in_parameters[pglobal->in[id].parametercount].value = 0;
    pglobal->in[id].in_parameters[pglobal->in[id].parametercount].class_id = (ctrl->id & 0xFFFF0000);
#ifndef V4L2_CTRL_FLAG_NEXT_CTRL
    pglobal->in[id].in_parameters[pglobal->in[id].parametercount].class_id = V4L2_CTRL_CLASS_USER;
#endif

    int ret = -1;
    if (pglobal->in[id].in_parameters[pglobal->in[id].parametercount].class_id == V4L2_CTRL_CLASS_USER) {
        DBG("V4L2 parameter found: %s value %d Class: USER \n", ctrl->name, c.value);
        ret = xioctl(vd->fd, VIDIOC_G_CTRL, &c);
        if(ret == 0) {
            pglobal->in[id].in_parameters[pglobal->in[id].parametercount].value = c.value;
        } else {
            DBG("Unable to get the value of %s retcode: %d  %s\n", ctrl->name, ret, strerror(errno));
        }
    } else {
        DBG("V4L2 parameter found: %s value %d Class: EXTENDED \n", ctrl->name, c.value);
        struct v4l2_ext_controls ext_ctrls = {0};
        struct v4l2_ext_control ext_ctrl = {0};
        ext_ctrl.id = ctrl->id;
#ifdef V4L2_CTRL_TYPE_STRING
        ext_ctrl.size = 0;
        if(ctrl.type == V4L2_CTRL_TYPE_STRING) {
            ext_ctrl.size = ctrl->maximum + 1;
            // FIXMEEEEext_ctrl.string = control->string;
        }
#endif
        ext_ctrls.count = 1;
        ext_ctrls.controls = &ext_ctrl;
        ret = xioctl(vd->fd, VIDIOC_G_EXT_CTRLS, &ext_ctrls);
        if(ret) {
            switch (ext_ctrl.id) {
                case V4L2_CID_PAN_RESET:
                    pglobal->in[id].in_parameters[pglobal->in[id].parametercount].value = 1;
                    DBG("Setting PAN reset value to 1\n");
                    break;
                case V4L2_CID_TILT_RESET:
                    pglobal->in[id].in_parameters[pglobal->in[id].parametercount].value = 1;
                    DBG("Setting the Tilt reset value to 2\n");
                    break;
                case V4L2_CID_PANTILT_RESET_LOGITECH:
                    pglobal->in[id].in_parameters[pglobal->in[id].parametercount].value = 3;
                    DBG("Setting the PAN/TILT reset value to 3\n");
                    break;
                default:
                    DBG("control id: 0x%08x failed to get value (error %i)\n", ext_ctrl.id, ret);
            }
        } else {
            switch(ctrl->type)
            {
#ifdef V4L2_CTRL_TYPE_STRING
                case V4L2_CTRL_TYPE_STRING:
                    //string gets set on VIDIOC_G_EXT_CTRLS
                    //add the maximum size to value
                    pglobal->in[id].in_parameters[pglobal->in[id].parametercount].value = ext_ctrl.size;
                    break;
#endif
                case V4L2_CTRL_TYPE_INTEGER64:
                    pglobal->in[id].in_parameters[pglobal->in[id].parametercount].value = ext_ctrl.value64;
                    break;
                default:
                    pglobal->in[id].in_parameters[pglobal->in[id].parametercount].value = ext_ctrl.value;
                    break;
            }
        }
    }

    pglobal->in[id].parametercount++;
}

/*  It should set the capture resolution
    Cheated from the openCV cap_libv4l.cpp the method is the following:
    Turn off the stream (video_disable)
    Unmap buffers
    Close the filedescriptor
    Initialize the camera again with the new resolution
*/
int setResolution(struct vdIn *vd, int width, int height)
{
    vd->streamingState = STREAMING_PAUSED;
    signal_pause_condition(vd);
    if (video_disable(vd, STREAMING_PAUSED) < 0) {
        IPRINT("Unable to disable streaming\n");
        return -1;
    }

    DBG("Unmap buffers\n");
    int i;
    for (i = 0; i < NB_BUFFER; i++) {
        munmap(vd->mem[i], vd->buf.length);
    }

    if (CLOSE_VIDEO(vd->fd) == 0) {
        DBG("Device closed successfully\n");
    }

    vd->width = width;
    vd->height = height;
    if (init_v4l2(vd) < 0) {
        return -1;
    }

    free_framebuffer(vd);
    if (init_framebuffer(vd) < 0) {
        IPRINT("Can\'t reallocate framebuffer\n");
        return -1;
    }

    DBG("Resolution changed to %dx%d , enabling the video...\n", width, height);
    if (video_enable(vd) < 0) {
        IPRINT("Can\'t RE-enable the video after setResolution(%dx%d)", width, height);
        return -1;
    }

    return 0;
}

/*
 *
 * Enumarates all V4L2 controls using various methods.
 * It places them to the
 *
 */

void enumerateControls(struct vdIn *vd, globals *pglobal, int id)
{
    // enumerating v4l2 controls
    struct v4l2_queryctrl ctrl;
    memset(&ctrl, 0, sizeof(struct v4l2_queryctrl));
    pglobal->in[id].parametercount = 0;
    pglobal->in[id].in_parameters = malloc(0 * sizeof(control));
    /* Enumerate the v4l2 controls
     Try the extended control API first */
#ifdef V4L2_CTRL_FLAG_NEXT_CTRL
    DBG("V4L2 API's V4L2_CTRL_FLAG_NEXT_CTRL is supported\n");
    ctrl.id = V4L2_CTRL_FLAG_NEXT_CTRL;
    if(0 == IOCTL_VIDEO(vd->fd, VIDIOC_QUERYCTRL, &ctrl)) {
        do {
            control_readed(vd, &ctrl, pglobal, id);
            ctrl.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
        } while(0 == IOCTL_VIDEO(vd->fd, VIDIOC_QUERYCTRL, &ctrl));
        // note: use simple ioctl or v4l2_ioctl instead of the xioctl
    } else
#endif
    {
        DBG("V4L2 API's V4L2_CTRL_FLAG_NEXT_CTRL is NOT supported\n");
        /* Fall back on the standard API */
        /* Check all the standard controls */
        int i;
        for(i = V4L2_CID_BASE; i < V4L2_CID_LASTP1; i++) {
            ctrl.id = i;
            if(IOCTL_VIDEO(vd->fd, VIDIOC_QUERYCTRL, &ctrl) == 0) {
                control_readed(vd, &ctrl, pglobal, id);
            }
        }

        /* Check any custom controls */
        for(i = V4L2_CID_PRIVATE_BASE; ; i++) {
            ctrl.id = i;
            if(IOCTL_VIDEO(vd->fd, VIDIOC_QUERYCTRL, &ctrl) == 0) {
                control_readed(vd, &ctrl, pglobal, id);
            } else {
                break;
            }
        }
    }

    memset(&pglobal->in[id].jpegcomp, 0, sizeof(struct v4l2_jpegcompression));
    if(xioctl(vd->fd, VIDIOC_G_JPEGCOMP, &pglobal->in[id].jpegcomp) != EINVAL) {
        DBG("JPEG compression details:\n");
        DBG("Quality: %d\n", pglobal->in[id].jpegcomp.quality);
        DBG("APPn: %d\n", pglobal->in[id].jpegcomp.APPn);
        DBG("APP length: %d\n", pglobal->in[id].jpegcomp.APP_len);
        DBG("APP data: %s\n", pglobal->in[id].jpegcomp.APP_data);
        DBG("COM length: %d\n", pglobal->in[id].jpegcomp.COM_len);
        DBG("COM data: %s\n", pglobal->in[id].jpegcomp.COM_data);
        struct v4l2_queryctrl ctrl_jpeg;
        ctrl_jpeg.id = 1;
        sprintf((char*)&ctrl_jpeg.name, "JPEG quality");
        ctrl_jpeg.minimum = 0;
        ctrl_jpeg.maximum = 100;
        ctrl_jpeg.step = 1;
        ctrl_jpeg.default_value = 50;
        ctrl_jpeg.flags = 0;
        ctrl_jpeg.type = V4L2_CTRL_TYPE_INTEGER;
        if (pglobal->in[id].in_parameters == NULL) {
            pglobal->in[id].in_parameters = (control*)calloc(1, sizeof(control));
        } else {
            pglobal->in[id].in_parameters = (control*)realloc(pglobal->in[id].in_parameters,(pglobal->in[id].parametercount + 1) * sizeof(control));
        }

        if (pglobal->in[id].in_parameters == NULL) {
            DBG("Calloc/realloc failed\n");
            return;
        }

        memcpy(&pglobal->in[id].in_parameters[pglobal->in[id].parametercount].ctrl, &ctrl_jpeg, sizeof(struct v4l2_queryctrl));
        pglobal->in[id].in_parameters[pglobal->in[id].parametercount].group = IN_CMD_JPEG_QUALITY;
        pglobal->in[id].in_parameters[pglobal->in[id].parametercount].value = pglobal->in[id].jpegcomp.quality;
        pglobal->in[id].parametercount++;
    } else {
        DBG("Modifying the setting of the JPEG compression is not supported\n");
        pglobal->in[id].jpegcomp.quality = -1;
    }
}
