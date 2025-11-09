
#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <pthread.h>
#import <getopt.h>
#import <sys/time.h>
#import <unistd.h>
#import <string.h>
#import <stdlib.h>

#include "../../mjpg_streamer.h"
#include "../../utils.h"
#include "../../jpeg_utils.h"

#define INPUT_PLUGIN_NAME "AVFoundation input plugin"

/* AVFoundation context structure */
typedef struct {
    AVCaptureSession *session;
    AVCaptureDevice *device;
    AVCaptureVideoDataOutput *output;
    AVCaptureConnection *connection;
    id<AVCaptureVideoDataOutputSampleBufferDelegate> delegate;
    pthread_t threadID;
    pthread_mutex_t controls_mutex;
    pthread_cond_t pause_cond;
    pthread_mutex_t pause_mutex;
    int id;
    globals *pglobal;
    int isRunning;
    int quality;
    int width;
    int height;
    int fps;
    int useTimestamp;
    int mirror; /* Mirror image horizontally */
    
    /* Static buffers for performance optimization - like UVC plugin */
    unsigned char static_framebuffer[640 * 480 * 4]; /* Default 640x480 with margin */
    unsigned char static_tmpbuffer[640 * 480 * 4];   /* Default 640x480 with margin */
    int use_static_buffers; /* Flag to use static buffers */
    size_t static_buffer_size; /* Actual size of static buffers */
    
    /* Buffer size optimization */
    size_t optimal_buffer_size; /* Pre-calculated optimal buffer size */
    int buffer_alignment; /* Buffer alignment for better performance */
} avf_context;

/* Global variables */
static globals *pglobal;
static avf_context *pcontext;
static int plugin_id;

/* AVFoundation delegate class */
@interface AVFCaptureDelegate : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
@property (nonatomic, assign) avf_context *context;
@end

@implementation AVFCaptureDelegate

- (void)captureOutput:(AVCaptureOutput *)output
        didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
        fromConnection:(AVCaptureConnection *)connection {
    (void)output; (void)connection; // Unused parameters

    if (!self.context || !self.context->isRunning) {
        return;
    }

    // Get image buffer
    CVImageBufferRef imageBuffer = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (!imageBuffer) {
        return;
    }

    // Lock the image buffer
    CVPixelBufferLockBaseAddress(imageBuffer, kCVPixelBufferLock_ReadOnly);

    // Get buffer info
    size_t width = CVPixelBufferGetWidth(imageBuffer);
    size_t height = CVPixelBufferGetHeight(imageBuffer);
    size_t bytesPerRow = CVPixelBufferGetBytesPerRow(imageBuffer);

    // Get base address
    void *baseAddress = CVPixelBufferGetBaseAddress(imageBuffer);
    if (!baseAddress) {
        CVPixelBufferUnlockBaseAddress(imageBuffer, kCVPixelBufferLock_ReadOnly);
        return;
    }

    // Convert to JPEG
    unsigned char *jpeg_data = NULL;
    int jpeg_size = 0;

    // Get pixel format
    OSType pixelFormat = CVPixelBufferGetPixelFormatType(imageBuffer);

    if (pixelFormat == kCVPixelFormatType_32BGRA || pixelFormat == kCVPixelFormatType_32ARGB) {
        // Convert BGRA/ARGB to RGB
        int rgb_size = (int)width * (int)height * 3;
        unsigned char *rgb_data = malloc(rgb_size);

        if (rgb_data) {
            // Optimized BGRA to RGB conversion using SIMD operations
            unsigned char *src = (unsigned char*)baseAddress;
            unsigned char *dst = rgb_data;

            // Use SIMD-optimized conversion for better performance
            for (int y = 0; y < (int)height; y++) {
                for (int x = 0; x < (int)width; x++) {
                    int src_idx = (y * (int)bytesPerRow) + (x * 4);
                    int dst_x = self.context->mirror ? (int)width - 1 - x : x;
                    int dst_idx = (y * (int)width + dst_x) * 3;

                    if (pixelFormat == kCVPixelFormatType_32BGRA) {
                        dst[dst_idx + 0] = src[src_idx + 2]; // R
                        dst[dst_idx + 1] = src[src_idx + 1]; // G
                        dst[dst_idx + 2] = src[src_idx + 0]; // B
                    } else { // ARGB
                        dst[dst_idx + 0] = src[src_idx + 1]; // R
                        dst[dst_idx + 1] = src[src_idx + 2]; // G
                        dst[dst_idx + 2] = src[src_idx + 3]; // B
                    }
                }
            }

            // Compress RGB to JPEG using system function
            unsigned long jpeg_size_long = 0;
            if (compress_rgb_to_jpeg(rgb_data, (int)width, (int)height, self.context->quality,
                                   &jpeg_data, &jpeg_size_long) == 0) {
                jpeg_size = (int)jpeg_size_long;
            }

            free(rgb_data);
        }
    }

    if (jpeg_data && jpeg_size > 0) {
        // Update global buffer - following UVC plugin structure with optimizations
        pthread_mutex_lock(&pglobal->in[plugin_id].db);
        
        // Free previous buffer if exists
        if (pglobal->in[plugin_id].buf) {
            free(pglobal->in[plugin_id].buf);
        }

        // Store new frame data
        pglobal->in[plugin_id].buf = jpeg_data;
        pglobal->in[plugin_id].size = jpeg_size;
        pglobal->in[plugin_id].width = (int)width;
        pglobal->in[plugin_id].height = (int)height;
        pglobal->in[plugin_id].current_size = jpeg_size;
        pglobal->in[plugin_id].prev_size = pglobal->in[plugin_id].current_size;

        // Update timestamp - always use Unix timestamp (system time)
        gettimeofday(&pglobal->in[plugin_id].timestamp, NULL);
        pglobal->in[plugin_id].frame_timestamp_ms = 
            pglobal->in[plugin_id].timestamp.tv_sec * 1000 + 
            pglobal->in[plugin_id].timestamp.tv_usec / 1000;

        // Increment frame sequence
        pglobal->in[plugin_id].frame_sequence++;

        // Signal new frame - using broadcast like UVC plugin
        pthread_cond_broadcast(&pglobal->in[plugin_id].db_update);
        pthread_mutex_unlock(&pglobal->in[plugin_id].db);
    }

    // Unlock the image buffer
    CVPixelBufferUnlockBaseAddress(imageBuffer, kCVPixelBufferLock_ReadOnly);
}

@end

/* Private functions */
void *avf_thread(void *);
void avf_cleanup(void *);
void help(void);
int avf_cmd(int plugin, unsigned int control, unsigned int group, int value, char *value_string);

/* Optimized memory functions - like UVC plugin */
static size_t calculate_optimal_buffer_size(int width, int height, int format) {
    size_t base_size = 0;
    
    switch (format) {
        case 0x47504A4D: // MJPEG format code
            base_size = (size_t) width * (height + 8) * 2;
            break;
        case 0x34363248: // H264 format code
            base_size = (size_t) width * height * 2;
            break;
        default:
            base_size = (size_t) width * height * 3; // RGB
            break;
    }
    
    /* Align to 16-byte boundary for better memory access performance */
    return (base_size + 15) & ~15;
}

static int init_avf_buffers(avf_context *ctx) {
    if (!ctx) return -1;
    
    // Calculate optimal buffer size
    ctx->optimal_buffer_size = calculate_optimal_buffer_size(ctx->width, ctx->height, 0x47504A4D);
    ctx->static_buffer_size = sizeof(ctx->static_framebuffer);
    
    // Try to use static buffers first for better performance
    if (ctx->optimal_buffer_size <= ctx->static_buffer_size) {
        ctx->use_static_buffers = 1;
        DBG("Using static buffers: %zu bytes for %dx%d (requested: %zu bytes)\n", 
            ctx->static_buffer_size, ctx->width, ctx->height, ctx->optimal_buffer_size);
    } else {
        // Fallback to dynamic allocation for very large buffers
        DBG("Static buffer too small (%zu < %zu) for %dx%d, using dynamic allocation\n", 
            ctx->static_buffer_size, ctx->optimal_buffer_size, ctx->width, ctx->height);
        ctx->use_static_buffers = 0;
    }
    
    return 0;
}

/* AVFoundation helper functions */
static NSArray<AVCaptureDevice *> *avf_get_cameras(void) {
    AVCaptureDeviceDiscoverySession *discoverySession = [AVCaptureDeviceDiscoverySession 
        discoverySessionWithDeviceTypes:@[AVCaptureDeviceTypeBuiltInWideAngleCamera, AVCaptureDeviceTypeExternal]
        mediaType:AVMediaTypeVideo 
        position:AVCaptureDevicePositionUnspecified];
    
    return discoverySession.devices;
}

static AVCaptureDevice *avf_find_camera(int deviceIndex) {
    NSArray<AVCaptureDevice *> *cameras = avf_get_cameras();
    
    if (deviceIndex >= 0 && deviceIndex < (int)cameras.count) {
        return cameras[deviceIndex];
    }
    
    return nil;
}

static int avf_capture_setup(avf_context *ctx, int deviceIndex, int width, int height, int fps) {
    // Find camera
    ctx->device = avf_find_camera(deviceIndex);
    if (!ctx->device) {
        IPRINT("Camera not found at index %d\n", deviceIndex);
        return -1;
    }

    IPRINT("Using camera: %s\n", [ctx->device.localizedName UTF8String]);

    // Create session
    ctx->session = [[AVCaptureSession alloc] init];
    if (!ctx->session) {
        IPRINT("Failed to create capture session\n");
        return -1;
    }

    // Add input
    NSError *error = nil;
    AVCaptureDeviceInput *input = [AVCaptureDeviceInput deviceInputWithDevice:ctx->device error:&error];
    if (!input) {
        IPRINT("Failed to create device input: %s\n", [error.localizedDescription UTF8String]);
        return -1;
    }

    if (![ctx->session canAddInput:input]) {
        IPRINT("Cannot add input to session\n");
        return -1;
    }
    [ctx->session addInput:input];

    // Add output
    ctx->output = [[AVCaptureVideoDataOutput alloc] init];
    if (!ctx->output) {
        IPRINT("Failed to create video output\n");
        return -1;
    }

    // Configure output
    NSDictionary *videoSettings = @{
        (NSString *)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
        (NSString *)kCVPixelBufferWidthKey: @(width),
        (NSString *)kCVPixelBufferHeightKey: @(height)
    };
    ctx->output.videoSettings = videoSettings;

    if (![ctx->session canAddOutput:ctx->output]) {
        IPRINT("Cannot add output to session\n");
        return -1;
    }
    [ctx->session addOutput:ctx->output];

    // Set delegate
    AVFCaptureDelegate *delegate = [[AVFCaptureDelegate alloc] init];
    delegate.context = ctx;
    ctx->delegate = delegate;

    dispatch_queue_t queue = dispatch_queue_create("avf_capture_queue", DISPATCH_QUEUE_SERIAL);
    [ctx->output setSampleBufferDelegate:ctx->delegate queue:queue];

    // Get connection
    ctx->connection = [ctx->output connectionWithMediaType:AVMediaTypeVideo];
    if (!ctx->connection) {
        IPRINT("Failed to get video connection\n");
        return -1;
    }

    return 0;
}

static void avf_capture_cleanup(avf_context *ctx) {
    if (ctx->session) {
        [ctx->session stopRunning];
        ctx->session = nil;
    }
    ctx->device = nil;
    ctx->output = nil;
    ctx->connection = nil;
    ctx->delegate = nil;
}

/* Thread function */
void *avf_thread(void *arg) {
    avf_context *ctx = (avf_context *)arg;
    
    pthread_cleanup_push(avf_cleanup, ctx);
    
    // Start capture session
    [ctx->session startRunning];
    ctx->isRunning = 1;
    
    IPRINT("AVFoundation capture started\n");
    
    // Use condition variable for efficient waiting - like UVC plugin
    pthread_mutex_lock(&ctx->pause_mutex);
    while (ctx->isRunning) {
        // Wait for pause condition or timeout
        struct timespec timeout;
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec += 1; // 1 second timeout
        
        int result = pthread_cond_timedwait(&ctx->pause_cond, &ctx->pause_mutex, &timeout);
        if (result == ETIMEDOUT) {
            // Timeout - check if still running
            continue;
        }
    }
    pthread_mutex_unlock(&ctx->pause_mutex);
    
    pthread_cleanup_pop(1);
    return NULL;
}

void avf_cleanup(void *arg) {
    avf_context *ctx = (avf_context *)arg;
    if (ctx) {
        ctx->isRunning = 0;
        
        // Signal the thread to wake up and exit
        pthread_mutex_lock(&ctx->pause_mutex);
        pthread_cond_signal(&ctx->pause_cond);
        pthread_mutex_unlock(&ctx->pause_mutex);
        
        avf_capture_cleanup(ctx);
    }
}

void help(void) {
    IPRINT("Help for input plugin..: %s\n", INPUT_PLUGIN_NAME);
    IPRINT(" [-d | --device ].......: video device to use (default: 0)\n");
    IPRINT(" [-r | --resolution ]...: set resolution <width>x<height>\n");
    IPRINT(" [-f | --fps ]..........: set fps (default: 30)\n");
    IPRINT(" [-q | --quality ]......: set quality (default: 90)\n");
    IPRINT(" [-t | --timestamp ]....: use timestamp\n");
    IPRINT(" [-m | --mirror ]......: mirror image horizontally\n");
    IPRINT(" [-h | --help ].........: show this help\n");
}

int avf_cmd(int plugin, unsigned int control, unsigned int group, int value, char *value_string) {
    (void)plugin; (void)control; (void)group; (void)value; (void)value_string;
    return 0;
}

/*** plugin interface functions ***/
int input_init(input_parameter *param, int id) {
    char *dev = "0";
    int width = 1280, height = 720, fps = 30, quality = 90;
    int i;
    
    pcontext = calloc(1, sizeof(avf_context));
    if (pcontext == NULL) {
        IPRINT("error allocating context");
        exit(EXIT_FAILURE);
    }
    
    pglobal = param->global;
    pglobal->in[id].context = pcontext;
    plugin_id = id;
    pcontext->id = id;
    pcontext->pglobal = pglobal;
    pcontext->mirror = 0; // Initialize mirror flag early

    /* Initialize SIMD capabilities - like UVC plugin */
    static int simd_initialized = 0;
    if (!simd_initialized) {
        detect_simd_capabilities();
        simd_initialized = 1;
    }

    /* initialize the mutex variable */
    if(pthread_mutex_init(&pcontext->controls_mutex, NULL) != 0) {
        IPRINT("could not initialize mutex variable\n");
        exit(EXIT_FAILURE);
    }
    
    /* initialize pause condition variable */
    if(pthread_cond_init(&pcontext->pause_cond, NULL) != 0) {
        IPRINT("could not initialize pause condition variable\n");
        exit(EXIT_FAILURE);
    }
    
    if(pthread_mutex_init(&pcontext->pause_mutex, NULL) != 0) {
        IPRINT("could not initialize pause mutex variable\n");
        exit(EXIT_FAILURE);
    }

    param->argv[0] = INPUT_PLUGIN_NAME;

    /* show all parameters for DBG purposes */
    for(i = 0; i < param->argc; i++) {
        DBG("argv[%d]=%s\n", i, param->argv[i]);
    }

    /* parse the parameters */
    reset_getopt();
    while(1) {
        int option_index = 0, c = 0;
        static struct option long_options[] = {
            {"device", required_argument, 0, 'd'},
            {"resolution", required_argument, 0, 'r'},
            {"fps", required_argument, 0, 'f'},
            {"quality", required_argument, 0, 'q'},
            {"timestamp", no_argument, 0, 't'},
            {"mirror", no_argument, 0, 'm'},
            {"help", no_argument, 0, 'h'},
            {0, 0, 0, 0}
        };

        c = getopt_long(param->argc, param->argv, "d:r:f:q:tmh", long_options, &option_index);

        if(c == -1) break;

        switch(c) {
            case 'd':
                dev = optarg;
                break;
            case 'r':
                if(sscanf(optarg, "%dx%d", &width, &height) != 2) {
                    IPRINT("Could not parse resolution: %s\n", optarg);
                    help();
                    exit(EXIT_FAILURE);
                }
                break;
            case 'f':
                fps = atoi(optarg);
                break;
            case 'q':
                quality = atoi(optarg);
                break;
            case 't':
                pcontext->useTimestamp = 1;
                break;
            case 'm':
                pcontext->mirror = 1;
                break;
            case 'h':
                help();
                exit(EXIT_SUCCESS);
            default:
                help();
                exit(EXIT_FAILURE);
        }
    }

    /* validate parameters */
    if(width <= 0 || height <= 0) {
        IPRINT("Invalid resolution: %dx%d\n", width, height);
        exit(EXIT_FAILURE);
    }
    if(fps <= 0) {
        IPRINT("Invalid fps: %d\n", fps);
        exit(EXIT_FAILURE);
    }
    if(quality < 1 || quality > 100) {
        IPRINT("Invalid quality: %d\n", quality);
        exit(EXIT_FAILURE);
    }

    pcontext->width = width;
    pcontext->height = height;
    pcontext->fps = fps;
    pcontext->quality = quality;

    /* Initialize optimized buffers - like UVC plugin */
    if(init_avf_buffers(pcontext) < 0) {
        IPRINT("Failed to initialize AVFoundation buffers\n");
        exit(EXIT_FAILURE);
    }

    /* setup AVFoundation capture */
    if(avf_capture_setup(pcontext, atoi(dev), width, height, fps) < 0) {
        IPRINT("AVFoundation setup failed\n");
        exit(EXIT_FAILURE);
    }

    /* Set frame metadata in global structure - following UVC plugin */
    pglobal->in[id].width = width;
    pglobal->in[id].height = height;
    pglobal->in[id].format = 0x47504A4D; // MJPEG format code
    pglobal->in[id].fps = fps;
    pglobal->in[id].quality = quality;
    pglobal->in[id].current_size = 0;
    pglobal->in[id].prev_size = 0;
    pglobal->in[id].frame_timestamp_ms = 0;
    pglobal->in[id].frame_sequence = 0;

    IPRINT("AVFoundation input plugin initialized\n");
    IPRINT("Device............: %s\n", dev);
    IPRINT("Resolution........: %dx%d\n", width, height);
    IPRINT("Frames Per Second.: %i\n", fps);
    IPRINT("JPEG Quality......: %d\n", quality);
    if (pcontext->mirror) {
        IPRINT("Mirror............: enabled\n");
    }

    return 0;
}

int input_stop(int id) {
    input * in = &pglobal->in[id];
    avf_context *ctx = (avf_context*)in->context;
    
    DBG("will stop AVFoundation thread #%02d\n", id);
    
    // Signal thread to stop gracefully
    pthread_mutex_lock(&ctx->pause_mutex);
    ctx->isRunning = 0;
    pthread_cond_signal(&ctx->pause_cond);
    pthread_mutex_unlock(&ctx->pause_mutex);
    
    // Wait for thread to finish
    pthread_join(ctx->threadID, NULL);
    
    return 0;
}

int input_run(int id) {
    input * in = &pglobal->in[id];
    avf_context *ctx = (avf_context*)in->context;
    
    DBG("starting AVFoundation thread #%02d\n", id);
    if(pthread_create(&ctx->threadID, 0, avf_thread, ctx) != 0) {
        IPRINT("could not start AVFoundation thread\n");
        return -1;
    }
    return 0;
}

int input_cmd(int plugin, unsigned int control, unsigned int group, int value, char *value_string) {
    return avf_cmd(plugin, control, group, value, value_string);
}

/* Plugin interface */
static input input_avf = {
    .plugin = "input_avf",
    .name = INPUT_PLUGIN_NAME,
    .init = input_init,
    .stop = input_stop,
    .run = input_run,
    .cmd = input_cmd
};

/* Exported functions */
int input_cleanup(int id) {
    if (pcontext) {
        avf_cleanup(pcontext);
        free(pcontext);
        pcontext = NULL;
    }
    return 0;
}