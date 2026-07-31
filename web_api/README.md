# Cat Feeder Web API

A simple, non-destructive web interface for monitoring and controlling your cat feeder remotely.

## Quick Start

1. **Install dependencies:**
   ```bash
   cd /home/ben/opener_code/web_api
   ./start.sh
   ```

2. **Access the dashboard:**
   - Local: http://localhost:8000
   - Via Tailscale: http://[your-pi-tailscale-ip]:8000

## Features

### Current Status Display
- Cans remaining
- Feed mode (INTERVAL/DAILY) with detailed schedule info
- Next feed time with countdown timer
- Operation status (machine state + Marlin state)
- Position information (X and Z axis)
- Weekend-only mode indicator
- Health check status
- Time remaining until next feed

### Remote Controls
- Manual feed trigger (test dispense)
- Eject-only operation (maintenance mode)
- System termination command
- Real-time status updates
- Mobile-friendly interface

### API Endpoints

#### Status Endpoints
- `GET /api/status` - Comprehensive feeder status with all details
  - Basic status (cans, states, positions)
  - Schedule information (mode, intervals, next feed time)
  - Countdown timer to next feed
  - Weekend mode status
  - Health check results
  
- `GET /api/schedule` - Detailed schedule configuration
  - Interval mode settings
  - Daily mode settings (time, weekend-only)
  - Next feed calculation
  - Current day status
  
- `GET /api/positions` - Detailed position information
  - X axis position (opener mechanism)
  - Z axis position (can stack)
  - Eject position tracking
  - Homing status
  
- `GET /api/system-info` - General system information
  - State file freshness
  - Machine and Marlin states
  - Inventory tracking
  - Health status

#### Control Endpoints
- `POST /api/feed` - Trigger manual feed
- `POST /api/eject` - Trigger eject-only operation
- `POST /api/terminate` - Terminate the feeder system

#### Health Check
- `GET /api/health` - Simple health check

#### Documentation
- `GET /docs` - Auto-generated API documentation (Swagger UI)
- `GET /` - Web dashboard interface

## How It Works

### Non-Destructive Integration
This web API is designed to be completely non-destructive to your existing C++ code:

1. **Reads state** from your existing `feeder_state.json` file
2. **Sends commands** via a simple `web_commands.json` file
3. **Your C++ code** checks for web commands every 2 seconds
4. **No modification** of core feeding logic

### File Communication
- **feeder_state.json** - Status information (created by your C++ app)
- **web_commands.json** - Commands from web interface (consumed by C++ app)

### Safety Features
- Commands only work when machine is idle
- Prevents multiple operations running simultaneously
- Automatic command cleanup after processing

## Tailscale Setup

To access from your phone via Tailscale:

1. **Install Tailscale** on your Raspberry Pi:
   ```bash
   curl -fsSL https://tailscale.com/install.sh | sh
   sudo tailscale up
   ```

2. **Install Tailscale** on your phone from app store

3. **Connect both devices** to same Tailscale network

4. **Access dashboard** at: `http://[pi-tailscale-ip]:8000`

## Adding a Webcam (Future)

To add webcam streaming:

1. **Install mjpg-streamer:**
   ```bash
   sudo apt install mjpg-streamer
   ```

2. **Start camera stream:**
   ```bash
   mjpg_streamer -i "input_uvc.so -d /dev/video0 -r 640x480 -f 10" -o "output_http.so -p 8081"
   ```

3. **Update web interface** to embed stream from port 8081

## Troubleshooting

### Web API won't start
- Check Python3 is installed: `python3 --version`
- Check port 8000 is available: `sudo netstat -tlnp | grep 8000`

### Commands not working
- Check file permissions in `/home/ben/opener_code/`
- Verify your C++ application is running
- Check for error messages in terminal

### Can't connect remotely
- Verify Tailscale is running: `sudo tailscale status`
- Check firewall allows port 8000
- Try local access first: `http://localhost:8000`

## Logs

Web API logs are shown in the terminal when running `./start.sh`

Your C++ application will show:
- "Web API: Manual feed command received" when commands are processed
- "Web API: Cannot start manual feed - machine busy" if machine is occupied

## Safe Shutdown

The web interface includes a safe shutdown option that:
1. Stops any running operations safely
2. Saves current state
3. Shuts down the Raspberry Pi cleanly

*Note: This feature can be added in a future update.*
