#!/usr/bin/env sh
# Generate the kernel's embedded program table.
# usage: gen-initramfs.sh out.asm /vfs/path=build/file ...
set -eu
out="$1"; shift
{
    echo "bits 64"
    echo "section .rodata"
    echo "align 8"
    echo "global initramfs_blobs"
    echo "global initramfs_blob_count"
    echo "initramfs_blobs:"
    i=0
    for entry in "$@"; do
        echo "    dq blob_path_$i, blob_data_$i, blob_end_$i - blob_data_$i"
        i=$((i + 1))
    done
    echo "initramfs_blob_count: dq $i"
    i=0
    for entry in "$@"; do
        echo "blob_path_$i: db \"${entry%%=*}\", 0"
        i=$((i + 1))
    done
    i=0
    for entry in "$@"; do
        echo "align 16"
        echo "blob_data_$i: incbin \"${entry#*=}\""
        echo "blob_end_$i:"
        i=$((i + 1))
    done
    echo "section .note.GNU-stack noalloc noexec nowrite progbits"
} > "$out"
