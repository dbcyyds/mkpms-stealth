# dbc_lostgo_hook + KPM 一体

`mkpms-stealth` 仓库里的完整例子：单文件 **`dbc`** 内嵌 `libdbc.so`，用内核 **kload** 注入目标进程，**WxShadow** 挂钩子。无需 tinjector / ptrace / 单独 stealth_inject。

仓库已带编好的 `outputs/arm64-v8a/{dbc,libdbc.so}` 和 `prebuilt/stealth.kpm`。未纳入 git 的只有 CLion `.idea`、`cmake-build-*`、`outputs.zip`，以及构建时自动生成的 `launcher/so_embed.c`。

## 产出

```
outputs/arm64-v8a/
  libdbc.so   # 注入模块（Engine + Hook + hide soinfo）
  dbc         # 启动器（so 已内嵌）
prebuilt/
  stealth.kpm # hide-maps + kload + wxshadow 四合一
```

## 编译

CMake 使用 NDK `D:/SDK/ndk/28.2.13676358`，配置并构建：

```text
cmake -S . -B cmake-build-release-ndk28 -G Ninja
cmake --build cmake-build-release-ndk28 -j
```

CLion 直接 Build 亦可。

## 设备运行

```bash
# 需 root + APatch kpatch
adb push outputs/arm64-v8a/dbc /data/local/tmp/dbc
adb push prebuilt/stealth.kpm /data/local/tmp/stealth.kpm
adb shell su -c 'chmod 755 /data/local/tmp/dbc /data/local/tmp/kpatch'
adb shell su -c '/data/local/tmp/kpatch "KEY" kpm load /data/local/tmp/stealth.kpm'
adb shell su -c '/data/local/tmp/dbc --package com.tencent.letsgo --key "KEY"'
adb logcat -s DbcHK
```

或直接双击 `m.bat`。

## 流程

1. `dbc` 释放内嵌 so → `/data/local/tmp/.dxxxxxxxx`
2. 加载 `stealth.kpm`（若未就绪）
3. 启动/等待 `com.tencent.letsgo` + `libUE4.so`
4. **kload** 劫持目标 `dlopen` 加载模块
5. so 内 `InitEngine` → WxShadow 挂渲染函数 → 业务 `OnRenderEnter`

## 业务改哪里

- 帧逻辑：`DBC/modules/engine/Engine.cppm` → `OnRenderEnter`
- 偏移：`DBC/modules/core/GameOffsets.cppm`、`kRenderOffset`
- 包名/密钥：`launcher/dbc_main.c` 默认值，或命令行 `--package` / `--key`
