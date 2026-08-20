#!/system/bin/sh
P=$(pidof com.tencent.letsgo)
echo PID=$P
if [ -z "$P" ]; then echo no_process; exit 1; fi
echo "=== libdl ==="
grep libdl /proc/$P/maps | head
echo "=== linker64 ==="
grep linker64 /proc/$P/maps | head
echo "=== UE ==="
grep -i UE /proc/$P/maps | head
echo "=== app so ==="
grep '\.so' /proc/$P/maps | grep data/app | head -40