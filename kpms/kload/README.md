# kload — 内核无 ptrace 注入 so

## 原理

```
用户态 stealth_inject
  → 解析目标进程 dlopen 地址
  → prctl(KLOAD_INJECT, pid, path, dlopen_va)
  → 目标任意 syscall 返回用户态前
       内核改写 pc=dlopen, x0=path, x1=RTLD_NOW
  → 目标自己执行 dlopen → constructor
```

**不 ptrace。** 配合 **hide-maps** 藏 maps 路径。

## 编译

```bash
cd /home/dbc/mkpms/build
cmake ..
cmake --build . --target kload.kpm -j
cmake --build . --target hide-maps.kpm -j
# tools
../tools/stealth_inject/build.sh
```

## 部署 & 使用

```bash
adb push kload.kpm hide-maps.kpm stealth_inject libdbc.so /data/local/tmp/
adb shell chmod 755 /data/local/tmp/stealth_inject

# 一键（自动 load kpm + 等进程 + 内核注入）
adb shell su -c "/data/local/tmp/stealth_inject \
  -p com.tencent.letsgo \
  -s /data/local/tmp/libdbc.so \
  --start --key 'YOUR_SUPERKEY'"
```

## prctl API

| option | 值 | 参数 |
|--------|-----|------|
| PING | `0x4B4C0001` | 返回 `0x4B4C` |
| INJECT | `0x4B4C0010` | pid, path_str, dlopen_va |
| STATUS | `0x4B4C0011` | 0 none / 1 pending / 2 done / <0 err |
| CLEAR | `0x4B4C0012` | 清任务 |

## 验证

```bash
# 无 ptrace
grep TracerPid /proc/$(pidof com.tencent.letsgo)/status

# maps 无 libdbc 路径（需 hide-maps）
cat /proc/$(pidof com.tencent.letsgo)/maps | grep -i dbc

# so 已跑
logcat -s DbcHK

# dmesg
dmesg | grep kload
# kload: hijack tgid=... dlopen=... path=...
```

## 限制

- 依赖目标进程很快产生 syscall（游戏/UI 通常足够）
- dlopen 地址由用户态从 maps+ELF 解析，需同设备同库
- 完整「无 solist」仍靠 so 内 hide 或后续增强
- 失败时 `stealth_inject` 自动回退 tinjector

## 无痕增强（当前）

- stage 到 `code_cache/<随机8位hex>`（不再用 `.kload/libdbc.so` 特征路径）
- dlopen 成功后 **unlink** 落盘文件（map_files 可能仅见 `…/code_cache/xxxxxxxx (deleted)`）
- hide-maps 过滤 maps/smaps：**关键词 + 地址区间**（注入 so 段 + 匿名 r-xp stub）
- `TracerPid=0`（无 ptrace）

## 与 hide-maps 分工

| 模块 | 职责 |
|------|------|
| **kload** | 无 ptrace 进进程 dlopen |
| **hide-maps** | maps/smaps 滤掉 so 名 |
| **stealth_inject** | 随机 stage + unlink + 登记 hide 关键词 |
| **libdbc** | 业务 + solist hide + khook |
