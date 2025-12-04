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
    
    # Run the official setup script
    # This script handles cloning and patching
    # Explicitly use 'main' branch to get the latest features
    curl -LSs "https://raw.githubusercontent.com/tiann/KernelSU/main/kernel/setup.sh" | bash -s main
    
    # Verify installation
    if [ -d "KernelSU" ]; then
        log "KernelSU directory found."
        
        # Fix: Hardcode KernelSU version for Bazel build
        # Bazel runs in a sandbox and cannot access .git directory to determine version
        cd KernelSU
        
        # Get version from git
        if git rev-parse --is-inside-work-tree > /dev/null 2>&1; then
            # Calculate version code (Integer) for KernelSU
            # Standard KernelSU versioning: 10000 + commit count for main branch
            COMMIT_COUNT=$(git rev-list --count HEAD)
            KSU_VERSION=$((COMMIT_COUNT + 30000))
        else
            # Fallback if not a git repo
            KSU_VERSION=30000
        fi
        
        log "Injecting KernelSU Version: $KSU_VERSION"
        
        # Modify kernel/Makefile to hardcode the version
        # Replace the dynamic git command with the static integer
        if [ -f "kernel/Makefile" ]; then
            # Remove existing KERNEL_SU_VERSION definition to avoid duplicates/conflicts
            sed -i '/-DKERNEL_SU_VERSION/d' kernel/Makefile
            
            # Append the hardcoded version (Integer, NO QUOTES)
            # We use += to add to existing flags
            echo "ccflags-y += -DKERNEL_SU_VERSION=$KSU_VERSION" >> kernel/Makefile
            log "✓ Patched KernelSU/kernel/Makefile with version"
        else
            warn "KernelSU/kernel/Makefile not found. Version might be incorrect."
        fi
        
        cd ..
        log "✓ KernelSU setup complete."
    else
        error "KernelSU setup failed."
        exit 1
    fi
}
