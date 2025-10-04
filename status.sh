#!/bin/bash

# Quick status check for cat feeder service
echo "Cat feeder service status:"
sudo systemctl status cat-feeder --no-pager -l