#!/bin/bash
set -e

# Configuration
NEW_BOOT="boot-new.img"

# 1. Locate the built Kernel Image
echo "Locating new kernel image..."
KERNEL_IMAGE=$(find ../gki_build_workspace/out -name Image | head -n 1)

if [ -z "$KERNEL_IMAGE" ]; then
    echo "Error: Could not find built Kernel Image in ../gki_build_workspace/out"
    exit 1
fi
echo "Found Kernel: $KERNEL_IMAGE"

# 2. Create new boot.img using mkbootimg
# Header version 4 is standard for GKI Android 13 devices (like Xiaomi 13).
# It contains NO ramdisk.
echo "Creating $NEW_BOOT..."

mkbootimg \
    --kernel "$KERNEL_IMAGE" \
    --header_version 4 \
    --output "$NEW_BOOT"

# 3. Add AVB Hash Footer with avbtool
# This is important for the bootloader to recognize the partition size and hash.
# Use actual image size + padding for AVB footer (dynamic based on image).
IMAGE_SIZE=$(stat -c%s "$NEW_BOOT")
# Add 2MB padding to accommodate AVB footer and ensure enough space
PADDING=$((2 * 1024 * 1024))  # 2MB
PARTITION_SIZE=$((IMAGE_SIZE + PADDING))
echo "Image size: $IMAGE_SIZE bytes"
echo "Partition size: $PARTITION_SIZE bytes (image + ${PADDING} bytes padding)"
echo "Adding AVB Hash Footer..."
avbtool add_hash_footer \
    --image "$NEW_BOOT" \
    --partition_name boot \
    --partition_size "$PARTITION_SIZE"

echo "=========================================="
echo "Done! Repacked image: $NEW_BOOT"
echo "Flash with: fastboot flash boot $NEW_BOOT"
echo "=========================================="



