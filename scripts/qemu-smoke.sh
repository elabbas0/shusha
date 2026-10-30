#!/usr/bin/env sh
# Boot the ISO headless and check the serial log for boot markers.
set -eu

iso="${1:-shusha.iso}"
log="${QEMU_LOG:-build/qemu-smoke.log}"
seconds="${QEMU_TIMEOUT:-120}"
mkdir -p "$(dirname "$log")"
rm -f "$log"

set +e
timeout --foreground "$seconds" qemu-system-x86_64 \
    -cdrom "$iso" -m 256M -serial "file:$log" \
    -display none -monitor none -no-reboot -no-shutdown
status=$?
set -e

if [ "$status" -ne 0 ] && [ "$status" -ne 124 ]; then
    echo "QEMU exited unexpectedly with status $status"
    cat "$log" 2>/dev/null
    exit "$status"
fi

if grep -Eq "FAIL|OOM|PAGE FAULT|EXCEPTION" "$log"; then
    echo "smoke: boot log contains an error marker"
    cat "$log"
    exit 1
fi

for marker in "scheduler core online" "ramfs online" "csprng" "shell ready"; do
    if ! grep -q "$marker" "$log"; then
        echo "smoke: missing boot marker: $marker"
        cat "$log"
        exit 1
    fi
done
echo "smoke: passed"
