# dbc_lostgo_hook 例子

注入目标进程的 **`libdbc.so`** 示例：constructor 起线程 → WxShadow 挂渲染函数 → `OnRenderEnter` 跑业务。启动器 **`dbc`** 把 so 内嵌进去，用内核 **kload** 注入，不需要 tinjector / ptrace。

依赖本仓库编出的 `stealth.kpm`（hide-maps + kload + wxshadow + dbc-rw）。

## 目录

| 路径 | 说明 |
|------|------|
| `DBC/Main.cpp` | so 入口：constructor / destructor |
| `DBC/modules/core/` | Memory、Hook、WxShadow、偏移 |
| `DBC/modules/engine/Engine.cppm` | `InitEngine`、`OnRenderEnter`（业务改这里） |
| `DBC/modules/ue/` | UE 容器 / 对象 / SDK（未编进默认目标，可自行挂上） |
| `DBC/modules/game/Task.cppm` | 游戏任务逻辑（同上，默认未链） |
| `DbcHideSoinfo/` | 从 linker solist 摘掉 so |
| `launcher/dbc_main.c` | 启动器：释放 so、load kpm、kload 注入 |
| `scripts/embed_so.py` | 把 `libdbc.so` 编进启动器 |

默认 CMake 只编 core + Engine（见根 `CMakeLists.txt` 的 `DBC_MODULES`）。

## 编译

需要 Android NDK（CMake 默认 `D:/SDK/ndk/28.2.13676358`，可用 `-DNDK_PATH=` 改）和 CMake ≥ 3.28（C++23 modules）。

```bash
# 先在仓库根目录编好 stealth.kpm
cmake -S ../.. -B ../../build -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc
cmake --build ../../build --target stealth.kpm -j

# 再编本例子
cmake -S . -B cmake-build-release-ndk28 -G Ninja
cmake --build cmake-build-release-ndk28 -j
```

或 CLion 打开本目录直接 Build。Windows 也可用 `python build.py`。

产物：

```
outputs/arm64-v8a/libdbc.so   # 注入模块
outputs/arm64-v8a/dbc         # 单文件启动器（so 已内嵌）
```

## 设备运行

```bash
KEY='your-superkey'
adb push outputs/arm64-v8a/dbc /data/local/tmp/dbc
adb push ../../build/kpms/stealth/stealth.kpm /data/local/tmp/stealth.kpm
adb shell su -c 'chmod 755 /data/local/tmp/dbc'
adb shell su -c '/data/local/tmp/kpatch "'"$KEY"'" kpm load /data/local/tmp/stealth.kpm'
adb shell su -c '/data/local/tmp/dbc --package com.tencent.letsgo --key "'"$KEY"'"'
adb logcat -s DbcHK
```

启动器会：释放内嵌 so → 等包名进程和 `libUE4.so` → kload 劫持 `dlopen` → so 里 `InitEngine` 用 WxShadow 挂 `libUE4+kRenderOffset`。

Windows 可改 `m.bat` 里的 push/run。

## 业务改哪里

- 帧逻辑：`DBC/modules/engine/Engine.cppm` → `OnRenderEnter`
- 渲染偏移：同文件 `kRenderOffset`
- UE 偏移：`DBC/modules/core/GameOffsets.cppm`
- 包名 / 密钥：`launcher/dbc_main.c` 默认值，或命令行 `--package` / `--key`
