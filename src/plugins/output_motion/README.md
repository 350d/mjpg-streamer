# Motion Detection Output Plugin

This plugin provides motion detection capabilities for mjpg-streamer. It analyzes incoming video frames to detect motion and can save motion frames and/or send webhook notifications.

## Features

- **Motion Detection**: Analyzes frame differences to detect motion
- **Configurable Parameters**: Adjustable sensitivity, thresholds, and intervals
- **Frame Saving**: Optional saving of motion frames with timestamps
- **Webhook Notifications**: HTTP GET/POST notifications when motion is detected (HTTP only)
- **Cooldown Period**: Prevents spam by limiting motion events
- **Optimized Processing**: Scales down frames for faster processing

## Parameters

| Parameter | Short | Description | Default |
|-----------|-------|-------------|---------|
| `--downscale` | `-d` | Scale down factor (1-16) | 4 |
| `--motion` | `-l` | Motion detection threshold in % (1-100) | 5 |
| `--overload` | `-o` | Overload threshold in % (1-100) - ignores lighting changes | 50 |
| `--skipframe` | `-s` | Check every N frames | 1 |
| `--folder` | `-f` | Folder to save motion frames | - |
| `--webhook` | `-w` | Webhook URL for motion events | - |
| `--post` | `-p` | Use POST instead of GET for webhook | GET |
| `--cooldown` | `-c` | Cooldown between events (seconds) | 5 |
| `--input` | `-n` | Input plugin number | 0 |
| `--jpeg-size-check` | `-z` | JPEG size change threshold in 0.1% units (0-1000) | 1 (0.1%) |

## Usage Examples

### Basic Motion Detection
```bash
mjpg_streamer -i "input_uvc.so -d /dev/video0" -o "output_motion.so"
```

### Save Motion Frames
```bash
mjpg_streamer -i "input_uvc.so -d /dev/video0" \
  -o "output_motion.so -f /tmp/motion_frames"
```

### Webhook Notifications (GET)
```bash
mjpg_streamer -i "input_uvc.so -d /dev/video0" \
  -o "output_motion.so -w http://localhost:8080/motion"
```

### Webhook Notifications (POST)
```bash
mjpg_streamer -i "input_uvc.so -d /dev/video0" \
  -o "output_motion.so -w http://localhost:8080/motion -p"
```

### High Sensitivity with Custom Settings
```bash
mjpg_streamer -i "input_uvc.so -d /dev/video0" \
  -o "output_motion.so -d 2 -l 3 -o 30 -c 1"
```

### Performance Optimization with Size Threshold
```bash
mjpg_streamer -i "input_uvc.so -d /dev/video0" \
  -o "output_motion.so -z 1 -d 4 -l 5"  # 0.1% threshold (default)
```

### Higher Sensitivity (More Processing)
```bash
mjpg_streamer -i "input_uvc.so -d /dev/video0" \
  -o "output_motion.so -z 0 -d 4 -l 5"  # 0.0% threshold (process all frames)
```

### Lower Sensitivity (Less Processing)
```bash
mjpg_streamer -i "input_uvc.so -d /dev/video0" \
  -o "output_motion.so -z 5 -d 4 -l 5"  # 0.5% threshold
```

### Combined Usage
```bash
mjpg_streamer -i "input_uvc.so -d /dev/video0" \
  -o "output_motion.so -f /tmp/motion -w http://localhost:8080/motion -p -d 4 -l 5 -o 50 -z 3"
```

## Key Features

### Overload Protection
The `--overload` parameter prevents false alarms from lighting changes:
- **Default: 50%** - ignores motion events above 50% change
- **Useful for**: Lights turning on/off, sudden brightness changes
- **Example**: `-o 80` ignores motion above 80% change

### Performance Optimization
The `--jpeg-size-check` parameter optimizes performance:
- **Default: 1 (0.1%)** - skips analysis for minimal changes
- **Higher values**: Less processing, may miss subtle motion
- **Lower values**: More processing, catches all motion

### Frame Skipping
The `--skipframe` parameter improves performance:
- **Default: 1** - processes every frame
- **Higher values**: Process every N frames (e.g., `-s 3` = every 3rd frame)
- **Trade-off**: Better performance vs. motion detection accuracy

### Cooldown Period
The `--cooldown` parameter prevents notification spam:
- **Default: 5 seconds** - minimum time between motion events
- **Useful for**: Preventing multiple notifications for the same motion
- **Example**: `-c 10` = minimum 10 seconds between notifications

## How It Works

1. **JPEG Size Check**: First, the plugin compares the current JPEG frame size with the previous frame
   - If the size change is below the threshold (`-z` in 0.1% units), motion analysis is skipped (performance optimization)
   - Default threshold is 1 (0.1%) - extremely sensitive to any changes
   - This prevents unnecessary processing only when the scene is completely static
2. **Frame Processing**: Each frame is converted to grayscale and scaled down by the specified factor
3. **Motion Calculation**: The plugin compares consecutive frames pixel by pixel
4. **Overload Check**: If motion level exceeds the overload threshold (`-o`), the event is ignored (prevents false alarms from lighting changes)
5. **Motion Detection**: If the difference exceeds the motion threshold (`-l`), motion is detected
6. **Cooldown Check**: If enough time hasn't passed since the last motion event (`-c`), the event is ignored
7. **Actions**: On motion detection:
   - Optionally saves the frame with timestamp and motion level
   - Optionally sends a webhook notification with motion data

## Webhook Format

When motion is detected, the plugin sends a request to the specified webhook URL:

### GET Request (default)
```
GET http://localhost:8080/motion?timestamp=2024-01-15%2014:30:25&motion_level=75.0&threshold=5.0
```

### POST Request (with -p flag)
```
POST http://localhost:8080/motion
Content-Type: application/x-www-form-urlencoded

timestamp=2024-01-15 14:30:25&motion_level=75.0&threshold=5.0
```

## File Naming

Saved motion frames use the following naming convention:
```
YYYYMMDD_HHMMSS_motion_LEVEL.jpg
```

Example: `20240115_143025_motion_75.0%.jpg`

## Configuration Examples

### Indoor Security Camera
```bash
# Sensitive detection for indoor use
mjpg_streamer -i "input_uvc.so -d /dev/video0" \
  -o "output_motion.so -d 2 -l 2 -o 30 -c 3 -f /var/motion"
```

### Outdoor Security Camera
```bash
# Conservative detection for outdoor use with lighting changes
mjpg_streamer -i "input_uvc.so -d /dev/video0" \
  -o "output_motion.so -d 4 -l 10 -o 80 -c 10 -f /var/motion"
```

### High Performance Setup
```bash
# Skip frames for better performance
mjpg_streamer -i "input_uvc.so -d /dev/video0" \
  -o "output_motion.so -d 4 -l 5 -s 3 -z 5"
```

### Complete Security Setup
```bash
# All features enabled
mjpg_streamer -i "input_uvc.so -d /dev/video0 -r 1280x720 -f 15" \
  -o "output_http.so -p 8080" \
  -o "output_motion.so -d 4 -l 5 -o 50 -s 1 -c 5 -z 1 -f /var/motion -w http://alerts.example.com/motion -p"
```

## Performance Notes

- **Downscale Factor**: Higher values (e.g., 8) reduce processing load but may miss small movements
- **Frame Skipping**: Skipping frames (e.g., `-s 3`) reduces CPU usage
- **Motion Threshold**: Lower values (e.g., 2%) increase sensitivity but may cause false positives
- **Overload Threshold**: Higher values (e.g., 80%) prevent false alarms from lighting changes
- **JPEG Size Check**: Higher values (e.g., 5) reduce processing but may miss subtle motion
- **Cooldown**: Prevents rapid-fire notifications for continuous motion

## Dependencies

- **libcurl**: For webhook notifications
- **pthread**: For multi-threading support

## Installation

The plugin is automatically built when compiling mjpg-streamer with CMake. Ensure libcurl development headers are installed:

```bash
# Ubuntu/Debian
sudo apt-get install libcurl4-openssl-dev

# CentOS/RHEL
sudo yum install libcurl-devel

# macOS
brew install curl
```

## Troubleshooting

### High CPU Usage
- Increase scale factor (`-s 8`)
- Increase check interval (`-i 5`)
- Increase noise threshold (`-t 7`)

### False Positives
- Increase brightness threshold (`-l 10`)
- Increase noise threshold (`-t 7`)
- Increase cooldown period (`-c 5`)

### Missing Motion
- Decrease brightness threshold (`-l 3`)
- Decrease noise threshold (`-t 3`)
- Decrease scale factor (`-s 2`)

### Webhook Issues
- Check URL accessibility
- Verify network connectivity
- Check webhook server logs
