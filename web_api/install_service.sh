#!/bin/bash
# Install the Cat Feeder Web API as a system service

SERVICE_FILE="/home/ben/opener_code/web_api/cat-feeder-web.service"
SYSTEM_SERVICE="/etc/systemd/system/cat-feeder-web.service"

echo "Installing Cat Feeder Web API service..."

# Copy service file to system location
sudo cp "$SERVICE_FILE" "$SYSTEM_SERVICE"

# Reload systemd and enable the service
sudo systemctl daemon-reload
sudo systemctl enable cat-feeder-web.service

echo "Service installed! Use these commands:"
echo ""
echo "Start:   sudo systemctl start cat-feeder-web"
echo "Stop:    sudo systemctl stop cat-feeder-web"
echo "Status:  sudo systemctl status cat-feeder-web"
echo "Logs:    sudo journalctl -u cat-feeder-web -f"
echo ""
echo "The service will auto-start on boot."
echo "Access dashboard at: http://localhost:8000"
