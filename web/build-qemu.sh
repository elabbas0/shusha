#!/bin/sh
# Builds QEMU for WebAssembly with Shusha's browser display and network card
# (qemu-wasm-shusha.patch) and copies it to web/qemu/.  Needs Docker and
# network access to GitHub, gitlab.com, zlib.net and download.gnome.org.
# Takes 20-40 minutes the first time.
set -eu

QEMU_WASM_REV=0ef7b4e2814b231705d8371dd7997f5b72e70baf
here="$(cd "$(dirname "$0")" && pwd)"
work="${QEMU_WASM_DIR:-$here/.qemu-wasm}"

if [ ! -d "$work/.git" ]; then
    git clone https://github.com/ktock/qemu-wasm "$work"
fi
cd "$work"
git checkout -q "$QEMU_WASM_REV"
if ! git apply --check -R "$here/qemu-wasm-shusha.patch" 2>/dev/null; then
    git apply "$here/qemu-wasm-shusha.patch"
fi
# zlib.net only keeps the latest release at the top level
sed -i 's#https://zlib.net/zlib-$ZLIB_VERSION.tar.xz | tar xJC#https://zlib.net/fossils/zlib-$ZLIB_VERSION.tar.gz | tar xzC#' Dockerfile

docker build -t shusha-qemu-wasm - < Dockerfile
docker rm -f shusha-qemu-wasm >/dev/null 2>&1 || true
docker run --rm -d --name shusha-qemu-wasm -v "$work:/qemu/" shusha-qemu-wasm >/dev/null
trap 'docker rm -f shusha-qemu-wasm >/dev/null 2>&1 || true' EXIT

flags="-O3 -g0 -Wno-error=unused-command-line-argument -matomics -mbulk-memory -DNDEBUG -DG_DISABLE_ASSERT -D_GNU_SOURCE -sASYNCIFY=1 -pthread -sPROXY_TO_PTHREAD=1 -sFORCE_FILESYSTEM -sALLOW_TABLE_GROWTH -sTOTAL_MEMORY=1200MB -sWASM_BIGINT -sMALLOC=mimalloc -sEXPORT_ES6=1 -sASYNCIFY_IMPORTS=ffi_call_js"
docker exec shusha-qemu-wasm emconfigure /qemu/configure --static \
    --target-list=x86_64-softmmu --cpu=wasm32 --cross-prefix= \
    --without-default-features --enable-system --enable-pixman \
    --with-coroutine=fiber \
    --extra-cflags="$flags" --extra-cxxflags="$flags" \
    --extra-ldflags="-sEXPORTED_RUNTIME_METHODS=getTempRet0,setTempRet0,addFunction,removeFunction,TTY,FS"
docker exec shusha-qemu-wasm sh -c 'emmake make -j$(nproc) qemu-system-x86_64'

mkdir -p "$here/qemu/pc-bios"
docker cp shusha-qemu-wasm:/build/qemu-system-x86_64 "$here/qemu/qemu-system-x86_64.js"
docker cp shusha-qemu-wasm:/build/qemu-system-x86_64.wasm "$here/qemu/"
docker cp shusha-qemu-wasm:/build/qemu-system-x86_64.worker.js "$here/qemu/"
for f in bios-256k.bin vgabios-stdvga.bin kvmvapic.bin efi-e1000.rom multiboot_dma.bin; do
    cp "pc-bios/$f" "$here/qemu/pc-bios/"
done
