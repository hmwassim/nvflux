#!/usr/bin/env bash
# uninstall.sh - Remove nvflux
set -euo pipefail

NVFLUX_BIN="/usr/local/bin/nvflux"
AUTOSTART_FILE="/etc/xdg/autostart/nvflux-restore.desktop"
STATE_DIR="/var/lib/nvflux"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

die() { echo -e "${RED}error:${NC} $*" >&2; exit 1; }
info() { echo -e "${BLUE}==>${NC} $*"; }
warn() { echo -e "${YELLOW}warning:${NC} $*" >&2; }
success() { echo -e "${GREEN}✓${NC} $*"; }

[ "$(id -u)" -eq 0 ] || die "must run as root (sudo $0)"

# ──────────────────────────────────────────────────────────────────────────────
# Reset GPU clocks before uninstalling (prevents locked clocks after removal)
# ──────────────────────────────────────────────────────────────────────────────
reset_clocks() {
    if [ -x "$NVFLUX_BIN" ]; then
        info "Resetting GPU clocks to driver-managed..."
        if "$NVFLUX_BIN" auto >/dev/null 2>&1; then
            success "GPU clocks unlocked (driver-managed)"
        else
            warn "Failed to reset clocks - may need manual reset"
        fi
    else
        die "nvflux not installed - nothing to uninstall"
    fi
}

# ──────────────────────────────────────────────────────────────────────────────
# Main uninstallation
# ──────────────────────────────────────────────────────────────────────────────

# Reset clocks first (before removing binary)
reset_clocks

info "Removing nvflux binary..."
if rm -f "$NVFLUX_BIN"; then
    success "Binary removed"
else
    warn "Failed to remove binary"
fi

info "Removing autostart..."
if rm -f "$AUTOSTART_FILE"; then
    success "Autostart removed"
else
    warn "Failed to remove autostart"
fi

info "Removing state directory..."
if rm -rf "$STATE_DIR"; then
    success "State directory removed"
else
    warn "Failed to remove state directory"
fi

# Remove backup if it exists
rm -f /usr/local/bin/nvflux.bak 2>/dev/null || true

echo ""
success "nvflux uninstalled"
