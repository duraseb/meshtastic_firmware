#!/usr/bin/env bash
# Build script that ensures .env variables are loaded
# This is necessary because PlatformIO's ${sysenv.VAR} doesn't always load .env files correctly
#
# Usage: bin/build-with-env.sh run -e <environment> [-t upload]
# Example: bin/build-with-env.sh run -e seeed-xiao-s3 -t upload

set -e

# Go to project root (parent of bin directory)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

# Load .env file
if [ -f .env ]; then
    echo "=== Loading environment variables from .env ==="
    # Export all variables
    export $(cat .env | grep -v '^#' | grep -v '^$' | xargs)
    # Log each loaded variable (name and length only, not value)
    while IFS='=' read -r key value; do
        # Skip comments and empty lines
        [[ "$key" =~ ^#.*$ ]] && continue
        [[ -z "$key" ]] && continue
        # Get the actual value from environment (in case of variable expansion)
        actual_value="${!key}"
        echo "  ✅ $key (length: ${#actual_value})"
    done < .env
else
    echo "❌ ERROR: .env file not found!"
    echo "Please create .env from .env.example and add your API keys"
    exit 1
fi

# Find PlatformIO executable
if command -v pio &> /dev/null; then
    PIO_CMD="pio"
elif command -v platformio &> /dev/null; then
    PIO_CMD="platformio"
elif [ -f "$HOME/.platformio/penv/bin/pio" ]; then
    PIO_CMD="$HOME/.platformio/penv/bin/pio"
else
    echo "❌ ERROR: PlatformIO not found!"
    echo "Please install PlatformIO: https://platformio.org/install"
    exit 1
fi

# Run PlatformIO command with all arguments passed through
echo "=== Running: $PIO_CMD $@ ==="
$PIO_CMD "$@"

