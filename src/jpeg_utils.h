/* Include TurboJPEG headers if available */
#ifdef HAVE_TURBOJPEG
    #include <turbojpeg.h>
#endif

/* JPEG library detection and initialization */
int detect_jpeg_library(void);
int jpeg_library_available(void);

/* JPEG decompression functions */
int jpeg_decode_to_gray_scaled(unsigned char *jpeg_data, int jpeg_size, int scale_factor,
                               unsigned char **gray_data, int *width, int *height, int known_width, int known_height);

int jpeg_decode_to_y_component(unsigned char *jpeg_data, int jpeg_size, int scale_factor,
                               unsigned char **y_data, int *width, int *height, int known_width, int known_height);

int decode_any_to_y_component(unsigned char *data, int data_size, int scale_factor,
                              unsigned char **y_data, int *width, int *height, int known_width, int known_height, int known_format);

int jpeg_decompress_to_rgb(unsigned char *jpeg_data, int jpeg_size, 
                           unsigned char **rgb_data, int *width, int *height, int known_width, int known_height);

/* TurboJPEG handle caching functions */
void cleanup_turbojpeg_handles(void);

/* Default JPEG quantization tables (quality ~75) */
extern const unsigned char jpeg_default_qt_luma[64];
extern const unsigned char jpeg_default_qt_chroma[64];

/* TurboJPEG helpers */
/* Returns 0 on success; fills width, height, subsamp (TJSAMP_*). */
int turbojpeg_header_info(const unsigned char *jpeg_data, int jpeg_size,
                          int *width, int *height, int *subsamp);

/* Recompress JPEG to baseline with default Huffman tables (RFC 2435 compatible) */
int recompress_jpeg_to_baseline_with_default_dht(const unsigned char *input_jpeg, int input_size,
                                                 unsigned char **output_jpeg, unsigned long *output_size,
                                                 int quality, int target_subsamp);


/* JPEG utility functions */


/* JPEG compression functions */
int compress_rgb_to_jpeg(unsigned char *rgb_data, int width, int height, int quality,
                        unsigned char **jpeg_data, unsigned long *jpeg_size);

/* JPEG data structure for RGB output */
typedef struct {
    int width;
    int height;
    unsigned char *buffer;
    int buffersize;
} jpeg_rgb_image;
