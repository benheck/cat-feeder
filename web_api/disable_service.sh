#!/bin/bash
# Disable the Cat Feeder Web API service

SERVICE_NAME="cat-feeder-web"

echo "Disabling Cat Feeder Web API service..."

# Stop the service if it's running
echo "Stopping service..."
sudo systemctl stop "$SERVICE_NAME"

# Disable auto-start on boot
echo "Disabling auto-start..."
sudo systemctl disable "$SERVICE_NAME"

# Check status
echo ""
echo "Service status:"
sudo systemctl is-enabled "$SERVICE_NAME" 2>/dev/null || echo "Service is disabled"
sudo systemctl is-active "$SERVICE_NAME" 2>/dev/null || echo "Service is stopped"

echo ""
echo "Cat Feeder Web API service has been disabled."
echo ""
echo "To re-enable later, use:"
echo "  sudo systemctl enable $SERVICE_NAME"
echo "  sudo systemctl start $SERVICE_NAME"
echo ""
echo "Or run ./install_service.sh again"
