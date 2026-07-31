#!/bin/bash
# Setup passwordless sudo for reboot command
# This allows the cat feeder app to reboot the system via web API

echo "Setting up passwordless sudo for reboot command..."

# Create a sudoers file for cat feeder reboot
sudo tee /etc/sudoers.d/cat-feeder-reboot > /dev/null << EOF
# Allow user ben to run reboot without password
ben ALL=(ALL) NOPASSWD: /sbin/reboot, /usr/sbin/reboot, /bin/systemctl reboot
EOF

# Set proper permissions on the sudoers file
sudo chmod 0440 /etc/sudoers.d/cat-feeder-reboot

# Verify the syntax
if sudo visudo -c -f /etc/sudoers.d/cat-feeder-reboot; then
    echo "✓ Passwordless reboot configured successfully!"
    echo "  The web API can now reboot the system"
else
    echo "✗ Error in sudoers configuration"
    sudo rm /etc/sudoers.d/cat-feeder-reboot
    exit 1
fi
