#!/bin/bash
set -e

# Configuration
ORIG_BOOT="boot.img"
NEW_BOOT="boot-new.img"
UNPACK_DIR="unpack_temp"

# 1. Locate the built Kernel Image
echo "Locating new kernel image..."
KERNEL_IMAGE=$(find ../gki_build_workspace/out -name Image | head -n 1)

if [ -z "$KERNEL_IMAGE" ]; then
    echo "Error: Could not find built Kernel Image in ../gki_build_workspace/out"
    exit 1
fi
echo "Found Kernel: $KERNEL_IMAGE"

# 2. Get Original Partition Size
if [ ! -f "$ORIG_BOOT" ]; then
    echo "Error: Original $ORIG_BOOT not found."
    exit 1
fi
BOOT_SIZE=$(stat -c%s "$ORIG_BOOT")
echo "Original Boot Image Size: $BOOT_SIZE bytes"

# 3. Create new boot.img using mkbootimg
# Header version 4 is standard for GKI Android 13 devices (like Xiaomi 13).
# It contains NO ramdisk.
echo "Creating $NEW_BOOT..."

mkbootimg \
    --kernel "$KERNEL_IMAGE" \
    --header_version 4 \
    --output "$NEW_BOOT"

# 4. Sign / Add Hash Footer with avbtool
# This is important for the bootloader to recognize the partition size and hash.
echo "Adding AVB Hash Footer..."
avbtool add_hash_footer \
    --image "$NEW_BOOT" \
    --partition_name boot \
    --partition_size "$BOOT_SIZE"

echo "=========================================="
echo "Done! Repacked image: $NEW_BOOT"
echo "Flash with: fastboot flash boot $NEW_BOOT"
echo "=========================================="

