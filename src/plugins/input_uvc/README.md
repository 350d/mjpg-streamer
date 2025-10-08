# input_uvc Plugin

The input_uvc plugin captures video from USB Video Class (UVC) compatible webcams using Video4Linux2.

## Features

- **UVC compatibility**: Works with most USB webcams
- **Multiple formats**: Supports MJPEG, YUV, and other formats
- **Configurable settings**: Resolution, frame rate, quality
- **Memory optimized**: Eliminated double buffering for MJPEG

## Parameters

| Parameter | Short | Description | Default |
|-----------|-------|-------------|---------|
| `--device` | `-d` | Video device path | /dev/video0 |
| `--resolution` | `-r` | Video resolution (e.g., 640x480) | Auto-detect |
| `--fps` | `-f` | Frames per second | 5 |
| `--format` | `-y` | Video format (mjpeg, yuv) | mjpeg |
| `--quality` | `-q` | JPEG quality (1-100) | 80 |
| `--no_dynctrl` | `-n` | Disable dynamic controls | false |

## Usage Examples

### Basic webcam capture
```bash
mjpg_streamer -i "input_uvc.so -d /dev/video0"
```

### High resolution with custom settings
```bash
mjpg_streamer -i "input_uvc.so -d /dev/video0 -r 1280x720 -f 30 -q 90"
```

### MJPEG format with quality control
```bash
mjpg_streamer -i "input_uvc.so -d /dev/video0 -y mjpeg -q 85"
```

## Critical Bug Fixes

### ✅ Memory Optimization (Fixed)
- **Issue**: Double buffering for MJPEG frames caused unnecessary memory usage
- **Fix**: For MJPEG format, `tmpbuffer` now points directly to `framebuffer`
- **Impact**: Reduced memory usage by ~50% for MJPEG streams
- **Files**: `v4l2uvc.c` in `init_framebuffer()` and `free_framebuffer()`

### ✅ Cross-Compilation Support (Fixed)
- **Issue**: JPEG headers not found during ARM cross-compilation
- **Fix**: Added proper JPEG include paths for cross-compilation
- **Impact**: Enables successful builds for Raspberry Pi
- **Files**: `CMakeLists.txt` lines 14-25, 46-48

### ✅ Memory Leak Prevention (Fixed)
- **Issue**: Potential memory leaks in buffer management
- **Fix**: Proper cleanup of buffer pointers in `free_framebuffer()`
- **Impact**: Prevents memory leaks during long-running sessions
- **Files**: `v4l2uvc.c` in `free_framebuffer()` function

## Supported Formats

### MJPEG (Motion JPEG)
- **Advantages**: Hardware accelerated, low CPU usage
- **Memory**: Optimized single buffer usage
- **Quality**: Configurable via `-q` parameter

### YUV420
- **Advantages**: Uncompressed, high quality
- **Memory**: Higher memory usage
- **CPU**: Requires more processing power

## Device Detection

### List available devices
```bash
ls /dev/video*
```

### Check device capabilities
```bash
v4l2-ctl --list-devices
v4l2-ctl --device=/dev/video0 --list-formats-ext
```

### Test device
```bash
mjpg_streamer -i "input_uvc.so -d /dev/video0 --help"
```

## Performance Notes

- **MJPEG recommended**: Best performance on Raspberry Pi
- **Memory efficient**: Single buffer for MJPEG format
- **Hardware acceleration**: Uses camera's built-in JPEG encoder
- **Optimized for Pi**: Reduced memory footprint for resource-constrained devices

## Troubleshooting

### Device not found
```bash
# Check if device exists
ls -la /dev/video0

# Check permissions
sudo chmod 666 /dev/video0
```

### Permission denied
```bash
# Add user to video group
sudo usermod -a -G video $USER
# Log out and back in
```

### No supported formats
```bash
# Check device capabilities
v4l2-ctl --device=/dev/video0 --list-formats-ext
```

### Memory issues
- Use MJPEG format (`-y mjpeg`)
- Reduce resolution (`-r 640x480`)
- Lower frame rate (`-f 15`)