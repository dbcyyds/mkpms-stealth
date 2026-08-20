# stealth.kpm — 四合一无痕栈

将 **hide-maps + kload + wxshadow + dbc-rw** 融合成**一个** KPM。

| 组件 | 职责 | 用户接口 |
|------|------|----------|
| hide-maps | maps/smaps/map_files 隐藏 | `prctl(0x484Dxxxx)` |
| kload | 无 ptrace dlopen 劫持 | `prctl(0x4B4Cxxxx)` |
| wxshadow | W^X shadow BP/PATCH | `prctl(0x5758xxxx)` |
| **dbc-rw** | 跨进程内存读写 | **`syscall(41)` + magic** |

## 编译

```bash
cd /home/dbc/mkpms/build
cmake ..
cmake --build . --target stealth.kpm -j
# 产物: build/kpms/stealth/stealth.kpm
```

单独 `dbc-rw.kpm` / `hide-maps.kpm` 等仍可分别编。

## 部署

```bash
adb push build/kpms/stealth/stealth.kpm /data/local/tmp/
kpatch KEY kpm unload stealth   # 若已加载
kpatch KEY kpm unload dbc       # 勿与 stealth 双开 rw
kpatch KEY kpm load /data/local/tmp/stealth.kpm
kpatch KEY kpm list             # → stealth
```

dmesg 应见：

```
stealth: [1/4] hide-maps ready
stealth: [2/4] kload ready
stealth: [3/4] wxshadow ready
stealth: [4/4] dbc-rw ready (syscall 41)
```

## 读写用户态

见 **`tools/dbc_rw/`**：

```bash
cd tools/dbc_rw
aarch64-linux-gnu-gcc -O2 -static -o example_rw example_rw.c dbc_rw.c
adb push example_rw /data/local/tmp/
adb shell su -c /data/local/tmp/example_rw
```

## kpm ctl

```bash
kpatch KEY kpm ctl0 stealth
# → stealth hm=1 kl=1 wx=1 rw=1 | ctl: hide|kload|wx|rw
kpatch KEY kpm ctl0 stealth rw
```
