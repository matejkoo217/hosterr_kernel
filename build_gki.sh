#!/bin/bash
set -e

# ==============================================================================
# GKI Kernel Build Script for Xiaomi 13 (fuxi)
# Environment: Arch Linux / Android 13 / Kernel 5.15
# ==============================================================================

# Colors
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

WORKSPACE_DIR="../gki_build_workspace"
KERNEL_SRC="$WORKSPACE_DIR/common"
CLANG_DIR="$WORKSPACE_DIR/prebuilts/clang/host/linux-x86"
CLANG_VER="clang-r547379"
STAMP_BZL="$WORKSPACE_DIR/build/kernel/kleaf/impl/stamp.bzl"

log() { echo -e "${GREEN}[INFO] $1${NC}"; }
warn() { echo -e "${YELLOW}[WARN] $1${NC}"; }
error() { echo -e "${RED}[ERROR] $1${NC}"; }

# 1. Dependency Check (Arch Linux)
log "Checking Dependencies..."
if [ -f /etc/arch-release ] && ! command -v repo &> /dev/null; then
    log "Installing dependencies..."
    sudo pacman -S --needed --noconfirm base-devel git repo zip unzip curl wget python bc rsync schedtool cpio ncurses libxml2 xmlto inetutils
fi

# 2. Workspace Setup
log "Setting up workspace..."
mkdir -p "$WORKSPACE_DIR"
cd "$WORKSPACE_DIR"

if [ ! -d ".repo" ]; then
    log "Initializing Repo..."
    repo init -u https://android.googlesource.com/kernel/manifest -b common-android13-5.15
    mkdir -p .repo/local_manifests
    # Skip downloading kernel source (using local link)
    cat > .repo/local_manifests/local.xml <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<manifest>
    <remove-project name="kernel/common" />
</manifest>
EOF
    log "Syncing repo (this may take a while)..."
    repo sync -c -j$(nproc) --no-tags
fi

# 3. Source Linking
log "Linking Kernel Source..."
if [ -L "common" ]; then rm "common"; fi
if [ -d "common" ]; then mv common "common_backup_$(date +%s)"; fi
ln -s "$(realpath ../android_gki_kernel_5.15_common)" common

# 4. Fix: Ensure Clang Toolchain Exists (Manual Fetch if Repo Sync failed)
if [ ! -d "$CLANG_DIR/$CLANG_VER" ]; then
    warn "Clang $CLANG_VER not found. Fetching manually..."
    mkdir -p "$CLANG_DIR" && cd "$CLANG_DIR"
    git init
    git remote add origin "https://android.googlesource.com/platform/prebuilts/clang/host/linux-x86"
    git fetch --depth 1 origin master
    git checkout FETCH_HEAD -- "$CLANG_VER"
    cd "$WORKSPACE_DIR"
fi

# 5. Fix: ZRAM Configuration (Build as Module)
log "Applying ZRAM Config Fix..."
GKI_DEFCONFIG="$KERNEL_SRC/arch/arm64/configs/gki_defconfig"
if grep -q "CONFIG_ZRAM=y" "$GKI_DEFCONFIG"; then
    sed -i 's/CONFIG_ZRAM=y/CONFIG_ZRAM=m/' "$GKI_DEFCONFIG"
    sed -i 's/CONFIG_ZSMALLOC=y/CONFIG_ZSMALLOC=m/' "$GKI_DEFCONFIG"
    log "Converted ZRAM to module."
fi

MODULES_LIST="$KERNEL_SRC/android/gki_system_dlkm_modules"
if ! grep -q "drivers/block/zram/zram.ko" "$MODULES_LIST"; then
    echo "drivers/block/zram/zram.ko" >> "$MODULES_LIST"
    echo "mm/zsmalloc.ko" >> "$MODULES_LIST"
fi

# 6. Fix: Export 'task_is_booster' for ZRAM Module
CPUSET_C="$KERNEL_SRC/kernel/cgroup/cpuset.c"
if ! grep -q "EXPORT_SYMBOL_GPL(task_is_booster)" "$CPUSET_C"; then
    log "Exporting task_is_booster..."
    echo "" >> "$CPUSET_C"
    echo "EXPORT_SYMBOL_GPL(task_is_booster);" >> "$CPUSET_C"
fi

# 7. Fix: Add symbol to KMI Allowlist (Strict Mode)
SYMBOL_LIST="$KERNEL_SRC/android/abi_gki_aarch64"
if ! grep -q "task_is_booster" "$SYMBOL_LIST"; then
    log "Updating KMI Symbol List..."
    echo "task_is_booster" >> "$SYMBOL_LIST"
fi

# ==============================================================================
# VERSION CUSTOMIZATION
# ==============================================================================
BUILD_DATE=$(date +%Y%m%d)
CUSTOM_VERSION="-serein-android13-$BUILD_DATE"

log "Customizing Kernel Version to: $CUSTOM_VERSION"

# 8. Update CONFIG_LOCALVERSION in gki_defconfig
# Replace existing CONFIG_LOCALVERSION="..." with our custom version
if grep -q "CONFIG_LOCALVERSION=" "$GKI_DEFCONFIG"; then
    sed -i 's/CONFIG_LOCALVERSION=".*"/CONFIG_LOCALVERSION="'"$CUSTOM_VERSION"'"/' "$GKI_DEFCONFIG"
else
    echo "CONFIG_LOCALVERSION=\"$CUSTOM_VERSION\"" >> "$GKI_DEFCONFIG"
fi

# 9. Remove '-maybe-dirty' suffix by hacking stamp.bzl
# The Bazel build system (Kleaf) forces LOCALVERSION="-maybe-dirty" for non-stamped builds.
# We change it to empty string so it doesn't append anything, and lets CONFIG_LOCALVERSION take precedence.
if [ -f "$STAMP_BZL" ]; then
    log "Patching stamp.bzl to remove -maybe-dirty..."
    sed -i 's/export LOCALVERSION="-maybe-dirty"/export LOCALVERSION=""/' "$STAMP_BZL"
else
    warn "stamp.bzl not found at $STAMP_BZL. Version might still include -maybe-dirty."
fi

# ==============================================================================

# 10. Build
log "Starting Bazel Build..."
tools/bazel build //common:kernel_aarch64_dist

log "Build Complete!"
IMAGE_PATH=$(find out/ -name Image | head -n 1)
echo "Kernel Image: $IMAGE_PATH"

# Verify version
log "Verifying Kernel Version..."
if [ -f "$IMAGE_PATH" ]; then
    strings "$IMAGE_PATH" | grep "Linux version" | head -n 1
fi
