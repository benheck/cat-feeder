#!/usr/bin/env python3
"""
Cat Feeder Web API
Non-destructive web interface for monitoring and basic control
"""

from fastapi import FastAPI, HTTPException
from fastapi.staticfiles import StaticFiles
from fastapi.responses import HTMLResponse
import json
import os
import time
from datetime import datetime
from typing import Dict, Any

app = FastAPI(title="Cat Feeder API", version="1.0.0")

# Path to your existing JSON state file  
STATE_FILE_PATH = "/home/ben/machine_state.json"  # Updated to use single JSON
COMMAND_FILE_PATH = "/home/ben/web_commands.json"

def read_state() -> Dict[str, Any]:
    """Read the current state from the JSON file your C++ code uses"""
    try:
        if os.path.exists(STATE_FILE_PATH):
            with open(STATE_FILE_PATH, 'r') as f:
                return json.load(f)
        else:
            return {"error": "State file not found"}
    except Exception as e:
        return {"error": f"Failed to read state: {str(e)}"}

def write_command(command: Dict[str, Any]) -> bool:
    """Write a command that your C++ code can pick up"""
    try:
        # Add timestamp to command
        command["timestamp"] = int(time.time())
        
        with open(COMMAND_FILE_PATH, 'w') as f:
            json.dump(command, f, indent=2)
        return True
    except Exception as e:
        print(f"Failed to write command: {e}")
        return False

@app.get("/")
async def root():
    """Serve the main dashboard page"""
    return HTMLResponse(content=get_dashboard_html())

@app.get("/api/status")
async def get_status():
    """Get current feeder status with detailed information"""
    state = read_state()
    
    if "error" in state:
        raise HTTPException(status_code=500, detail=state["error"])
    
    current_time = int(time.time())
    
    # Format next feed time as human readable
    feed_time_unix = state.get("feed_time", 0)
    if feed_time_unix and feed_time_unix > 0:
        try:
            feed_time_readable = datetime.fromtimestamp(feed_time_unix).strftime("%Y-%m-%d %H:%M:%S")
            # Calculate time remaining
            time_remaining_seconds = feed_time_unix - current_time
            if time_remaining_seconds > 0:
                hours = time_remaining_seconds // 3600
                minutes = (time_remaining_seconds % 3600) // 60
                time_remaining_str = f"{hours}h {minutes}m"
            else:
                time_remaining_str = "Overdue"
        except (ValueError, OSError):
            feed_time_readable = "Invalid time"
            time_remaining_str = "Unknown"
    else:
        feed_time_readable = "Not set"
        time_remaining_str = "Not scheduled"
    
    # Determine if weekend mode is active
    schedule_mode = state.get("schedule_mode", "UNKNOWN")
    daily_weekend_only = state.get("daily_weekend_only", False)
    
    # Calculate system uptime from state file timestamp
    state_timestamp = state.get("timestamp", 0)
    if state_timestamp:
        try:
            state_age_seconds = current_time - int(state_timestamp)
            state_age_str = f"{state_age_seconds}s ago"
        except (ValueError, TypeError):
            state_age_str = "Unknown"
    else:
        state_age_str = "Unknown"
    
    # Format the response for easier web consumption
    response = {
        "timestamp": current_time,
        "last_update_from_system": state_age_str,
        
        # Basic status
        "cans_left": state.get("cans_loaded", 0),
        "operation_running": state.get("machine_state", "idle") != "idle",
        "machine_state": state.get("machine_state", "unknown"),
        "marlin_state": state.get("marlin_state", "unknown"),
        
        # Schedule information
        "feed_mode": schedule_mode,
        "next_feed_time": feed_time_readable,
        "next_feed_time_unix": feed_time_unix,
        "time_remaining_until_feed": time_remaining_str,
        "feed_interval_hours": state.get("feed_gap", 0),
        "feed_interval_minutes": int(state.get("feed_gap", 1) * 60),
        
        # Daily schedule details
        "daily_feed_hour": state.get("daily_feed_hour", 0),
        "daily_feed_minute": state.get("daily_feed_minute", 0),
        "daily_weekend_only": daily_weekend_only,
        "daily_feed_time_formatted": f"{state.get('daily_feed_hour', 0):02d}:{state.get('daily_feed_minute', 0):02d}",
        
        # Position information
        "x_position": state.get("x_position", 0),
        "z_position": state.get("z_position", 0),
        "eject_last": state.get("eject_last", 318),
        
        # Health check status
        "health_check_failed": state.get("health_check_failed", False),
        "health_error_message": state.get("health_error_message", ""),
        
        # Additional computed info
        "is_weekend": datetime.now().weekday() >= 5,  # 5=Saturday, 6=Sunday
        "feeding_enabled": not (daily_weekend_only and schedule_mode == "DAILY" and datetime.now().weekday() < 5),
        
        # Raw state for debugging
        "raw_state": state
    }
    
    return response

@app.post("/api/feed")
async def manual_feed():
    """Trigger a manual feed (test dispense)"""
    command = {
        "action": "manual_feed",
        "source": "web_api"
    }
    
    if write_command(command):
        return {"success": True, "message": "Manual feed command sent"}
    else:
        raise HTTPException(status_code=500, detail="Failed to send command")

@app.post("/api/eject")
async def eject_only():
    """Trigger eject-only operation (maintenance mode)"""
    command = {
        "action": "eject_only",
        "source": "web_api"
    }
    
    if write_command(command):
        return {"success": True, "message": "Eject-only command sent"}
    else:
        raise HTTPException(status_code=500, detail="Failed to send command")

@app.post("/api/terminate")
async def terminate_system():
    """Terminate the cat feeder control system"""
    command = {
        "action": "terminate",
        "source": "web_api"
    }
    
    if write_command(command):
        return {"success": True, "message": "Terminate command sent - system will shut down"}
    else:
        raise HTTPException(status_code=500, detail="Failed to send terminate command")

@app.get("/api/health")
async def health_check():
    """Simple health check endpoint"""
    state_exists = os.path.exists(STATE_FILE_PATH)
    return {
        "status": "healthy" if state_exists else "degraded",
        "state_file_exists": state_exists,
        "timestamp": int(time.time())
    }

@app.get("/api/schedule")
async def get_schedule():
    """Get detailed schedule configuration"""
    state = read_state()
    
    if "error" in state:
        raise HTTPException(status_code=500, detail=state["error"])
    
    schedule_mode = state.get("schedule_mode", "UNKNOWN")
    daily_weekend_only = state.get("daily_weekend_only", False)
    current_time = int(time.time())
    
    response = {
        "mode": schedule_mode,
        "interval_mode": {
            "enabled": schedule_mode == "INTERVAL",
            "feed_gap_hours": state.get("feed_gap", 0),
            "feed_gap_minutes": int(state.get("feed_gap", 0) * 60)
        },
        "daily_mode": {
            "enabled": schedule_mode == "DAILY",
            "hour": state.get("daily_feed_hour", 0),
            "minute": state.get("daily_feed_minute", 0),
            "formatted_time": f"{state.get('daily_feed_hour', 0):02d}:{state.get('daily_feed_minute', 0):02d}",
            "weekend_only": daily_weekend_only
        },
        "next_feed": {
            "unix_timestamp": state.get("feed_time", 0),
            "human_readable": datetime.fromtimestamp(state.get("feed_time", 0)).strftime("%Y-%m-%d %H:%M:%S") if state.get("feed_time", 0) > 0 else "Not set",
            "is_overdue": state.get("feed_time", 0) < current_time if state.get("feed_time", 0) > 0 else False
        },
        "current_day": {
            "is_weekend": datetime.now().weekday() >= 5,
            "day_name": datetime.now().strftime("%A"),
            "feeding_enabled": not (daily_weekend_only and schedule_mode == "DAILY" and datetime.now().weekday() < 5)
        }
    }
    
    return response

@app.get("/api/positions")
async def get_positions():
    """Get detailed position information for X and Z axes"""
    state = read_state()
    
    if "error" in state:
        raise HTTPException(status_code=500, detail=state["error"])
    
    x_pos = state.get("x_position", 0)
    z_pos = state.get("z_position", 0)
    eject_last = state.get("eject_last", 318)
    cans_loaded = state.get("cans_loaded", 0)
    
    response = {
        "x_axis": {
            "current_position": x_pos,
            "description": "Horizontal position (opener mechanism)"
        },
        "z_axis": {
            "current_position": z_pos,
            "eject_position": eject_last,
            "cans_below_current": cans_loaded,
            "description": "Vertical position (can stack)"
        },
        "status": {
            "x_homed": x_pos == 0 or x_pos < 0.1,  # Assuming home is near 0
            "z_positioned": abs(z_pos - eject_last) < 1.0  # Within 1mm of eject position
        }
    }
    
    return response

@app.get("/api/system-info")
async def get_system_info():
    """Get general system information and statistics"""
    state = read_state()
    
    if "error" in state:
        raise HTTPException(status_code=500, detail=state["error"])
    
    current_time = int(time.time())
    state_timestamp = state.get("timestamp", 0)
    
    if state_timestamp:
        try:
            state_age = current_time - int(state_timestamp)
        except (ValueError, TypeError):
            state_age = 0
    else:
        state_age = 0
    
    response = {
        "system": {
            "state_file_location": STATE_FILE_PATH,
            "command_file_location": COMMAND_FILE_PATH,
            "last_state_update": datetime.fromtimestamp(int(state_timestamp)).strftime("%Y-%m-%d %H:%M:%S") if state_timestamp else "Unknown",
            "state_age_seconds": state_age,
            "state_is_fresh": state_age < 300  # Consider fresh if updated within 5 minutes
        },
        "machine": {
            "state": state.get("machine_state", "unknown"),
            "marlin_state": state.get("marlin_state", "unknown"),
            "operation_running": state.get("machine_state", "idle") != "idle"
        },
        "inventory": {
            "cans_loaded": state.get("cans_loaded", 0),
            "feeds_available": state.get("cans_loaded", 0)
        },
        "health": {
            "check_failed": state.get("health_check_failed", False),
            "error_message": state.get("health_error_message", ""),
            "status": "unhealthy" if state.get("health_check_failed", False) else "healthy"
        },
        "timestamp": current_time
    }
    
    return response

def get_dashboard_html() -> str:
    """Return a simple HTML dashboard"""
    return """
<!DOCTYPE html>
<html>
<head>
    <title>Cat Feeder Dashboard</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { 
            font-family: Arial, sans-serif; 
            margin: 20px; 
            background-color: #f5f5f5;
        }
        .container { 
            max-width: 600px; 
            margin: 0 auto; 
            background: white;
            padding: 20px;
            border-radius: 8px;
            box-shadow: 0 2px 4px rgba(0,0,0,0.1);
        }
        .status-card {
            background: #f8f9fa;
            border: 1px solid #dee2e6;
            border-radius: 6px;
            padding: 15px;
            margin: 10px 0;
        }
        .status-title { 
            font-weight: bold; 
            color: #495057;
            margin-bottom: 8px;
        }
        .status-value { 
            font-size: 1.2em; 
            color: #212529;
        }
        .button {
            background-color: #007bff;
            color: white;
            border: none;
            padding: 10px 20px;
            border-radius: 4px;
            cursor: pointer;
            margin: 5px;
            font-size: 16px;
        }
        .button:hover { background-color: #0056b3; }
        .button:disabled { 
            background-color: #6c757d; 
            cursor: not-allowed; 
        }
        .warning { color: #dc3545; }
        .success { color: #28a745; }
        .health-alert {
            background-color: #f8d7da;
            border: 2px solid #dc3545;
            border-radius: 6px;
            padding: 15px;
            margin: 15px 0;
            color: #721c24;
            display: none;
        }
        .health-alert.active {
            display: block;
        }
        .health-alert-title {
            font-weight: bold;
            font-size: 1.1em;
            margin-bottom: 5px;
        }
        #status { margin-top: 20px; }
        .last-updated {
            font-size: 0.9em;
            color: #6c757d;
            text-align: center;
            margin-top: 15px;
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>🐱 Cat Feeder Dashboard</h1>
        
        <div id="health-alert" class="health-alert">
            <div class="health-alert-title">⚠️ HEALTH CHECK FAILED</div>
            <div id="health-error-message"></div>
            <div style="margin-top: 8px; font-size: 0.9em;">
                Recommended: Restart the cat feeder service
            </div>
        </div>
        
        <div id="status">
            <div class="status-card">
                <div class="status-title">Cans Left</div>
                <div class="status-value" id="cans-left">Loading...</div>
            </div>
            
            <div class="status-card">
                <div class="status-title">Feed Mode</div>
                <div class="status-value" id="feed-mode">Loading...</div>
                <div style="font-size: 0.85em; color: #6c757d; margin-top: 5px;" id="feed-mode-details"></div>
            </div>
            
            <div class="status-card">
                <div class="status-title">Next Feed Time</div>
                <div class="status-value" id="next-feed">Loading...</div>
                <div style="font-size: 0.9em; color: #007bff; margin-top: 5px; font-weight: bold;" id="time-remaining"></div>
            </div>
            
            <div class="status-card">
                <div class="status-title">Operation Status</div>
                <div class="status-value" id="operation-status">Loading...</div>
                <div style="font-size: 0.85em; color: #6c757d; margin-top: 5px;" id="marlin-status"></div>
            </div>
            
            <div class="status-card">
                <div class="status-title">Position Info</div>
                <div style="font-size: 0.95em; color: #212529;">
                    X: <span id="x-position">-</span> | Z: <span id="z-position">-</span>
                </div>
                <div style="font-size: 0.85em; color: #6c757d; margin-top: 5px;">
                    Eject Position: <span id="eject-position">-</span>
                </div>
            </div>
        </div>

        <div style="text-align: center; margin-top: 20px;">
            <button class="button" onclick="refreshStatus()" style="display: block; width: 100%; margin-bottom: 30px;">
                🔄 Refresh
            </button>
            <button class="button" onclick="ejectOnly()" id="eject-btn" style="display: block; width: 100%; margin-bottom: 10px; background-color: #fd7e14;">
                📤 Eject Only
            </button>
            <button class="button" onclick="manualFeed()" id="feed-btn" style="display: block; width: 100%; margin-bottom: 10px;">
                🍽️ Manual Feed
            </button>
            <button class="button" onclick="terminateSystem()" id="terminate-btn" style="display: block; width: 100%; margin-top: 20px; background-color: #dc3545;">
                🛑 Terminate System
            </button>
        </div>
        
        <div class="last-updated" id="last-updated"></div>
        
        <div id="message" style="margin-top: 15px; text-align: center;"></div>
    </div>

    <script>
        let autoRefresh = null;

        async function fetchStatus() {
            try {
                const response = await fetch('/api/status');
                const data = await response.json();
                
                // Basic status
                document.getElementById('cans-left').textContent = data.cans_left;
                document.getElementById('feed-mode').textContent = data.feed_mode;
                document.getElementById('next-feed').textContent = data.next_feed_time;
                
                // Time remaining with visual indicator
                const timeRemaining = document.getElementById('time-remaining');
                if (data.time_remaining_until_feed) {
                    if (data.time_remaining_until_feed === "Overdue") {
                        timeRemaining.textContent = '⚠️ Feed is overdue!';
                        timeRemaining.style.color = '#dc3545';
                    } else if (data.time_remaining_until_feed === "Not scheduled") {
                        timeRemaining.textContent = 'No feed scheduled';
                        timeRemaining.style.color = '#6c757d';
                    } else {
                        timeRemaining.textContent = '⏱️ ' + data.time_remaining_until_feed + ' remaining';
                        timeRemaining.style.color = '#007bff';
                    }
                }
                
                // Feed mode details
                const feedModeDetails = document.getElementById('feed-mode-details');
                if (data.feed_mode === 'INTERVAL') {
                    feedModeDetails.textContent = `Every ${data.feed_interval_hours} hours`;
                } else if (data.feed_mode === 'DAILY') {
                    let dailyText = `Daily at ${data.daily_feed_time_formatted}`;
                    if (data.daily_weekend_only) {
                        dailyText += ' (Weekends Only)';
                        if (!data.feeding_enabled) {
                            dailyText += ' - 🚫 Feeding disabled (weekday)';
                        }
                    }
                    feedModeDetails.textContent = dailyText;
                }
                
                // Operation status with Marlin state
                const opStatus = document.getElementById('operation-status');
                const marlinStatus = document.getElementById('marlin-status');
                if (data.operation_running) {
                    opStatus.textContent = 'RUNNING (' + data.machine_state + ')';
                    opStatus.className = 'status-value warning';
                } else {
                    opStatus.textContent = 'IDLE';
                    opStatus.className = 'status-value success';
                }
                marlinStatus.textContent = 'Marlin: ' + data.marlin_state;
                
                // Position information
                document.getElementById('x-position').textContent = data.x_position.toFixed(2);
                document.getElementById('z-position').textContent = data.z_position.toFixed(2);
                document.getElementById('eject-position').textContent = data.eject_last.toFixed(2);
                
                // Show/hide health alert
                const healthAlert = document.getElementById('health-alert');
                const healthMessage = document.getElementById('health-error-message');
                if (data.health_check_failed) {
                    healthAlert.classList.add('active');
                    healthMessage.textContent = data.health_error_message || 'Communication check failed - feed skipped';
                } else {
                    healthAlert.classList.remove('active');
                }
                
                // Last updated timestamp
                const lastUpdated = document.getElementById('last-updated');
                const now = new Date();
                lastUpdated.textContent = `Last updated: ${now.toLocaleTimeString()} | System last update: ${data.last_update_from_system}`;
                
                // Disable buttons if operation is running
                const feedBtn = document.getElementById('feed-btn');
                const ejectBtn = document.getElementById('eject-btn');
                feedBtn.disabled = data.operation_running;
                ejectBtn.disabled = data.operation_running;
                // Terminate button is never disabled - always available for emergency stop
                
            } catch (error) {
                console.error('Error fetching status:', error);
                document.getElementById('message').innerHTML = 
                    '<span class="warning">Error connecting to feeder</span>';
            }
        }

        async function manualFeed() {
            const feedBtn = document.getElementById('feed-btn');
            feedBtn.disabled = true;
            
            try {
                const response = await fetch('/api/feed', { method: 'POST' });
                const data = await response.json();
                
                if (data.success) {
                    showMessage('Manual feed command sent!', 'success');
                    // Refresh status after a short delay
                    setTimeout(fetchStatus, 1000);
                } else {
                    showMessage('Failed to send feed command', 'warning');
                }
            } catch (error) {
                showMessage('Error: ' + error.message, 'warning');
                console.error('Error:', error);
            } finally {
                setTimeout(() => { feedBtn.disabled = false; }, 2000);
            }
        }

        async function ejectOnly() {
            const ejectBtn = document.getElementById('eject-btn');
            ejectBtn.disabled = true;
            
            try {
                const response = await fetch('/api/eject', { method: 'POST' });
                const data = await response.json();
                
                if (data.success) {
                    showMessage('Eject-only command sent!', 'success');
                    // Refresh status after a short delay
                    setTimeout(fetchStatus, 1000);
                } else {
                    showMessage('Failed to send eject command', 'warning');
                }
            } catch (error) {
                showMessage('Error: ' + error.message, 'warning');
                console.error('Error:', error);
            } finally {
                setTimeout(() => { ejectBtn.disabled = false; }, 2000);
            }
        }

        async function terminateSystem() {
            if (!confirm('Are you sure you want to terminate the cat feeder system? This will shut down the control program.')) {
                return;
            }
            
            const terminateBtn = document.getElementById('terminate-btn');
            terminateBtn.disabled = true;
            
            try {
                const response = await fetch('/api/terminate', { method: 'POST' });
                const data = await response.json();
                
                if (data.success) {
                    showMessage('Terminate command sent! System shutting down...', 'warning');
                    // Stop auto-refresh since system is terminating
                    stopAutoRefresh();
                } else {
                    showMessage('Failed to send terminate command', 'warning');
                }
            } catch (error) {
                showMessage('Error: ' + error.message, 'warning');
                console.error('Error:', error);
            } finally {
                setTimeout(() => { terminateBtn.disabled = false; }, 5000);
            }
        }

        function refreshStatus() {
            fetchStatus();
        }

        function showMessage(text, type) {
            const messageDiv = document.getElementById('message');
            messageDiv.textContent = text;
            messageDiv.className = type;
        }

        function clearMessage() {
            const messageDiv = document.getElementById('message');
            messageDiv.textContent = '';
            messageDiv.className = '';
        }

        // Auto-refresh every 5 seconds
        function startAutoRefresh() {
            autoRefresh = setInterval(fetchStatus, 5000);
        }

        function stopAutoRefresh() {
            if (autoRefresh) {
                clearInterval(autoRefresh);
                autoRefresh = null;
            }
        }

        // Initial load and start auto-refresh
        fetchStatus();
        startAutoRefresh();

        // Handle page visibility to pause/resume auto-refresh
        document.addEventListener('visibilitychange', function() {
            if (document.hidden) {
                stopAutoRefresh();
            } else {
                startAutoRefresh();
            }
        });
    </script>
</body>
</html>
    """

if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=8000)
