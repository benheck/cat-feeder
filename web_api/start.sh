#!/bin/bash
# Simple startup script for the cat feeder web API

cd "$(dirname "$0")"

echo "Starting Cat Feeder Web API..."
echo "Dashboard will be available at: http://localhost:8000"
echo "API docs will be available at: http://localhost:8000/docs"
echo ""

# Install dependencies if needed
if [ ! -d "venv" ]; then
    echo "Creating virtual environment..."
    python3 -m venv venv
fi

source venv/bin/activate

echo "Installing/updating dependencies..."
pip install -r requirements.txt

echo "Starting web server..."
python app.py
