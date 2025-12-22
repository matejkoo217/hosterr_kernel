#!/bin/bash
# ==============================================================================
# Workspace Setup Module
# ==============================================================================

# Source common functions
MODULE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$MODULE_DIR/common.sh"

# Setup workspace
setup_workspace() {
    log "Setting up workspace..."
    mkdir -p "$WORKSPACE_DIR"
    cd "$WORKSPACE_DIR"

    # 检测是否在 GitHub Actions 环境中（云构建）
    if [ -n "${GITHUB_ACTIONS}" ]; then
        # 云构建使用官方源地址
        log "Detected GitHub Actions environment, using official sources..."
        REPO_MANIFEST_URL="${REPO_MANIFEST_URL:-https://android.googlesource.com/kernel/manifest}"
        REPO_TOOL_URL="${REPO_TOOL_URL:-https://gerrit.googlesource.com/git-repo}"
        CLANG_FETCH_URL="${CLANG_FETCH_URL:-https://android.googlesource.com/platform/prebuilts/clang/host/linux-x86.git}"
    else
        # 本地构建使用镜像源
        log "Local build detected, using mirror sources..."
        REPO_MANIFEST_URL="${REPO_MANIFEST_URL:-https://mirrors.ustc.edu.cn/aosp/kernel/manifest}"
        REPO_TOOL_URL="${REPO_TOOL_URL:-https://mirrors.ustc.edu.cn/aosp/git-repo}"
        CLANG_FETCH_URL="${CLANG_FETCH_URL:-https://mirrors.ustc.edu.cn/aosp/platform/prebuilts/clang/host/linux-x86.git}"
    fi
    
    REPO_BRANCH="${REPO_BRANCH:-common-android13-5.15}"
    REPO_TOOL_BRANCH="${REPO_TOOL_BRANCH:-stable}"

    # Helper to run repo
    run_repo() {
        if [ -x ".repo/repo/repo" ]; then
            .repo/repo/repo "$@"
        else
            repo "$@"
        fi
    }
    
    # 检查是否有缓存（.repo 目录且有 manifest）
    if [ -d ".repo" ] && [ -f ".repo/manifests/default.xml" ]; then
        if [ -n "${GITHUB_ACTIONS}" ]; then
            # 云构建：进行完整性检查
            log "Found cached repo, checking integrity..."
            if [ -d "build/kernel" ] && [ -d "tools/bazel" ]; then
                log "Cache appears valid, skipping repo init/sync"
            else
                log "Cache incomplete, re-initializing repo and running sync..."
                # Clean up potential conflicting directories from manual fetch
                if [ -d "prebuilts/clang/host/linux-x86/.git" ] && [ ! -f "prebuilts/clang/host/linux-x86/.git" ]; then
                    log "Removing conflicting git repo in prebuilts/clang/host/linux-x86..."
                    rm -rf "prebuilts/clang/host/linux-x86"
                fi
                run_repo init -c --repo-url "$REPO_TOOL_URL" --repo-branch "$REPO_TOOL_BRANCH" -u "$REPO_MANIFEST_URL" -b "$REPO_BRANCH"
                log "Starting repo sync (this may take a while)..."
                # 检查是否有正在运行的 repo sync 进程
                if pgrep -f "repo.*sync" > /dev/null; then
                    warn "Another repo sync process is running. Waiting for it to complete..."
                    while pgrep -f "repo.*sync" > /dev/null; do
                        sleep 2
                        echo -n "."
                    done
                    echo ""
                    log "Previous repo sync completed."
                else
                    # 使用详细模式显示进度，避免看起来卡住
                    run_repo sync -c -j$(nproc) --no-tags --force-sync -v || {
                        error "Repo sync failed. Please check the output above for details."
                        exit 1
                    }
                fi
            fi
        else
            # 本地构建：直接使用现有缓存，不检查完整性，不重新同步
            log "Found cached repo, using existing cache (local build - skipping sync)"
        fi
    elif [ ! -d ".repo" ] || [ ! -f ".repo/manifests/default.xml" ]; then
        log "Initializing Repo..."
        run_repo init -c --repo-url "$REPO_TOOL_URL" --repo-branch "$REPO_TOOL_BRANCH" -u "$REPO_MANIFEST_URL" -b "$REPO_BRANCH"
        mkdir -p .repo/local_manifests
        # Skip downloading kernel source (using local link)
        cat > .repo/local_manifests/local.xml <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<manifest>
    <remove-project name="kernel/common" />
</manifest>
EOF
        log "Syncing repo (this may take a while)..."
        # Clean up potential conflicting directories
        if [ -d "prebuilts/clang/host/linux-x86/.git" ] && [ ! -f "prebuilts/clang/host/linux-x86/.git" ]; then
             rm -rf "prebuilts/clang/host/linux-x86"
        fi
        # 检查是否有正在运行的 repo sync 进程
        if pgrep -f "repo.*sync" > /dev/null; then
            warn "Another repo sync process is running. Waiting for it to complete..."
            while pgrep -f "repo.*sync" > /dev/null; do
                sleep 2
                echo -n "."
            done
            echo ""
            log "Previous repo sync completed."
        else
            # 使用详细模式显示进度，避免看起来卡住
            run_repo sync -c -j$(nproc) --no-tags -v || {
                error "Repo sync failed. Please check the output above for details."
                exit 1
            }
        fi
    fi
    
    # Source Linking
    log "Linking Kernel Source..."
    if [ -L "common" ]; then rm "common"; fi
    if [ -d "common" ]; then mv common "common_backup_$(date +%s)"; fi
    ln -s "$REPO_ROOT" common
    
    # Remove build artifacts from source tree to avoid "source tree is not clean" error
    # The Makefile's outputmakefile target checks if .config exists in srctree during out-of-tree builds
    # Bazel will generate .config from gki_defconfig automatically, so we can safely remove it
    SOURCE_DIR="$REPO_ROOT"
    
    # [优化] 更彻底的清理，确保 Bazel 不会因为 "source tree is not clean" 报错
    log "Sanitizing source tree for Bazel..."
    rm -f "$SOURCE_DIR/.config" "$SOURCE_DIR/.config.old" "$SOURCE_DIR/.kernelvariables"
    rm -rf "$SOURCE_DIR/include/generated"
    rm -rf "$SOURCE_DIR/include/config"
    rm -rf "$SOURCE_DIR/arch"/*/include/generated
    
    # 新增：清理可能残留的 Kbuild 文件或 modules.order
    find "$SOURCE_DIR" -name "*.o" -delete
    find "$SOURCE_DIR" -name "*.cmd" -delete
    find "$SOURCE_DIR" -name "modules.order" -delete
    
    # Fix: Ensure Clang Toolchain Exists (Manual Fetch if Repo Sync failed)
    # CLANG_FETCH_URL 已在上面根据环境设置（本地镜像或官方源）
    CLANG_FETCH_REF="${CLANG_FETCH_REF:-master}"
    if [ -d "$CLANG_DIR/$CLANG_VER" ] && [ -f "$CLANG_DIR/$CLANG_VER/bin/clang" ]; then
        log "Clang $CLANG_VER found (cached or from repo sync)"
    else
        warn "Clang $CLANG_VER not found. Fetching manually..."
        mkdir -p "$CLANG_DIR" && cd "$CLANG_DIR"
        # 清理可能存在的不完整目录
        rm -rf "$CLANG_VER"
        
        # Use existing git repo if available, otherwise init
        if [ ! -d ".git" ] && [ ! -f ".git" ]; then
            git init
        fi
        
        # Fetch directly from URL to avoid remote conflicts
        log "Fetching $CLANG_VER from: $CLANG_FETCH_URL (ref: $CLANG_FETCH_REF)"
        git remote remove origin 2>/dev/null || true
        git remote add origin "$CLANG_FETCH_URL"
        git fetch --depth 1 --no-tags origin "$CLANG_FETCH_REF"
        git checkout FETCH_HEAD -- "$CLANG_VER"
        cd "$WORKSPACE_DIR"
    fi
}

# Check dependencies
check_dependencies() {
    log "Checking Dependencies..."
    if [ -f /etc/arch-release ] && ! command -v repo &> /dev/null; then
        log "Installing dependencies..."
        sudo pacman -S --needed --noconfirm base-devel git repo zip unzip curl wget python bc rsync schedtool cpio ncurses libxml2 xmlto inetutils
    fi
}

