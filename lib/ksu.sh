#!/bin/bash
# ==============================================================================
# KernelSU Setup Module
# ==============================================================================

# Source common functions
MODULE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$MODULE_DIR/common.sh"

# Setup KernelSU
setup_kernelsu() {
    log "Setting up KernelSU..."

    cd "$KERNEL_SRC"

    # Clean previous KernelSU integration
    if [ -d "KernelSU" ]; then
        log "Removing existing KernelSU directory..."
        rm -rf "KernelSU"
    fi

    if [ -L "drivers/kernelsu" ]; then
        log "Removing existing kernelsu symlink..."
        rm -f "drivers/kernelsu"
    fi

    # Get latest KernelSU release tag first.
    log "Cloning KernelSU..."
    git clone https://github.com/tiann/KernelSU.git KernelSU

    cd "$KERNEL_SRC/KernelSU"

    git fetch --tags origin

    KSU_TAG=$(git tag --sort=-v:refname \
        | grep -E '^v[0-9]+(\.[0-9]+)*$' \
        | head -n1)

    if [ -z "$KSU_TAG" ]; then
        error "No KernelSU release tags found."
        exit 1
    fi

    log "Using latest KernelSU release: $KSU_TAG"

    git checkout --detach "$KSU_TAG"

    KSU_COMMIT=$(git rev-parse --short HEAD)
    log "KernelSU commit: $KSU_COMMIT"

    cd "$KERNEL_SRC"

    # ------------------------------------------------------------------
    # Integrate KernelSU into kernel tree
    # ------------------------------------------------------------------

    log "Creating KernelSU integration..."

    ln -sf \
        "$(realpath --relative-to="$KERNEL_SRC/drivers" "$KERNEL_SRC/KernelSU/kernel")" \
        "$KERNEL_SRC/drivers/kernelsu"

    # Add KernelSU to drivers/Makefile
    if ! grep -q 'obj-$(CONFIG_KSU) += kernelsu/' "$KERNEL_SRC/drivers/Makefile"; then
        printf '\nobj-$(CONFIG_KSU) += kernelsu/\n' \
            >> "$KERNEL_SRC/drivers/Makefile"
        log "Modified drivers/Makefile"
    fi

    # Add KernelSU Kconfig
    if ! grep -q 'source "drivers/kernelsu/Kconfig"' "$KERNEL_SRC/drivers/Kconfig"; then
        sed -i '/endmenu/i source "drivers/kernelsu/Kconfig"' \
            "$KERNEL_SRC/drivers/Kconfig"
        log "Modified drivers/Kconfig"
    fi

    # ------------------------------------------------------------------
    # KernelSU version
    # ------------------------------------------------------------------

    cd "$KERNEL_SRC/KernelSU"

    KSU_COMMIT_COUNT=$(git rev-list --count HEAD 2>/dev/null || echo "1")
    KSU_VERSION=$((KSU_COMMIT_COUNT + 30000))
    KSU_GIT_VERSION=$(git rev-parse --short HEAD 2>/dev/null || echo "unknown")

    log "KernelSU version: $KSU_VERSION"
    log "KernelSU git: $KSU_GIT_VERSION"

    if [ -f "kernel/Kbuild" ]; then
        sed -i \
            "s/ccflags-y += -DKSU_VERSION=.*/ccflags-y += -DKSU_VERSION=$KSU_VERSION/" \
            kernel/Kbuild

        if grep -q "DKSU_GIT_VERSION" kernel/Kbuild; then
            sed -i \
                "s/ccflags-y += -DKSU_GIT_VERSION=.*/ccflags-y += -DKSU_GIT_VERSION=\\\"$KSU_GIT_VERSION\\\"/" \
                kernel/Kbuild
        else
            printf 'ccflags-y += -DKSU_GIT_VERSION=\\"%s\\"\n' \
                "$KSU_GIT_VERSION" >> kernel/Kbuild
        fi
    fi

    cd "$KERNEL_SRC"

    # ------------------------------------------------------------------
    # Verify integration
    # ------------------------------------------------------------------

    if [ ! -L "$KERNEL_SRC/drivers/kernelsu" ]; then
        error "drivers/kernelsu symlink was not created!"
        exit 1
    fi

    if [ ! -f "$KERNEL_SRC/drivers/kernelsu/Kconfig" ]; then
        error "KernelSU Kconfig not found!"
        exit 1
    fi

    log "✓ KernelSU $KSU_TAG integrated successfully."
}