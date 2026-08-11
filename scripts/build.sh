#!/bin/bash
# 交叉编译 wireless_display（顶层 cmake，产物在 build/src/p2p_app/）
# 用法: ./scripts/build.sh [-c]    -c 全量重建
#       TOOLCHAIN=xxx.cmake ./scripts/build.sh   覆盖工具链文件
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/.." && pwd)"
TOOLCHAIN="${TOOLCHAIN:-$HOME/onvif_project/cmake/arm64-toolchain.cmake}"
BUILD="$ROOT/build"

if [ "${1:-}" = "-c" ] && [ -d "$BUILD" ]; then
  echo "==> clean rebuild"; rm -rf "$BUILD"
fi

echo "==> cmake configure  (toolchain: $TOOLCHAIN)"
cmake -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DENABLE_DRM_TARGETS=OFF -S "$ROOT" -B "$BUILD"
echo "==> cmake build (-j$(nproc))"
cmake --build "$BUILD" -j"$(nproc)"

BIN="$BUILD/src/p2p_app/p2p_manager"
file "$BIN" | grep -q aarch64 || { echo "❌ build failed (not aarch64 ELF)"; exit 1; }
echo "✅ built: $BIN"
