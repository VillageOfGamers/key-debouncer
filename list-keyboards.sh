#!/bin/bash

INPUT_ROOT="/dev/input"

echo "Available keyboards (paths relative to /dev/input):"
echo "--------------------------------------------------------------------------------"

for ev in "$INPUT_ROOT"/event*; do
    [[ ! -e "$ev" ]] && continue  # skip if no event devices

    # Find symlinks in by-id and by-path
    byid=$(find "$INPUT_ROOT"/by-id -lname "*$(basename "$ev")" 2>/dev/null)
    bypath=$(find "$INPUT_ROOT"/by-path -lname "*$(basename "$ev")" 2>/dev/null)

    # Skip devices with no symlinks
    [[ -z $byid && -z $bypath ]] && continue

    # Bus, vendor, product (may be empty for virtual devices)
    bustype=$(udevadm info --query=property --name="$ev" | awk -F= '/ID_BUS/ {print $2}')
    vendor=$(udevadm info --query=property --name="$ev" | awk -F= '/ID_VENDOR_ID/ {print $2}')
    product=$(udevadm info --query=property --name="$ev" | awk -F= '/ID_MODEL_ID/ {print $2}')

    # Print info
    echo "Device: $(basename "$ev")"
    # Print relative symlink paths under a single label "symlink"
    for link in $byid $bypath; do
        [[ -n $link ]] || continue
        rel="${link#$INPUT_ROOT/}"  # remove /dev/input/ from start
        echo "  Symlink: $rel"
    done
    
    [[ $bustype ]] && echo "  Bus    : $bustype"
    [[ $vendor ]]  && echo "  Vendor : $vendor"
    [[ $product ]] && echo "  Product: $product"

    echo
done