#!/system/bin/sh
P=$(pidof com.tencent.letsgo)
echo PID=$P
echo "so count=$(grep '\.so' /proc/$P/maps | wc -l)"
echo "=== UE related ==="
grep -iE 'UE4|Unreal|libmain|libgame|libUE' /proc/$P/maps | head -30
echo "=== data/app so ==="
grep 'data/app' /proc/$P/maps | grep '\.so' | sed 's/.* //' | sort -u | head -40
echo "=== native lib dirs ==="
ls /data/app/*/com.tencent.letsgo*/lib/arm64 2>/dev/null | head -30
ls /data/app/*/*letsgo*/lib/arm64 2>/dev/null | head -30
find /data/app -maxdepth 4 -path '*letsgo*' -name 'libUE4.so' 2>/dev/null
find /data/app -maxdepth 5 -path '*letsgo*' -name '*.so' 2>/dev/null | head -40