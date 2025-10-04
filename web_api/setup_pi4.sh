#!/bin/bash

# Pi4 Complete Setup Script for Cat Feeder
# This installs all dependencies for both C++ application and Python web API

echo "Setting up Cat Feeder complete system for Pi4..."

# Install required system packages
echo "Installing system dependencies..."
sudo apt update
sudo apt install -y \
    build-essential \
    cmake \
    git \
    libgpiod-dev \
    libi2c-dev \
    i2c-tools \
    python3 \
    python3-pip \
    python3-venv

# Enable I2C interface
echo "Enabling I2C interface..."
sudo raspi-config nonint do_i2c 0

# Navigate to web API directory
cd /home/ben/opener_code/web_api

# Create virtual environment
echo "Creating Python virtual environment..."
python3 -m venv venv

# Activate virtual environment
echo "Activating virtual environment..."
source venv/bin/activate

# Install dependencies
echo "Installing Python dependencies..."
pip install fastapi uvicorn

# Create systemd service file for Pi4
echo "Creating systemd service file..."
sudo tee /etc/systemd/system/cat-feeder-web.service > /dev/null << EOF
[Unit]
Description=Cat Feeder Web API
After=network.target

[Service]
Type=simple
User=ben
WorkingDirectory=/home/ben/opener_code/web_api
Environment=PATH=/home/ben/opener_code/web_api/venv/bin
ExecStart=/home/ben/opener_code/web_api/venv/bin/python -m uvicorn app:app --host 0.0.0.0 --port 8000
Restart=always

[Install]
WantedBy=multi-user.target
EOF

# Enable and start the service
echo "Enabling and starting the service..."
sudo systemctl daemon-reload
sudo systemctl enable cat-feeder-web.service
sudo systemctl start cat-feeder-web.service

echo "Setup complete!"
echo "Check service status with: sudo systemctl status cat-feeder-web.service"
echo "View logs with: sudo journalctl -u cat-feeder-web.service -f"