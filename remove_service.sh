#!/bin/bash

# Cat Feeder Service Removal Script
# This script removes the cat feeder systemd service

SERVICE_NAME="cat-feeder"
INSTALL_PATH="/etc/systemd/system/${SERVICE_NAME}.service"

echo "Removing Cat Feeder Service..."

# Check if running as root/sudo
if [ "$EUID" -ne 0 ]; then
    echo "This script must be run with sudo privileges"
    echo "Usage: sudo ./remove_service.sh"
    exit 1
fi

# Stop the service
echo "Stopping service..."
systemctl stop $SERVICE_NAME 2>/dev/null || true

# Disable service from auto-start
echo "Disabling auto-start..."
systemctl disable $SERVICE_NAME 2>/dev/null || true

# Remove service file
if [ -f "$INSTALL_PATH" ]; then
    echo "Removing service file..."
    rm $INSTALL_PATH
fi

# Reload systemd
echo "Reloading systemd daemon..."
systemctl daemon-reload

echo ""
echo "Service removal complete!"
echo "The cat feeder will no longer start automatically on boot."
echo ""
echo "You can still run it manually with:"
echo "  cd /home/ben/opener_code/build/bin && ./opnr"