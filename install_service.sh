#!/bin/bash

# Cat Feeder Service Installation Script
# This script installs the cat feeder as a systemd service to run on boot

SERVICE_NAME="cat-feeder"
SERVICE_FILE="cat-feeder.service"
INSTALL_PATH="/etc/systemd/system/${SERVICE_NAME}.service"

echo "Installing Cat Feeder Service..."

# Check if running as root/sudo
if [ "$EUID" -ne 0 ]; then
    echo "This script must be run with sudo privileges"
    echo "Usage: sudo ./install_service.sh"
    exit 1
fi

# Stop any existing service
echo "Stopping any existing service..."
systemctl stop $SERVICE_NAME 2>/dev/null || true

# Copy service file
echo "Installing service file..."
cp $SERVICE_FILE $INSTALL_PATH

# Set proper permissions
chmod 644 $INSTALL_PATH

# Reload systemd
echo "Reloading systemd daemon..."
systemctl daemon-reload

# Enable service to start on boot
echo "Enabling service for auto-start on boot..."
systemctl enable $SERVICE_NAME

# Start the service now
echo "Starting service..."
systemctl start $SERVICE_NAME

# Show status
echo ""
echo "Service installation complete!"
echo ""
echo "Service status:"
systemctl status $SERVICE_NAME --no-pager -l

echo ""
echo "Useful commands:"
echo "  Check status:    sudo systemctl status $SERVICE_NAME"
echo "  View logs:       sudo journalctl -u $SERVICE_NAME -f"
echo "  Stop service:    sudo systemctl stop $SERVICE_NAME"
echo "  Start service:   sudo systemctl start $SERVICE_NAME"
echo "  Restart service: sudo systemctl restart $SERVICE_NAME"
echo "  Disable boot:    sudo systemctl disable $SERVICE_NAME"
echo ""
echo "The cat feeder will now start automatically on boot!"