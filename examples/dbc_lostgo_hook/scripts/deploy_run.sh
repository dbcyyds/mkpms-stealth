#!/bin/bash
# same as dbc_kpm/scripts/deploy_run.sh
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
V8="$ROOT/outputs/arm64-v8a"
KEY="${KEY:-Lkdj4K46iwowb3K+-+@+++}"
PKG="${PKG:-com.tencent.letsgo}"

adb push "$V8/dbc" /sdcard/Download/dbc_lostgo
if [ -f "$ROOT/prebuilt/stealth.kpm" ]; then
  adb push "$ROOT/prebuilt/stealth.kpm" /sdcard/Download/stealth_kpm.kpm
fi

adb shell "su -c '
cp -f /sdcard/Download/dbc_lostgo /data/local/tmp/dbc
cp -f /sdcard/Download/stealth_kpm.kpm /data/local/tmp/stealth.kpm 2>/dev/null || true
chmod 755 /data/local/tmp/dbc
/data/local/tmp/kpatch \"$KEY\" kpm load /data/local/tmp/stealth.kpm
'"

echo "======== run dbc ========"
adb shell "su -c '/data/local/tmp/dbc --package $PKG --key $KEY --delay 800 --timeout 90'"
echo "======== logcat -s DbcHK ========"
sleep 3
adb logcat -d -s DbcHK:V | tail -50