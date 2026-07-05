#!/usr/bin/env bash
# install.sh - Build and install nvflux
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
INSTALL_BIN="/usr/local/bin/nvflux"
STATE_DIR="/var/lib/nvflux"
AUTOSTART_FILE="/etc/xdg/autostart/nvflux-restore.desktop"
BACKUP_BIN="/usr/local/bin/nvflux.bak"
FORCE=0
if [ "${1:-}" = "--force" ]; then FORCE=1; fi

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

# Trap for cleanup on error
cleanup() {
    local exit_code=$?
    if [ $exit_code -ne 0 ]; then
        warn "Installation failed (exit code: $exit_code)"
        if [ -f "$BACKUP_BIN" ]; then
            warn "Restoring previous version..."
            mv -f "$BACKUP_BIN" "$INSTALL_BIN" 2>/dev/null || true
        fi
    fi
}
trap cleanup EXIT

[ "$(id -u)" -eq 0 ] || die "must run as root (sudo $0)"

# ──────────────────────────────────────────────────────────────────────────────
# Pre-installation checks
# ──────────────────────────────────────────────────────────────────────────────
pre_checks() {
    info "Running pre-installation checks..."

    # Check disk space (need at least 10MB)
    local available_kb
    available_kb=$(df -k /usr/local 2>/dev/null | awk 'NR==2 {print $4}')
    if [ -n "$available_kb" ] && [ "$available_kb" -lt 10240 ]; then
        die "Insufficient disk space (need at least 10MB)"
    fi

    # Check if /usr/local/bin is writable
    if [ ! -d "/usr/local/bin" ]; then
        mkdir -p /usr/local/bin || die "Failed to create /usr/local/bin"
    fi

    # Check for conflicting installations
    if [ -f "$INSTALL_BIN" ] && [ ! -L "$INSTALL_BIN" ]; then
        info "Found existing installation at $INSTALL_BIN"
        if [ -x "$INSTALL_BIN" ]; then
            local existing_ver
            existing_ver=$("$INSTALL_BIN" --version 2>/dev/null || echo "unknown")
            info "Existing version: $existing_ver"
        fi
    fi

    # Check state directory permissions
    if [ -d "$STATE_DIR" ]; then
        if [ ! -w "$STATE_DIR" ]; then
            warn "State directory $STATE_DIR is not writable"
        fi
    fi

    success "Pre-installation checks passed"
}

# ──────────────────────────────────────────────────────────────────────────────
# Check if NVIDIA drivers are installed (required)
# ──────────────────────────────────────────────────────────────────────────────
check_nvidia() {
    info "Checking for NVIDIA drivers..."

    # Search common paths for nvidia-smi
    if command -v nvidia-smi >/dev/null 2>&1; then
        local nvsmi_path
        nvsmi_path="$(command -v nvidia-smi)"
        success "Found nvidia-smi at: $nvsmi_path"
        
        # Verify it actually works
        if ! nvidia-smi --query-gpu=name --format=csv,noheader >/dev/null 2>&1; then
            warn "nvidia-smi found but not responding - drivers may be broken"
        fi
        return 0
    fi

    for path in /usr/bin/nvidia-smi /usr/local/bin/nvidia-smi; do
        if [ -x "$path" ]; then
            success "Found nvidia-smi at: $path"
            return 0
        fi
    done

    # Not found - prompt user
    echo ""
    echo -e "${RED}⚠ NVIDIA drivers not detected!${NC}"
    echo ""
    echo "Please install NVIDIA drivers for your distribution first,"
    echo "then run: sudo $0"
    echo ""
    echo "Installation instructions: https://www.nvidia.com/object/unix.html"
    echo ""
    die "Aborting: NVIDIA drivers required"
}

# ──────────────────────────────────────────────────────────────────────────────
# Detect and install build dependencies (works on any distro)
# ──────────────────────────────────────────────────────────────────────────────
install_deps() {
    info "Checking build dependencies..."

    # Check if compiler and make are already installed
    if command -v gcc >/dev/null 2>&1 && command -v make >/dev/null 2>&1; then
        info "Build tools already installed"
        return 0
    fi

    # Detect package manager and install dependencies
    if command -v apt-get >/dev/null 2>&1; then
        # Debian, Ubuntu, Linux Mint, Pop!_OS, Kali
        info "Detected APT (Debian/Ubuntu family)"
        apt-get update -qq
        apt-get install -y -qq build-essential || die "Failed to install dependencies"
    elif command -v dnf >/dev/null 2>&1; then
        # Fedora, RHEL 8+, CentOS 8+
        info "Detected DNF (Fedora/RHEL family)"
        dnf install @development-tools >/dev/null || \
        dnf install -y gcc gcc-c++ make >/dev/null || die "Failed to install dependencies"
    elif command -v yum >/dev/null 2>&1; then
        # CentOS 7, older RHEL
        info "Detected YUM (CentOS/RHEL)"
        yum groupinstall -y "Development Tools" >/dev/null || \
        yum install -y gcc gcc-c++ make >/dev/null || die "Failed to install dependencies"
    elif command -v pacman >/dev/null 2>&1; then
        # Arch Linux, Manjaro, EndeavourOS
        info "Detected Pacman (Arch family)"
        pacman -Sy --noconfirm base-devel >/dev/null || die "Failed to install dependencies"
    elif command -v zypper >/dev/null 2>&1; then
        # openSUSE
        info "Detected Zypper (openSUSE)"
        zypper install -y -t pattern devel_basis >/dev/null || \
        zypper install -y -q gcc gcc-c++ make >/dev/null || die "Failed to install dependencies"
    elif command -v xbps-install >/dev/null 2>&1; then
        # Void Linux
        info "Detected XBPS (Void Linux)"
        xbps-install -Sy base-devel >/dev/null || die "Failed to install dependencies"
    elif command -v emerge >/dev/null 2>&1; then
        # Gentoo / Funtoo
        info "Detected Portage (Gentoo/Funtoo)"
        emerge --quiet sys-devel/gcc sys-devel/make sys-devel/binutils || die "Failed to install dependencies"
    elif command -v apk >/dev/null 2>&1; then
        # Alpine Linux
        info "Detected APK (Alpine Linux)"
        apk add --quiet build-base >/dev/null || die "Failed to install dependencies"
    elif command -v nix-env >/dev/null 2>&1; then
        # NixOS
        info "Detected Nix (NixOS)"
        nix-env -iA nixpkgs.gcc nixpkgs.gnumake nixpkgs.binutils >/dev/null || die "Failed to install dependencies"
    elif command -v eopkg >/dev/null 2>&1; then
        # Solus
        info "Detected Eopkg (Solus)"
        eopkg install -y -c system.devel >/dev/null || die "Failed to install dependencies"
    elif command -v pisi >/dev/null 2>&1; then
        # Pardus (old, uses pisi)
        info "Detected PISI (Pardus)"
        pisi install -y development-tools >/dev/null || die "Failed to install dependencies"
    else
        info "Unknown package manager - please install gcc and make manually"
        info "Continuing anyway (build may fail if tools are missing)..."
        return 0
    fi

    info "Dependencies installed"
}

# ──────────────────────────────────────────────────────────────────────────────
# Backup existing installation
# ──────────────────────────────────────────────────────────────────────────────
backup_existing() {
    if [ -f "$INSTALL_BIN" ] && [ ! -L "$INSTALL_BIN" ]; then
        info "Backing up existing installation..."
        cp -f "$INSTALL_BIN" "$BACKUP_BIN" || {
            warn "Failed to create backup"
            return 1
        }
        success "Backup created at $BACKUP_BIN"
    fi
}

# ──────────────────────────────────────────────────────────────────────────────
# Main installation
# ──────────────────────────────────────────────────────────────────────────────

# Run pre-installation checks
pre_checks

# Check for NVIDIA drivers first (will abort if not found)
check_nvidia

# Skip if nvflux is already installed and functional (unless --force)
if [ "$FORCE" -eq 0 ] && [ -x "$INSTALL_BIN" ] && "$INSTALL_BIN" --version >/dev/null 2>&1; then
    success "nvflux $("$INSTALL_BIN" --version) already installed at $INSTALL_BIN"
    success "Re-run with --force to rebuild and reinstall"
    exit 0
fi

# Install build dependencies
install_deps

# Backup existing installation before making changes
backup_existing || true

info "Building nvflux..."
if ! make -C "$SCRIPT_DIR" clean >/dev/null 2>&1; then
    warn "Clean failed, continuing anyway"
fi

if ! make -C "$SCRIPT_DIR"; then
    die "Build failed"
fi
success "Build completed"

info "Installing to $INSTALL_BIN..."
if ! make -C "$SCRIPT_DIR" install; then
    die "Installation failed"
fi
success "Binary installed"

# Remove backup if installation succeeded
if [ -f "$BACKUP_BIN" ]; then
    rm -f "$BACKUP_BIN"
fi

info "Cleaning up build artifacts..."
make -C "$SCRIPT_DIR" clean >/dev/null 2>&1 || true

# ──────────────────────────────────────────────────────────────────────────────
# Verify installation
# ──────────────────────────────────────────────────────────────────────────────
verify_installation() {
    info "Verifying installation..."
    
    # Check binary exists and is executable
    if [ ! -x "$INSTALL_BIN" ]; then
        die "Installation verification failed: binary not executable"
    fi
    
    # Check setuid bit
    if [ ! -u "$INSTALL_BIN" ]; then
        warn "setuid bit not set - some features may require root"
    fi
    
    # Test version command
    if ! "$INSTALL_BIN" --version >/dev/null 2>&1; then
        die "Installation verification failed: binary not working"
    fi
    
    # Test help command (requires nvidia-smi)
    if ! "$INSTALL_BIN" status >/dev/null 2>&1; then
        warn "Status command failed - nvidia-smi may not be accessible"
    fi
    
    success "Installation verified successfully"
}

verify_installation

echo ""
success "nvflux $("$INSTALL_BIN" --version) installed successfully!"
echo ""
echo "Usage:"
echo "  nvflux powersave     # Lock memory (audio fix)"
echo "  nvflux balanced      # Mid tier"
echo "  nvflux performance   # Highest tier"
echo "  nvflux ultra         # Max clocks"
echo "  nvflux auto          # Unlock (driver-managed)"
echo "  nvflux status        # Show profile"
echo "  nvflux clock         # Show memory clock"
echo ""
echo "Autostart is enabled. Profile will be restored on login."
