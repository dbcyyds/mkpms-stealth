#!/system/bin/sh
LIB=/apex/com.android.runtime/lib64/bionic/libdl.so
echo "=== maps libdl ==="
P=$(pidof com.tencent.letsgo)
if [ -z "$P" ]; then
  am start -n com.tencent.letsgo/com.epicgames.ue4.GameActivityExt >/dev/null
  sleep 3
  P=$(pidof com.tencent.letsgo)
fi
echo PID=$P
# map_files for libdl
for f in /proc/$P/map_files/*; do
  t=$(readlink $f 2>/dev/null)
  echo "$t" | grep -q 'libdl.so' || continue
  echo "$f -> $t"
done
# nm/readelf if available
if [ -x /data/local/tmp/llvm-nm ]; then true; fi
# use toybox/readelf from ndk? try objdump via python?
ls -la $LIB
# dump first PT_LOAD via od of elf program headers - use kptools?
# print dlsym via getprop
echo "=== try resolve with linker ==="
for f in /proc/$P/map_files/*; do
  t=$(readlink $f 2>/dev/null)
  echo "$t" | grep -q 'linker64' || continue
  echo "$f -> $t"
done | head -10