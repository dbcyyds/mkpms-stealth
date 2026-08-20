@echo off
@REM REM build (so -> embed array -> dbc) then push & run
@REM setlocal
@REM
@REM cd /d "%~dp0"
@REM
@REM echo ==== build ====
@REM python build.py
@REM if errorlevel 1 (
@REM   echo [!] build failed
@REM   pause
@REM   exit /b 1
@REM )
@REM
@REM if not exist outputs\arm64-v8a\dbc (
@REM   echo [!] missing outputs\arm64-v8a\dbc
@REM   pause
@REM   exit /b 1
@REM )

echo ==== push ====
adb push outputs\arm64-v8a\dbc /data/local/tmp/dbc
if exist prebuilt\stealth.kpm adb push prebuilt\stealth.kpm /data/local/tmp/stealth.kpm
adb shell su -c "chmod 755 /data/local/tmp/dbc"

echo ==== run dbc ====
adb shell su -c "/data/local/tmp/dbc"

echo ==== dbc.log ====
adb shell su -c "cat /data/local/tmp/dbc.log 2>/dev/null"

echo ==== logcat ====
adb logcat -d -s DbcHK:V
echo done.
pause
