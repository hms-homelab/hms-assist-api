#!/bin/bash
# HMS-Assist Build and Install Script

set -e  # Exit on error

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

echo "==========================================="
echo "HMS-Assist Build and Install Script"
echo "==========================================="

# Check if running as root for installation
if [ "$EUID" -eq 0 ]; then
    echo "ERROR: Do not run this script as root (use sudo only when prompted)"
    exit 1
fi

# Function to check if a command exists
command_exists() {
    command -v "$1" >/dev/null 2>&1
}

# Check dependencies
echo ""
echo "[1/6] Checking dependencies..."

MISSING_DEPS=()

if ! command_exists cmake; then
    MISSING_DEPS+=("cmake")
fi

if ! command_exists g++; then
    MISSING_DEPS+=("g++")
fi

if ! command_exists psql; then
    MISSING_DEPS+=("postgresql-client")
fi

if [ ${#MISSING_DEPS[@]} -gt 0 ]; then
    echo "ERROR: Missing dependencies: ${MISSING_DEPS[*]}"
    echo "Install with: sudo apt install ${MISSING_DEPS[*]}"
    exit 1
fi

echo "✓ All dependencies found"

# Initialize database
echo ""
echo "[2/6] Setting up PostgreSQL database..."

if psql -h localhost -U maestro -d postgres -c "SELECT 1 FROM pg_database WHERE datname='hms_assist'" | grep -q 1; then
    echo "✓ Database 'hms_assist' already exists"
else
    echo "Creating database 'hms_assist'..."
    psql -h localhost -U maestro -d postgres -c "CREATE DATABASE hms_assist;"
fi

echo "Initializing database schema..."
PGPASSWORD=maestro_postgres_2026_secure psql -h localhost -U maestro -d hms_assist -f "$SCRIPT_DIR/init_database.sql"

echo "✓ Database initialized"

# Clean build directory
echo ""
echo "[3/6] Preparing build directory..."

if [ -d "$BUILD_DIR" ]; then
    echo "Cleaning existing build directory..."
    rm -rf "$BUILD_DIR"
fi

mkdir -p "$BUILD_DIR"
echo "✓ Build directory ready"

# Build service
echo ""
echo "[4/6] Building HMS-Assist service..."

cd "$BUILD_DIR"
cmake ..
make -j$(nproc)

if [ ! -f "$BUILD_DIR/hms_assist" ]; then
    echo "ERROR: Build failed - binary not found"
    exit 1
fi

echo "✓ Build complete"

# Test service
echo ""
echo "[5/6] Testing service (5 seconds)..."

$BUILD_DIR/hms_assist &
SERVICE_PID=$!
sleep 5

# Check if service is running
if ! kill -0 $SERVICE_PID 2>/dev/null; then
    echo "ERROR: Service failed to start"
    exit 1
fi

# Test health endpoint
HEALTH_CHECK=$(curl -s http://localhost:8894/health || echo "failed")

if [[ "$HEALTH_CHECK" == *"healthy"* ]]; then
    echo "✓ Health check passed"
else
    echo "ERROR: Health check failed"
    kill $SERVICE_PID 2>/dev/null
    exit 1
fi

# Stop test service
kill $SERVICE_PID 2>/dev/null
sleep 2

echo "✓ Service test complete"

# Install systemd service
echo ""
echo "[6/6] Installing systemd service..."

if systemctl is-active --quiet hms-assist; then
    echo "Stopping existing service..."
    sudo systemctl stop hms-assist
fi

sudo cp "$SCRIPT_DIR/hms-assist.service" /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable hms-assist

echo "✓ Systemd service installed"

# Start service
echo ""
echo "Starting HMS-Assist service..."
sudo systemctl start hms-assist

sleep 3

if systemctl is-active --quiet hms-assist; then
    echo "✓ Service started successfully"
else
    echo "ERROR: Service failed to start"
    sudo journalctl -u hms-assist -n 50
    exit 1
fi

# Final status
echo ""
echo "==========================================="
echo "Installation Complete!"
echo "==========================================="
echo ""
echo "Service Status:"
sudo systemctl status hms-assist --no-pager | head -n 10
echo ""
echo "Health Check:"
curl -s http://localhost:8894/health | python3 -m json.tool
echo ""
echo "Useful Commands:"
echo "  sudo systemctl status hms-assist     - Check service status"
echo "  sudo systemctl restart hms-assist    - Restart service"
echo "  sudo journalctl -u hms-assist -f     - View logs"
echo "  curl http://localhost:8894/health    - Health check"
echo ""
echo "Next Steps:"
echo "  1. Flash ESP32-S3 firmware (see README.md)"
echo "  2. Test voice commands via MQTT"
echo "  3. Configure Home Assistant entities"
echo ""
