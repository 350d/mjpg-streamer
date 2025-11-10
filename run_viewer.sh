#!/bin/bash
echo "Starting MJPG-Streamer with HTTP output..."
echo "Video will be available at: http://localhost:8080"
echo "Press Ctrl+C to stop"
./build/mjpg_streamer -i "./build/plugins/input_avf.dylib" -o "./build/plugins/output_http.dylib -p 8080 -w ./src/www"
