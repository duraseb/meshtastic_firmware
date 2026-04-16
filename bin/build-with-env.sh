#!/usr/bin/env bash
# Build script that ensures .env variables are loaded
# Supports both global .env and variant-specific .env files
#
# Usage:
#   From project root:     bin/build-with-env.sh run -e <environment> [-t upload]
#   From variant directory: ../../../bin/build-with-env.sh run [-t upload]
#                          (auto-detects environment from local platformio.ini)
#
# Environment files (loaded in order, later overrides earlier):
#   1. <project_root>/.env           - Global settings (API keys, etc.)
#   2. <variant_dir>/.env            - Variant-specific overrides
#
# Examples:
#   bin/build-with-env.sh run -e seeed-xiao-s3 -t upload
#   cd variants/esp32s3/seeed_xiao_s3 && ../../../bin/build-with-env.sh run -t upload

set -e

# Determine script and project directories
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CURRENT_DIR="$(pwd)"

# Function to load .env file
load_env_file() {
    local env_file="$1"
    local label="$2"

    if [ -f "$env_file" ]; then
        echo "=== Loading $label: $env_file ==="
        # Export all variables
        export $(cat "$env_file" | grep -v '^#' | grep -v '^$' | xargs 2>/dev/null) 2>/dev/null || true
        # Log each loaded variable (name and length only, not value)
        while IFS='=' read -r key value; do
            # Skip comments and empty lines
            [[ "$key" =~ ^#.*$ ]] && continue
            [[ -z "$key" ]] && continue
            # Get the actual value from environment
            actual_value="${!key}"
            echo "  ✅ $key (length: ${#actual_value})"
        done < "$env_file"
        return 0
    fi
    return 1
}

# Function to extract environment name from platformio.ini
get_env_from_platformio_ini() {
    local ini_file="$1"
    if [ -f "$ini_file" ]; then
        # Extract first [env:xxx] section name
        grep -m1 '^\[env:' "$ini_file" | sed 's/\[env:\(.*\)\]/\1/'
    fi
}

# Detect variant directory (check if we're in a variant folder with platformio.ini)
VARIANT_DIR=""
DETECTED_ENV=""

if [ -f "$CURRENT_DIR/platformio.ini" ] && [ "$CURRENT_DIR" != "$PROJECT_ROOT" ]; then
    # We're in a variant directory
    VARIANT_DIR="$CURRENT_DIR"
    DETECTED_ENV=$(get_env_from_platformio_ini "$VARIANT_DIR/platformio.ini")
    echo "=== Detected variant directory: $VARIANT_DIR ==="
    echo "=== Detected environment: $DETECTED_ENV ==="
fi

# Load global .env from project root
cd "$PROJECT_ROOT"
if ! load_env_file "$PROJECT_ROOT/.env" "global environment"; then
    echo "⚠️  WARNING: Global .env file not found at $PROJECT_ROOT/.env"
    echo "   Create from .env.example if you need API keys or other settings"
fi

# Load variant-specific .env if it exists (overrides global)
if [ -n "$VARIANT_DIR" ] && [ -f "$VARIANT_DIR/.env" ]; then
    echo ""
    load_env_file "$VARIANT_DIR/.env" "variant environment"
fi

# Convert EXCLUDE_ALERT_* env vars to build flags
EXTRA_BUILD_FLAGS=""
for var in EXCLUDE_ALERT_RCB EXCLUDE_ALERT_IMGW EXCLUDE_ALERT_POZ EXCLUDE_ALERT_SYNOP EXCLUDE_ALERT_AIWEATHER EXCLUDE_ALERT_INTERACTIVE; do
    val="${!var}"
    if [ "$val" = "1" ]; then
        EXTRA_BUILD_FLAGS="$EXTRA_BUILD_FLAGS -DMESHTASTIC_${var}=1"
        echo "  🚫 Excluding provider: $var"
    fi
done
if [ -n "$EXTRA_BUILD_FLAGS" ]; then
    export PLATFORMIO_BUILD_FLAGS="${PLATFORMIO_BUILD_FLAGS:-}$EXTRA_BUILD_FLAGS"
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

# Build the command arguments
# If environment was detected and not explicitly provided, add it
ARGS=("$@")
if [ -n "$DETECTED_ENV" ]; then
    # Check if -e was already provided
    HAS_ENV_FLAG=false
    for arg in "${ARGS[@]}"; do
        if [ "$arg" = "-e" ] || [ "$arg" = "--environment" ]; then
            HAS_ENV_FLAG=true
            break
        fi
    done

    if [ "$HAS_ENV_FLAG" = false ]; then
        # Insert -e <env> after the first argument (usually 'run')
        if [ ${#ARGS[@]} -gt 0 ]; then
            FIRST_ARG="${ARGS[0]}"
            ARGS=("$FIRST_ARG" "-e" "$DETECTED_ENV" "${ARGS[@]:1}")
        else
            ARGS=("-e" "$DETECTED_ENV")
        fi
    fi
fi

# Run PlatformIO command
echo ""
echo "=== Running: $PIO_CMD ${ARGS[*]} ==="
$PIO_CMD "${ARGS[@]}"
