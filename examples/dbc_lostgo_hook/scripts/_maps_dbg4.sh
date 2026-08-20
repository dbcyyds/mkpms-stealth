#!/system/bin/sh
P=$(pidof com.tencent.letsgo)
echo PID=$P
echo "map_files count=$(ls /proc/$P/map_files 2>/dev/null | wc -l)"
echo "=== map_files so sample ==="
for f in /proc/$P/map_files/*; do
  t=$(readlink $f 2>/dev/null)
  echo "$t" | grep -q '\.so' && echo "$t"
done | sort -u | head -40
echo "=== UE4? ==="
for f in /proc/$P/map_files/*; do
  t=$(readlink $f 2>/dev/null)
  echo "$t" | grep -qiE 'UE4|Unreal|libmain' && echo "$t"
done | sort -u | head -20