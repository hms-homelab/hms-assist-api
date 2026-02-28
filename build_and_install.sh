#!/bin/bash
# HMS-Assist Build and Install Script
#
# Builds the C++ API binary, installs it system-wide, sets up the Python
# entity-sync service, and registers both as systemd units.
#
# Usage:
#   ./build_and_install.sh [--skip-tests] [--skip-sync-install]

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
TOOLS_DIR="$SCRIPT_DIR/tools"
CONFIG_PATH="/etc/hms-assist/config.yaml"
SKIP_TESTS=false
SKIP_SYNC=false

for arg in "$@"; do
    case $arg in
        --skip-tests)   SKIP_TESTS=true ;;
        --skip-sync-install) SKIP_SYNC=true ;;
    esac
done

echo "==========================================="
echo " HMS-Assist Build and Install"
echo "==========================================="

if [ "$EUID" -eq 0 ]; then
    echo "ERROR: Do not run as root. sudo will be used only when needed."
    exit 1
fi

# ─── 1. Dependencies ──────────────────────────────────────────────────────────
echo ""
echo "[1/7] Checking C++ build dependencies..."

MISSING=()
for cmd in cmake g++ psql curl python3; do
    command -v "$cmd" >/dev/null 2>&1 || MISSING+=("$cmd")
done

if [ ${#MISSING[@]} -gt 0 ]; then
    echo "ERROR: Missing: ${MISSING[*]}"
    echo "Install with: sudo apt install cmake g++ postgresql-client curl python3"
    exit 1
fi
echo "✓ Dependencies OK"

# ─── 2. Database ──────────────────────────────────────────────────────────────
echo ""
echo "[2/7] Setting up PostgreSQL..."

DB_EXISTS=$(PGPASSWORD=maestro_postgres_2026_secure psql -h localhost -U maestro -d postgres \
    -tAc "SELECT 1 FROM pg_database WHERE datname='hms_assist'" 2>/dev/null || true)

if [ "$DB_EXISTS" != "1" ]; then
    echo "Creating database hms_assist..."
    PGPASSWORD=maestro_postgres_2026_secure psql -h localhost -U maestro -d postgres \
        -c "CREATE DATABASE hms_assist;"
fi

echo "Applying schema..."
PGPASSWORD=maestro_postgres_2026_secure psql -h localhost -U maestro -d hms_assist \
    -f "$SCRIPT_DIR/init_database.sql" -q
echo "✓ Database ready"

# ─── 3. Config ────────────────────────────────────────────────────────────────
echo ""
echo "[3/7] Checking config..."

if [ ! -f "$CONFIG_PATH" ]; then
    echo "Config not found at $CONFIG_PATH"
    EXAMPLE="$SCRIPT_DIR/config/config.yaml.example"
    if [ -f "$EXAMPLE" ]; then
        sudo mkdir -p /etc/hms-assist
        sudo cp "$EXAMPLE" "$CONFIG_PATH"
        sudo chmod 644 "$CONFIG_PATH"
        echo "⚠  Copied example config — edit $CONFIG_PATH before starting the service."
    else
        echo "ERROR: No config at $CONFIG_PATH and no example found."
        echo "Create $CONFIG_PATH from the template in config/config.yaml.example"
        exit 1
    fi
else
    echo "✓ Config present at $CONFIG_PATH"
fi

# ─── 4. Build ─────────────────────────────────────────────────────────────────
echo ""
echo "[4/7] Building C++ binary..."

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON 2>&1 | tail -3
make -j"$(nproc)" hms_assist
echo "✓ Binary built: $BUILD_DIR/hms_assist"

# ─── 5. Unit tests ────────────────────────────────────────────────────────────
if [ "$SKIP_TESTS" = false ]; then
    echo ""
    echo "[5/7] Running C++ unit tests..."
    make -j"$(nproc)" hms_assist_tests 2>&1 | tail -3
    ./hms_assist_tests --gtest_brief=1
    echo "✓ All unit tests passed"
else
    echo ""
    echo "[5/7] Skipping tests (--skip-tests)"
fi

# ─── 6. Install binary + service ─────────────────────────────────────────────
echo ""
echo "[6/7] Installing API binary and systemd service..."

sudo cp "$BUILD_DIR/hms_assist" /usr/local/bin/hms_assist
sudo chmod 755 /usr/local/bin/hms_assist
echo "✓ Installed /usr/local/bin/hms_assist"

sudo cp "$SCRIPT_DIR/hms-assist.service" /etc/systemd/system/
echo "✓ Installed hms-assist.service"

# ─── 7. Sync tool + service ───────────────────────────────────────────────────
echo ""
echo "[7/7] Setting up entity sync tool..."

if [ "$SKIP_SYNC" = false ]; then
    # Python venv
    if [ ! -d "$TOOLS_DIR/venv" ]; then
        echo "Creating Python venv..."
        python3 -m venv "$TOOLS_DIR/venv"
    fi
    "$TOOLS_DIR/venv/bin/pip" install -q -r "$TOOLS_DIR/requirements.txt"
    echo "✓ Python venv ready at $TOOLS_DIR/venv"

    sudo cp "$TOOLS_DIR/hms-assist-sync.service" /etc/systemd/system/
    echo "✓ Installed hms-assist-sync.service"
fi

# Reload and enable
sudo systemctl daemon-reload

if [ "$SKIP_SYNC" = false ]; then
    sudo systemctl enable hms-assist-sync
    sudo systemctl restart hms-assist-sync
    echo "✓ hms-assist-sync enabled and started"
fi

sudo systemctl enable hms-assist
sudo systemctl restart hms-assist

sleep 3
if systemctl is-active --quiet hms-assist; then
    echo "✓ hms-assist started"
else
    echo "ERROR: hms-assist failed to start"
    sudo journalctl -u hms-assist -n 30 --no-pager
    exit 1
fi

# ─── Done ─────────────────────────────────────────────────────────────────────
echo ""
echo "==========================================="
echo " Installation Complete"
echo "==========================================="
echo ""
curl -s http://localhost:8894/health | python3 -m json.tool
echo ""
echo "Commands:"
echo "  sudo systemctl status hms-assist          # API service"
echo "  sudo systemctl status hms-assist-sync     # Entity sync"
echo "  sudo journalctl -u hms-assist -f          # API logs"
echo "  sudo journalctl -u hms-assist-sync -f     # Sync logs"
echo "  curl http://localhost:8894/health          # Health check"
