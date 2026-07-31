#!/bin/bash
# Setup passwordless sudo for reboot command
# This allows the cat feeder app to reboot the system via web API

echo "Setting up passwordless sudo for reboot command..."

# Check if already configured
if [ -f /etc/sudoers.d/cat-feeder-reboot ]; then
    echo "⚠️  Configuration file already exists"
    echo "Removing old configuration..."
    sudo rm /etc/sudoers.d/cat-feeder-reboot
fi

# Create a sudoers file for cat feeder reboot
echo "Creating sudoers configuration..."
sudo tee /etc/sudoers.d/cat-feeder-reboot > /dev/null << EOF
# Allow user ben to run reboot without password
# Created by cat-feeder setup script
ben ALL=(ALL) NOPASSWD: /sbin/reboot
ben ALL=(ALL) NOPASSWD: /usr/sbin/reboot
ben ALL=(ALL) NOPASSWD: /bin/systemctl reboot
ben ALL=(ALL) NOPASSWD: /usr/bin/systemctl reboot
EOF

# Set proper permissions on the sudoers file
sudo chmod 0440 /etc/sudoers.d/cat-feeder-reboot

echo ""
echo "Verifying configuration..."

# Verify the syntax
if sudo visudo -c -f /etc/sudoers.d/cat-feeder-reboot; then
    echo ""
    echo "✓ Passwordless reboot configured successfully!"
    echo ""
    echo "Testing sudo reboot permission (dry run)..."
    if sudo -n reboot --help > /dev/null 2>&1; then
        echo "✓ Sudo reboot is accessible without password"
    else
        echo "⚠️  Warning: sudo -n test failed, but this might be normal"
    fi
    echo ""
    echo "Setup complete! The web API can now reboot the system."
else
    echo ""
    echo "✗ Error in sudoers configuration!"
    echo "Removing invalid configuration..."
    sudo rm /etc/sudoers.d/cat-feeder-reboot
    exit 1
fi
