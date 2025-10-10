/* JPEG decompression functions */
int jpeg_decode_to_gray_scaled(unsigned char *jpeg_data, int jpeg_size, int scale_factor,
                               unsigned char **gray_data, int *width, int *height);

int jpeg_decompress_to_rgb(unsigned char *jpeg_data, int jpeg_size, 
                           unsigned char **rgb_data, int *width, int *height);

/* JPEG utility functions (new) */
int jpeg_validate_data(unsigned char *jpeg_data, int jpeg_size);

int jpeg_get_dimensions(unsigned char *jpeg_data, int jpeg_size, int *width, int *height);

/* JPEG data structure for RGB output */
typedef struct {
    int width;
    int height;
    unsigned char *buffer;
    int buffersize;
} jpeg_rgb_image;
