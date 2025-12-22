#!/bin/bash
# ==============================================================================
# Packaging Module
# ==============================================================================

# Source common functions
MODULE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$MODULE_DIR/common.sh"

# Find the built kernel Image
# ✅ UPDATED: 优先从 dist 目录查找，这是 Bazel run --dist_dir 的标准输出位置
find_kernel_image() {
    # Get script directory to find dist output
    local script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
    local dist_dir="$script_dir/out/dist"
    
    # First, try dist directory (Bazel run --dist_dir output)
    local image_path=""
    if [ -f "$dist_dir/Image" ]; then
        image_path="$dist_dir/Image"
    elif [ -f "$dist_dir/Image.gz" ]; then
        image_path="$dist_dir/Image.gz"
    fi
    
    # Fallback: Try workspace out directory (legacy support)
    if [ -z "$image_path" ]; then
        image_path=$(find "$WORKSPACE_DIR/out" -name Image 2>/dev/null | head -n 1)
    fi
    
    # Fallback: Try alternative locations
    if [ -z "$image_path" ]; then
        image_path=$(find "$WORKSPACE_DIR" -name Image -path "*/out/*" 2>/dev/null | head -n 1)
    fi
    
    if [ -z "$image_path" ]; then
        error "Could not find built Kernel Image"
        error "Expected locations:"
        error "  - $dist_dir/Image"
        error "  - $dist_dir/Image.gz"
        error "  - $WORKSPACE_DIR/out/Image"
        error "Please run the build first or ensure the build completed successfully."
        exit 1
    fi
    
    echo "$image_path"
}

# Pack Image.gz and boot.img
pack_image_artifacts() {
    log "Packing Image.gz and boot.img..."
    
    # Find kernel image
    IMAGE_PATH=$(find_kernel_image)
    echo "Kernel Image: $IMAGE_PATH"
    
    # Get kernel source root directory (parent of lib directory)
    KERNEL_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
    OUT_DIR="$KERNEL_ROOT/out"
    
    # Create output directory
    mkdir -p "$OUT_DIR"
    log "Output directory: $OUT_DIR"
    
    # 1. Generate Image.gz
    log "Creating Image.gz..."
    cp "$IMAGE_PATH" "$OUT_DIR/Image"
    gzip -f "$OUT_DIR/Image"
    log "✓ Image.gz created: $OUT_DIR/Image.gz"
    
    # 2. Generate boot.img
    log "Creating boot.img..."
    BOOT_IMG="$OUT_DIR/boot.img"
    
    # Check if mkbootimg is available
    if [ -f "$KERNEL_ROOT/tools/mkbootimg/mkbootimg.py" ]; then
        log "Using local mkbootimg from tools..."
        export PYTHONPATH="$KERNEL_ROOT/tools/mkbootimg:$PYTHONPATH"
        MKBOOTIMG_CMD="python3 $KERNEL_ROOT/tools/mkbootimg/mkbootimg.py"
    elif ! command -v mkbootimg &> /dev/null; then
        warn "mkbootimg not found. Trying to use from Android build tools..."
        # Try to find mkbootimg in common Android locations
        if [ -f "$WORKSPACE_DIR/prebuilts/misc/linux-x86/libufdt/mkbootimg.py" ]; then
            MKBOOTIMG_CMD="python3 $WORKSPACE_DIR/prebuilts/misc/linux-x86/libufdt/mkbootimg.py"
        else
            error "mkbootimg not found. Please install Android build tools or set up mkbootimg."
            exit 1
        fi
    else
        MKBOOTIMG_CMD="mkbootimg"
    fi
    
    # Extract cmdline from defconfig if available
    # Note: For GKI, cmdline is usually provided by bootloader/vendor_boot
    # So we should use empty cmdline or only minimal parameters
    CMDLINE=""
    DEFCONFIG="$KERNEL_ROOT/arch/arm64/configs/gki_defconfig"
    CMDLINE_EXTEND=""
    
    if [ -f "$DEFCONFIG" ]; then
        # Check if CMDLINE_EXTEND is enabled
        if grep -q "^CONFIG_CMDLINE_EXTEND=y" "$DEFCONFIG"; then
            CMDLINE_EXTEND="y"
            log "CONFIG_CMDLINE_EXTEND=y detected - cmdline will be appended by bootloader"
        fi
        
        # Extract CONFIG_CMDLINE value (remove quotes and CONFIG_CMDLINE=)
        CMDLINE=$(grep "^CONFIG_CMDLINE=" "$DEFCONFIG" | sed 's/^CONFIG_CMDLINE="\(.*\)"$/\1/' | head -n 1)
    fi
    
    # For GKI, if CMDLINE_EXTEND is enabled, use empty cmdline (bootloader will append)
    # Otherwise, use cmdline from defconfig
    if [ "$CMDLINE_EXTEND" = "y" ]; then
        CMDLINE=""
        log "Using empty cmdline (CMDLINE_EXTEND=y - bootloader will provide full cmdline)"
    elif [ -z "$CMDLINE" ]; then
        # Use empty cmdline for GKI (safer - let bootloader handle it)
        CMDLINE=""
        log "Using empty cmdline (GKI standard - bootloader/vendor_boot provides cmdline)"
    else
        log "Using cmdline from defconfig: $CMDLINE"
    fi
    
    # Create boot.img with header version 4 (GKI Android 13)
    # GKI boot images typically use:
    # - base: 0x00000000 (ARM64 standard)
    # - pagesize: 4096 (standard for most devices)
    # - cmdline: from defconfig
    log "Creating boot.img with cmdline: $CMDLINE"
    $MKBOOTIMG_CMD \
        --kernel "$IMAGE_PATH" \
        --header_version 4 \
        --pagesize 4096 \
        --base 0x00000000 \
        --cmdline "$CMDLINE" \
        --output "$BOOT_IMG"
    
    # Add AVB hash footer if avbtool is available
    if command -v avbtool &> /dev/null; then
        log "Adding AVB hash footer..."
        IMAGE_SIZE=$(stat -c%s "$BOOT_IMG")
        PADDING=$((2 * 1024 * 1024))  # 2MB
        PARTITION_SIZE=$((IMAGE_SIZE + PADDING))
        avbtool add_hash_footer \
            --image "$BOOT_IMG" \
            --partition_name boot \
            --partition_size "$PARTITION_SIZE"
        log "✓ AVB footer added"
    else
        warn "avbtool not found. boot.img created without AVB footer."
    fi
    
    log "✓ boot.img created: $BOOT_IMG"
}

# Pack anykernel.zip
pack_anykernel() {
    log "Packing anykernel.zip..."
    
    # Find kernel image
    IMAGE_PATH=$(find_kernel_image)
    
    # Get kernel source root directory (parent of lib directory)
    KERNEL_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
    OUT_DIR="$KERNEL_ROOT/out"
    
    # Create output directory
    mkdir -p "$OUT_DIR"
    
    # Template path
    AK3_TEMPLATE="$KERNEL_ROOT/anykernel_template"
    
    ANYKERNEL_DIR="$OUT_DIR/anykernel_tmp"
    rm -rf "$ANYKERNEL_DIR"
    
    if [ -d "$AK3_TEMPLATE" ]; then
        log "Using AnyKernel3 template from $AK3_TEMPLATE"
        cp -r "$AK3_TEMPLATE" "$ANYKERNEL_DIR"
        
        # 复制新编译的内核镜像
        log "Updating kernel Image in template..."
        cp "$IMAGE_PATH" "$ANYKERNEL_DIR/Image"
        
        # 更新 anykernel.sh 属性
        log "Updating anykernel.sh properties..."
        ANYKERNEL_BUILD_DATE=$(date +%Y-%m-%d)
        sed -i "s/kernel.string=.*/kernel.string=Serein GKI Kernel for Xiaomi 13/" "$ANYKERNEL_DIR/anykernel.sh"
        sed -i "s/device.name1=.*/device.name1=fuxi/" "$ANYKERNEL_DIR/anykernel.sh"
        sed -i "s/device.name2=.*/device.name2=xiaomi13/" "$ANYKERNEL_DIR/anykernel.sh"
        # 确保 build.date 存在或更新 (如果模板里有这个字段的话)
        if grep -q "build.date=" "$ANYKERNEL_DIR/anykernel.sh"; then
            sed -i "s/build.date=.*/build.date=$ANYKERNEL_BUILD_DATE/" "$ANYKERNEL_DIR/anykernel.sh"
        fi
    else
        warn "Template not found at $AK3_TEMPLATE, falling back to basic generation"
        mkdir -p "$ANYKERNEL_DIR/META-INF/com/google/android"
        
        # Create a simple update-binary if template is missing
        cat > "$ANYKERNEL_DIR/META-INF/com/google/android/update-binary" <<'EOF'
#!/sbin/sh
# Simple fallback update-binary
OUTFD=$2
ZIPFILE=$3
ui_print() { echo "ui_print $1" >&$OUTFD; echo "ui_print " >&$OUTFD; }
ui_print "****************************************"
ui_print "*   Serein Kernel Fallback Installer   *"
ui_print "****************************************"
ui_print "Template not found, using basic installer"
exit 0
EOF
        chmod +x "$ANYKERNEL_DIR/META-INF/com/google/android/update-binary"
        cp "$IMAGE_PATH" "$ANYKERNEL_DIR/Image"
    fi

    # 创建 anykernel.zip
    log "Creating anykernel.zip..."
    cd "$ANYKERNEL_DIR"
    zip -r "$OUT_DIR/anykernel.zip" . > /dev/null
    cd "$KERNEL_ROOT"
    rm -rf "$ANYKERNEL_DIR"

    log "✓ anykernel.zip created: $OUT_DIR/anykernel.zip"
}



