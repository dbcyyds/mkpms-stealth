#!/system/bin/sh
P=$(pidof com.tencent.letsgo)
echo PID=$P
echo "=== so count ==="
grep '\.so' /proc/$P/maps | wc -l
echo "=== sample so paths ==="
grep '\.so' /proc/$P/maps | sed 's/.* //' | sort -u | head -50
echo "=== libc ==="
grep 'libc.so' /proc/$P/maps | head -5