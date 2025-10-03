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
STATE_FILE_PATH = "/home/ben/machine_state.json"  # Use existing file
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
    """Get current feeder status"""
    state = read_state()
    
    if "error" in state:
        raise HTTPException(status_code=500, detail=state["error"])
    
    # Format next feed time as human readable
    feed_time_unix = state.get("feed_time", 0)
    if feed_time_unix and feed_time_unix > 0:
        try:
            feed_time_readable = datetime.fromtimestamp(feed_time_unix).strftime("%Y-%m-%d %H:%M:%S")
        except (ValueError, OSError):
            feed_time_readable = "Invalid time"
    else:
        feed_time_readable = "Not set"
    
    # Format the response for easier web consumption
    response = {
        "timestamp": int(time.time()),
        "cans_left": state.get("cans_loaded", 0),
        "feed_mode": state.get("schedule_mode", "UNKNOWN"),
        "next_feed_time": feed_time_readable,
        "next_feed_time_unix": feed_time_unix,  # Keep original for calculations
        "operation_running": False,  # TODO: add this to your JSON if needed
        "machine_state": state.get("machine_state", "unknown"),
        "feed_interval_minutes": int(state.get("feed_gap", 1) * 60),
        "daily_feed_hour": state.get("daily_feed_hour", 0),
        "daily_feed_minute": state.get("daily_feed_minute", 0),
        "raw_state": state  # Include full state for debugging
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

@app.get("/api/health")
async def health_check():
    """Simple health check endpoint"""
    state_exists = os.path.exists(STATE_FILE_PATH)
    return {
        "status": "healthy" if state_exists else "degraded",
        "state_file_exists": state_exists,
        "timestamp": int(time.time())
    }

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
        
        <div id="status">
            <div class="status-card">
                <div class="status-title">Cans Left</div>
                <div class="status-value" id="cans-left">Loading...</div>
            </div>
            
            <div class="status-card">
                <div class="status-title">Feed Mode</div>
                <div class="status-value" id="feed-mode">Loading...</div>
            </div>
            
            <div class="status-card">
                <div class="status-title">Next Feed Time</div>
                <div class="status-value" id="next-feed">Loading...</div>
            </div>
            
            <div class="status-card">
                <div class="status-title">Operation Status</div>
                <div class="status-value" id="operation-status">Loading...</div>
            </div>
        </div>

        <div style="text-align: center; margin-top: 20px;">
            <button class="button" onclick="manualFeed()" id="feed-btn">
                🍽️ Manual Feed (Test)
            </button>
            <button class="button" onclick="refreshStatus()">
                🔄 Refresh
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
                
                document.getElementById('cans-left').textContent = data.cans_left;
                document.getElementById('feed-mode').textContent = data.feed_mode;
                document.getElementById('next-feed').textContent = data.next_feed_time;
                
                const opStatus = document.getElementById('operation-status');
                if (data.operation_running) {
                    opStatus.textContent = 'RUNNING (' + data.machine_state + ')';
                    opStatus.className = 'status-value warning';
                } else {
                    opStatus.textContent = 'IDLE';
                    opStatus.className = 'status-value success';
                }
                
                // Disable feed button if operation is running
                const feedBtn = document.getElementById('feed-btn');
                feedBtn.disabled = data.operation_running;
                
                document.getElementById('last-updated').textContent = 
                    'Last updated: ' + new Date().toLocaleTimeString();
                
                clearMessage();
                
            } catch (error) {
                showMessage('Error fetching status: ' + error.message, 'warning');
                console.error('Error:', error);
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
