#!/bin/bash

# Quick start script for cat feeder service
echo "Starting cat feeder service..."
sudo systemctl start cat-feeder

# Check if it started successfully
sleep 2
if sudo systemctl is-active cat-feeder >/dev/null 2>&1; then
    echo "Cat feeder service started successfully"
else
    echo "Failed to start service"
    exit 1
fi