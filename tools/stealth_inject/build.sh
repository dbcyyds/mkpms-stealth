#!/bin/bash
# 编 Android arm64 stealth_inject（可在 WSL 用 aarch64-linux-gnu-gcc）
set -e
OUT="$(cd "$(dirname "$0")" && pwd)"
NDK="${NDK:-/mnt/d/SDK/ndk/android-ndk-r28b}"
if [[ -x "$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang" ]]; then
  CC="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang"
  "$CC" -O2 -static -o "$OUT/stealth_inject" "$OUT/stealth_inject.c" -ldl
elif [[ -x "$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/aarch64-linux-android30-clang" ]]; then
  # 部分环境可从 WSL 调 Windows NDK（.cmd）
  CC="$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/aarch64-linux-android30-clang"
  "$CC" -O2 -static -o "$OUT/stealth_inject" "$OUT/stealth_inject.c" -ldl
else
  CC="${CC:-aarch64-linux-gnu-gcc}"
  "$CC" -O2 -static -o "$OUT/stealth_inject" "$OUT/stealth_inject.c" -ldl
fi
echo "OK: $OUT/stealth_inject"
file "$OUT/stealth_inject"
cp -f "$OUT/stealth_inject" "$OUT/../../outputs/arm64-v8a/stealth_inject" 2>/dev/null || true
