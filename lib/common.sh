#!/bin/bash
# ==============================================================================
# Common Configuration and Utility Functions
# ==============================================================================
# Provides log/warn/error helpers for the build scripts (ksu.sh, etc).
# NOTE: does NOT set KERNEL_SRC/CLANG_DIR — those are defined by run.sh so a
# module sourcing this file can't clobber them.

# Colors
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

# Logging functions
log()   { echo -e "${GREEN}[INFO]  $1${NC}"; }
warn()  { echo -e "${YELLOW}[WARN]  $1${NC}"; }
error() { echo -e "${RED}[ERROR] $1${NC}"; }

# Script directory helper
get_script_dir() {
    echo "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
}