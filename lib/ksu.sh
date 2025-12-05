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
    
    # Clean up any previous failed installation
    if [ -d "KernelSU" ]; then
        log "Removing existing KernelSU directory..."
        rm -rf "KernelSU"
    fi
    if [ -L "drivers/kernelsu" ]; then
        log "Removing existing kernelsu symlink..."
        rm -f "drivers/kernelsu"
    fi
    
    # Run the official setup script WITHOUT arguments
    # This will checkout the latest tag (stable release)
    # Passing 'main' can cause issues if repo is already on main branch
    log "Running official KernelSU setup script..."
    curl -LSs "https://raw.githubusercontent.com/tiann/KernelSU/main/kernel/setup.sh" | bash -s
    
    # Switch to latest main branch instead of tag
    if [ -d "KernelSU" ]; then
        log "Switching KernelSU to latest main branch..."
        cd KernelSU
        git fetch origin main
        git checkout origin/main
        CURRENT_COMMIT=$(git rev-parse --short HEAD)
        log "✓ KernelSU now at main branch: $CURRENT_COMMIT"
        cd ..
    fi
    
    # Verify installation
    if [ -d "KernelSU" ] && [ -d "KernelSU/kernel" ]; then
        log "KernelSU directory found."
        
        # Fix: Hardcode KernelSU version for Bazel build
        # Bazel runs in a sandbox and cannot access .git directory to determine version
        cd KernelSU
        
        # Get version from git
        if git rev-parse --is-inside-work-tree > /dev/null 2>&1; then
            # Fetch full history for accurate version calculation
            git fetch --unshallow 2>/dev/null || true
            
            # Calculate version code (Integer) for KernelSU
            # KernelSU versioning: 30000 + commit count
            COMMIT_COUNT=$(git rev-list --count HEAD 2>/dev/null || echo "1")
            KSU_VERSION=$((COMMIT_COUNT + 30000))
            KSU_GIT_VERSION=$(git rev-parse --short HEAD 2>/dev/null || echo "unknown")
        else
            # Fallback if not a git repo
            KSU_VERSION=30000
            KSU_GIT_VERSION="unknown"
        fi
        
        log "Injecting KernelSU Version: $KSU_VERSION, Git Version: $KSU_GIT_VERSION"
        
        # Modify kernel/Kbuild to hardcode the version
        if [ -f "kernel/Kbuild" ]; then
            # Replace the fallback version with our calculated version
            sed -i "s/ccflags-y += -DKSU_VERSION=.*/ccflags-y += -DKSU_VERSION=$KSU_VERSION/" kernel/Kbuild
            
            # Also inject KSU_GIT_VERSION if not present or replace it
            if grep -q "DKSU_GIT_VERSION" kernel/Kbuild; then
                sed -i "s/ccflags-y += -DKSU_GIT_VERSION=.*/ccflags-y += -DKSU_GIT_VERSION=\\\\\"$KSU_GIT_VERSION\\\\\"/" kernel/Kbuild
            else
                echo "ccflags-y += -DKSU_GIT_VERSION=\\\"$KSU_GIT_VERSION\\\"" >> kernel/Kbuild
            fi

            # Debug: Show the grep result to verify
            log "Verifying KernelSU/kernel/Kbuild patch:"
            grep "DKSU_VERSION=$KSU_VERSION" kernel/Kbuild || warn "Patch might have failed!"
            
            log "✓ Patched KernelSU/kernel/Kbuild with version"
        else
            warn "KernelSU/kernel/Kbuild not found. Version might be incorrect."
        fi

        # Modify Kbuild (root) to hardcode the git version
        if [ -f "Kbuild" ]; then
             # Remove the check for KSU_GIT_VERSION if it exists (it causes the error)
             # The error "KSU_GIT_VERSION not defined" usually comes from a check like:
             # ifndef KSU_GIT_VERSION
             # $(error ...)
             # endif
             # We can try to comment out the error or define the variable.
             
             # Inject the variable at the top
             sed -i "1iKSU_GIT_VERSION := $KSU_GIT_VERSION" Kbuild
             sed -i "1iKSU_VERSION := $KSU_VERSION" Kbuild
             
             # Also try to replace ccflags if they exist
             if grep -q "DKSU_GIT_VERSION" Kbuild; then
                sed -i "s/ccflags-y += -DKSU_GIT_VERSION=.*/ccflags-y += -DKSU_GIT_VERSION=\\\\\"$KSU_GIT_VERSION\\\\\"/" Kbuild
             else
                echo "ccflags-y += -DKSU_GIT_VERSION=\\\"$KSU_GIT_VERSION\\\"" >> Kbuild
             fi
             
             log "✓ Patched KernelSU/Kbuild with version"
        fi
        
        cd ..
        
        # Verify the symlink and Kconfig exist
        if [ ! -L "drivers/kernelsu" ]; then
            error "drivers/kernelsu symlink not created!"
            exit 1
        fi
        if [ ! -f "drivers/kernelsu/Kconfig" ]; then
            error "drivers/kernelsu/Kconfig not found!"
            ls -la drivers/kernelsu/ 2>/dev/null || true
            exit 1
        fi
        
        log "✓ KernelSU setup complete."
    else
        error "KernelSU setup failed - directory or kernel subdirectory not found."
        ls -la KernelSU/ 2>/dev/null || true
        exit 1
    fi
}
