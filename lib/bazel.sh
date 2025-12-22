#!/bin/bash
# ==============================================================================
# Bazel Management Module
# ==============================================================================

# Source common functions
MODULE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$MODULE_DIR/common.sh"

# Clean bazel cache to ensure fresh build
clean_bazel_cache() {
    if [ -d "$WORKSPACE_DIR" ]; then
        log "Cleaning Bazel cache to fix timestamp issue..."
        cd "$WORKSPACE_DIR"
        
        # Check if bazel binary exists
        if [ ! -f "tools/bazel" ]; then
            warn "Bazel binary not found, skipping clean."
            return
        fi
        
        # Using --expunge to remove all files
        tools/bazel clean --expunge || warn "Bazel clean failed. Continuing..."
        
        # Also remove the disk cache directory
        local bazel_cache_dir="$HOME/.bazel_cache"
        if [ -d "$bazel_cache_dir" ]; then
            log "Removing Bazel disk cache directory: $bazel_cache_dir"
            rm -rf "$bazel_cache_dir"
        fi

        log "✓ Bazel cache cleaned."
        cd "$REPO_ROOT" # Go back to the original root
    else
        warn "Workspace directory not found, skipping Bazel cache clean."
    fi
}
