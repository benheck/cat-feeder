#!/bin/bash

# Quick stop script for cat feeder service
echo "Stopping cat feeder service..."
sudo systemctl stop cat-feeder

# Check if it stopped successfully
if sudo systemctl is-active cat-feeder >/dev/null 2>&1; then
    echo "Failed to stop service"
    exit 1
else
    echo "Cat feeder service stopped successfully"
fi