cmake --build build --target dbc-rw_obj -j$(nproc)
make dbc-rw.kpm
adb push kpms/dbc-rw/dbc-rw.kpm /sdcard/