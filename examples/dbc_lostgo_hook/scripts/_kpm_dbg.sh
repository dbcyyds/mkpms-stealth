#!/system/bin/sh
# reload kpm and check maps without dbc hide
P=$(pidof com.tencent.letsgo)
echo before_reload so_count=$(grep '\.so' /proc/$P/maps | wc -l)
# try clear via prctl from a tiny helper - use existing dbc? skip
# check if kpatch exists elsewhere
ls -la /data/adb/ap/bin 2>/dev/null
ls -la /data/adb/apd 2>/dev/null
find /data/adb -name 'kpatch' 2>/dev/null
find /data/adb -name '*patch*' 2>/dev/null | head
# list modules
ls /data/adb/modules 2>/dev/null
# After force clear by rebooting kpm - if we have kpatch
for k in /data/local/tmp/kpatch /data/adb/ap/bin/kpatch /data/adb/kpatch; do
  if [ -x "$k" ]; then echo found $k; $k 'Lkdj4K46iwowb3K+-+@+++' kpm list; fi
done