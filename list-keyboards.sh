#!/bin/sh

INPUT_DIR="/dev/input/by-id"

if [ ! -d "$INPUT_DIR" ]; then
    echo "No such directory: $INPUT_DIR"
    exit 1
fi

echo "Detected keyboard input devices:"
echo

found=0
for symlink in "$INPUT_DIR"/*-event-kbd; do
    [ -e "$symlink" ] || continue

    realdev=$(readlink -f "$symlink")

    # Try udevadm for device name
    name=$(udevadm info -q property -n "$realdev" 2>/dev/null | \
        grep '^NAME=' | cut -d= -f2- | tr -d '"')

    # Fallback to sysfs if udevadm fails
    if [ -z "$name" ]; then
        sysname="/sys/class/input/$(basename "$realdev")/device/name"
        if [ -r "$sysname" ]; then
            name=$(cat "$sysname")
        fi
    fi

    # Final fallback: use symlink basename
    if [ -z "$name" ]; then
        name="[unnamed] (basename: $(basename "$symlink"))"
    fi

    echo "Device:     $name"
    echo "Symlink:    $symlink"
    echo "Resolved:   $realdev"
    echo
    found=1
done

if [ "$found" -eq 0 ]; then
    echo "No keyboards found via $INPUT_DIR/*-event-kbd"
    exit 1
fi
