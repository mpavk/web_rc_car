#!/bin/bash
# Цей скрипт запускає камеру і транслює потік на локальний порт 5001

LIBCAM_CMD="libcamera-vid -t 0 --inline -n -o - --width 640 --height 480 --framerate 25 --codec h264 --profile baseline --intra 25 --bitrate 1500000"

GST_CMD="gst-launch-1.0 -v fdsrc ! queue ! h264parse ! rtph264pay ! udpsink host=127.0.0.1 port=5001"

echo "Starting camera stream to localhost:5001..."
$LIBCAM_CMD | $GST_CMD
