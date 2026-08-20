# mkpms-stealth

KernelPatch 模块栈：一次加载 `stealth.kpm`，同时具备 **maps 隐藏、无 ptrace 注入、W^X 无痕 hook、跨进程读写**。

面向 ARM64 Android（KernelPatch / APatch）。仅供安全研究。

```
                    ┌─────────────────────────────────────────────┐
                    │              stealth.kpm（四合一）            │
                    │  hide-maps │ kload │ wxshadow │ dbc-rw      │
                    └──────┬─────────┬─────────┬──────────┬───────┘
                           │         │         │          │
              prctl 0x484D │  0x4B4C │  0x5758 │ syscall 41
                           │         │         │          │
              stealth_inject ────────┘         │     tools/dbc_rw
              （包名 + so 一键注入）            │     example_rw / dbc_rw.h
                                               │
                                        wxshadow_client
                                        （隐藏断点 / patch）
```

推荐只 load **一个** `stealth.kpm`。不要再同时 load 独立的 `hide-maps` / `kload` / `wxshadow` / `dbc-rw`（读写尤其会双 hook syscall 41）。

---

## 目录

| 路径 | 说明 |
|------|------|
| `kpms/stealth/` | 融合模块：一次编出 `stealth.kpm` |
| `kpms/hide-maps/` | maps / smaps / map_files 隐藏 |
| `kpms/kload/` | 内核 dlopen 劫持（无 ptrace） |
| `kpms/wxshadow/` | W^X shadow 隐藏断点 / patch |
| `kpms/dbc-rw/` | 内核跨进程读/写 |
| `kpms/common/` | KPM 公共头 |
| `tools/stealth_inject/` | 用户态一键注入器 |
| `tools/dbc_rw/` | 用户态读写库 + `example_rw` |
| `.kp/` | [KernelPatch](https://github.com/bmax121/KernelPatch) 子模块 |
| `setup-kp.sh` | 拉 KernelPatch 并建立 `kernel` 符号链接 |

---

## 功能一览

### 1. hide-maps — 进程 maps 隐藏

过滤目标进程的 `/proc/<pid>/maps`、`smaps`、`map_files`，避免注入 so 的路径/文件名出现在这些接口里。

| 能力 | 说明 |
|------|------|
| 关键词隐藏 | 路径或 so 名命中关键词则整行不出现 |
| 地址区间隐藏 | 按 `[start, end)` 藏 VMA（匿名 stub / so 段） |
| map_files | 命中区间的 `start-end` 目录项不出现；`readlink` 返回 `-ENOENT` |
| 任务登记 | 按包名 + so 路径登记，进程起来后自动套用 |

**做不到的：** 内存扫描、`dl_iterate_phdr`、调用栈、linker solist。solist 需 so 自己摘（如 `DbcHideSoinfo`）或 tinjector `--hide`。

用户接口：`prctl(0x484Dxxxx)`，见下文 API。

### 2. kload — 无 ptrace 注入 so

用户态解析目标进程 `dlopen` 地址，内核在目标 **任意常见 syscall 返回用户态前** 改写 `pc=dlopen`、`x0=path`、`x1=RTLD_NOW`。目标自己执行 `dlopen`，走 constructor。

| 能力 | 说明 |
|------|------|
| 无 ptrace | `TracerPid=0` |
| 随机 stage | so 拷到目标 `code_cache/<随机 8 位 hex>` |
| 注入后 unlink | 落盘文件删掉，maps 里最多看到 `(deleted)` |
| 失败回退 | `stealth_inject` 可回退 tinjector |

限制：目标需要较快产生 syscall；`dlopen` 地址由用户态从 maps+ELF 解析，需同设备同库。

用户接口：`prctl(0x4B4Cxxxx)`。

### 3. wxshadow — 无痕断点 / patch

读执行分离：进程 **读** 到原始页，**执行** 的是 shadow 页（BRK 或自定义指令）。用于隐藏 inline hook / 断点对内存的修改。

| 能力 | 说明 |
|------|------|
| 隐藏断点 | shadow 页写 BRK，读侧仍是原指令 |
| 触发改寄存器 | 断点命中时可改 `x0–x30` / `sp`（每断点最多 4 个） |
| 自定义 patch | 向 shadow 写入任意机器码（如 NOP、`mov; ret`） |
| 单步恢复 | BRK 后切回原页执行原指令，再切回 shadow |
| fork / 退出 | hook `copy_process`、`exit_mmap` 做保护与清理 |

限制：

- 仅 ARM64
- 同一页不能同时被同一段代码「自读 + 执行」（自校验同页会卡死）
- PATCH 不能跨页（`offset + len <= PAGE_SIZE`）
- 每页最多 128 个断点、128 个 patch

用户接口：`prctl(0x5758xxxx)`，工具为 `wxshadow_client`。

### 4. dbc-rw — 跨进程内存读写

内核侧按 pid 走页表读写目标进程内存。用户态走 **syscall 41**（`socket`）+ magic，避免普通 `process_vm_*` 通道。

同 pid 有进程缓存（mm/pgd）和同页 VA→PA 缓存。

| cmd | 含义 | 参数 | 成功返回 |
|-----|------|------|----------|
| 0 | VERSION | — | `0x20260725` |
| 1 | READ | pid, addr, buf, size | size |
| 2 | WRITE | pid, addr, buf, size | size |

用户态：`tools/dbc_rw/dbc_rw.h` + `dbc_rw.c`，示例 `example_rw`。C++ 也可 `#include "kpms/dbc-rw/X.hpp"`。

### 5. stealth_inject — 用户态一键注入

把 hide-maps 登记、kload 注入、可选自动 `kpm load` 串成一条命令：给包名和 so 路径即可。

流程：

1. 可选 `--key` 自动 `kpatch kpm load stealth.kpm`
2. 向 hide-maps 登记包名、so 路径、隐藏关键词和地址区间
3. 等进程出现；默认再等 maps 里出现 `libUE4.so`
4. 把 so stage 到目标 `code_cache/<随机 hex>`
5. 优先 kload（无 ptrace）；失败可回退 tinjector
6. unlink 落盘 so

### 6. stealth.kpm 控制口

```text
kpatch KEY kpm ctl0 stealth            # 状态 hm/kl/wx/rw
kpatch KEY kpm ctl0 stealth hide ...   # 转发给 hide-maps
kpatch KEY kpm ctl0 stealth kload ...
kpatch KEY kpm ctl0 stealth wx ...
kpatch KEY kpm ctl0 stealth rw ...
```

---

## 环境

- 主机：Linux / WSL，CMake ≥ 3.10
- 交叉编译器：`aarch64-linux-gnu-gcc`（或 NDK `aarch64-linux-android30-clang`）
- 设备：ARM64，已刷 KernelPatch 或 APatch，知道 **superkey**
- 设备上要有 `kpatch`（一般在 magisk/apatch 模块或自行推送）

```bash
git clone --recurse-submodules https://github.com/dbcyyds/mkpms-stealth.git
cd mkpms-stealth
# 子模块没拉下来时：
git submodule update --init --recursive
# 或：
./setup-kp.sh
ln -sfn .kp/kernel kernel
```

`kernel` 必须指向 `.kp/kernel`，CMake 靠它找 KernelPatch 头文件。

Debian/Ubuntu 交叉编译器：

```bash
sudo apt install cmake gcc-aarch64-linux-gnu
```

---

## 编译

在仓库根目录：

```bash
mkdir -p build && cd build
cmake -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc ..
```

### 推荐：只编融合模块

```bash
cmake --build . --target stealth.kpm -j
# 产物: build/kpms/stealth/stealth.kpm
```

### 全部 KPM

```bash
cmake --build . -j
```

| 目标 | 产物 |
|------|------|
| `stealth.kpm` | `build/kpms/stealth/stealth.kpm` |
| `hide-maps.kpm` | `build/kpms/hide-maps/hide-maps.kpm` |
| `kload.kpm` | `build/kpms/kload/kload.kpm` |
| `wxshadow.kpm` | `build/kpms/wxshadow/wxshadow.kpm` |
| `dbc-rw.kpm` | `build/kpms/dbc-rw/dbc-rw.kpm`（模块名 `dbc`） |
| `wxshadow_client` | `build/kpms/wxshadow/wxshadow_client`（用户态，静态） |

### 注入器

```bash
cd tools/stealth_inject
./build.sh
# 产物: tools/stealth_inject/stealth_inject
```

`build.sh` 优先用 `$NDK`（默认 `/mnt/d/SDK/ndk/android-ndk-r28b`）里的 Android clang，否则用 `aarch64-linux-gnu-gcc`。可覆盖：

```bash
CC=aarch64-linux-gnu-gcc ./build.sh
```

### 读写示例

```bash
cd tools/dbc_rw
aarch64-linux-gnu-gcc -O2 -static -o example_rw example_rw.c dbc_rw.c
```

只当库用：

```bash
aarch64-linux-gnu-gcc -O2 -c dbc_rw.c -o dbc_rw.o
# 自己的程序再链 dbc_rw.o
```

---

## 使用教程

以下 `KEY` 换成你的 KernelPatch superkey。所有设备命令默认文件在 `/data/local/tmp/`。

### 1. 推文件并加载 stealth

```bash
adb push build/kpms/stealth/stealth.kpm /data/local/tmp/
adb push tools/stealth_inject/stealth_inject /data/local/tmp/
adb push tools/dbc_rw/example_rw /data/local/tmp/          # 可选
adb push build/kpms/wxshadow/wxshadow_client /data/local/tmp/  # 可选
adb shell chmod 755 /data/local/tmp/stealth_inject /data/local/tmp/example_rw /data/local/tmp/wxshadow_client

adb shell su -c '/data/local/tmp/kpatch "'"$KEY"'" kpm unload stealth'
adb shell su -c '/data/local/tmp/kpatch "'"$KEY"'" kpm unload dbc'      # 若曾单独 load 过
adb shell su -c '/data/local/tmp/kpatch "'"$KEY"'" kpm load /data/local/tmp/stealth.kpm'
adb shell su -c '/data/local/tmp/kpatch "'"$KEY"'" kpm list'
```

`dmesg` 应看到：

```
stealth: [1/4] hide-maps ready
stealth: [2/4] kload ready
stealth: [3/4] wxshadow ready
stealth: [4/4] dbc-rw ready (syscall 41)
```

`ctl0` 看状态：

```bash
adb shell su -c '/data/local/tmp/kpatch "'"$KEY"'" kpm ctl0 stealth'
# → stealth hm=1 kl=1 wx=1 rw=1 | ctl: hide|kload|wx|rw
```

`stealth_inject --key` 也会尝试自动 load，前提是同目录或 `/data/local/tmp/` 能找到 `stealth.kpm` 和 `kpatch`。

### 2. 注入 so

把要注入的 so（示例名 `libdbc.so`）推到设备：

```bash
adb push libdbc.so /data/local/tmp/
adb shell chmod 644 /data/local/tmp/libdbc.so
```

**进程已在跑：**

```bash
adb shell su -c '/data/local/tmp/stealth_inject -p com.example.app -s /data/local/tmp/libdbc.so --key "'"$KEY"'"'
```

**强停再拉起，等引擎库映射后再注（默认等 `libUE4.so`）：**

```bash
adb shell su -c '/data/local/tmp/stealth_inject \
  -p com.example.app \
  -s /data/local/tmp/libdbc.so \
  --start \
  --wait-lib libUE4.so \
  --delay 500 \
  --key "'"$KEY"'"'
```

| 参数 | 含义 | 默认 |
|------|------|------|
| `-p` / `--package` | 包名 | `com.tencent.letsgo` |
| `-s` / `--so` | so 绝对路径 | 同目录 `libdbc.so` 或 `/data/local/tmp/libdbc.so` |
| `--start` | 强停并拉起目标 | 关 |
| `--wait-lib` | maps 出现该库再注；空=只等 libdl | `libUE4.so` |
| `--delay` | 就绪后再等毫秒 | 500 |
| `--key` | superkey，用于自动 load KPM | 空（需预先 load） |
| `--kpm` | `stealth.kpm` 路径 | 同目录或 `/data/local/tmp/stealth.kpm` |
| `--kpatch` | `kpatch` 路径 | 自动找 |
| `--tinject` | 强制 tinjector，不用 kload | 关 |
| `--no-tinject` | kload 失败不回退 | 关 |
| `--timeout` | 等进程超时（秒） | 120 |

回退 tinjector 时，需设备上有 `tinjector`（同目录或 `/data/local/tmp/`）。

**检查注入是否干净：**

```bash
pid=$(adb shell su -c 'pidof com.example.app' | awk '{print $1}')
adb shell su -c "cat /proc/$pid/maps" | grep -iE 'dbc|tinject|libdbc'
# 期望：无输出

adb shell su -c "grep TracerPid /proc/$pid/status"
# kload 成功时 TracerPid=0

adb logcat -s DbcHK    # 取决于 so 自己的 log tag
adb shell su -c 'dmesg | grep -E "stealth|kload|hidemaps"'
```

### 3. 跨进程读写

先确认模块在：

```bash
adb shell su -c /data/local/tmp/example_rw
# [*] dbc_rw_version = 0x20260725
# [+] read / write 自测 OK
```

读目标模块基址 + 偏移：

```bash
adb shell su -c '/data/local/tmp/example_rw -p com.example.app -m libUE4.so -o 0'
```

读绝对地址：

```bash
adb shell su -c '/data/local/tmp/example_rw -p 12345 -a 0x7b12340000'
```

写再读（会改对方内存，谨慎）：

```bash
adb shell su -c '/data/local/tmp/example_rw -p 12345 -a 0x7b12340000 -w 0x1'
```

自己的 C 程序：

```c
#include "dbc_rw.h"

long ver = dbc_rw_version();          /* >0 表示模块在 */
pid_t pid = dbc_rw_pidof("com.example.app");
dbc_rw_bind(pid);

uint64_t base = dbc_rw_module_base(pid, "libUE4.so");
uint64_t v = dbc_rw_r64(base);
dbc_rw_w32(base + 0x100, 123);

uint8_t buf[16];
dbc_rw_read_pid(pid, base, buf, sizeof buf);
```

`hide-maps` 可能滤掉 maps 里的路径，`dbc_rw_module_base()` 按路径子串找基址时可能失败，这时用已知绝对地址。

### 4. 无痕断点 / patch

`wxshadow_client` 在设备上跑（已随 KPM 编出）：

```bash
# 看可执行段
adb shell su -c '/data/local/tmp/wxshadow_client -p <pid> -m'

# 按绝对地址下隐藏断点
adb shell su -c '/data/local/tmp/wxshadow_client -p <pid> -a 0x7b5c001234'

# 库名 + 偏移
adb shell su -c '/data/local/tmp/wxshadow_client -p <pid> -b libc.so -o 0x12345'

# 命中时改寄存器
adb shell su -c '/data/local/tmp/wxshadow_client -p <pid> -a 0x7b5c001234 -r x0=0 -r x1=0x100'

# 一次性断点（命中后自动撤）
adb shell su -c '/data/local/tmp/wxshadow_client -p <pid> -a 0x7b5c001234 --once'

# 向 shadow 写 NOP
adb shell su -c '/data/local/tmp/wxshadow_client -p <pid> -a 0x7b5c001234 --patch d503201f'

# mov x0,#0; ret
adb shell su -c '/data/local/tmp/wxshadow_client -p <pid> -a 0x7b5c001234 --patch 000080d2c0035fd6'

# 删断点 / 释放 shadow
adb shell su -c '/data/local/tmp/wxshadow_client -p <pid> -a 0x7b5c001234 -d'
adb shell su -c '/data/local/tmp/wxshadow_client -p <pid> --release'
```

日志：

```bash
adb shell su -c 'dmesg | grep wxshadow'
```

配合用户态 hook 时：跳板代码不要去读同一页；wxshadow 隐藏的是执行页上的修改，读侧仍是原文。

### 5. 卸载

```bash
adb shell su -c '/data/local/tmp/kpatch "'"$KEY"'" kpm unload stealth'
```

---

## 内核 / 用户态 API

### hide-maps `prctl`

| option | 值 | 含义 |
|--------|-----|------|
| PING | `0x484D0001` | 返回 `0x484D` 表示模块在 |
| ADD_KW | `0x484D0002` | arg1=字符串，加隐藏词 |
| ADD_RANGE | `0x484D0003` | arg1=start, arg2=end（end 不含） |
| CLR_RANGE | `0x484D0004` | 清空地址区间表 |
| REGISTER | `0x484D0010` | arg1=包名, arg2=so 路径 |
| QUERY | `0x484D0011` | 查登记任务 |
| CLEAR | `0x484D0012` | 清任务（不清 kw/range） |

头文件：`tools/stealth_inject/stealth_inject.h`。

### kload `prctl`

| option | 值 | 含义 |
|--------|-----|------|
| PING | `0x4B4C0001` | 返回 `0x4B4C` |
| INJECT | `0x4B4C0010` | pid, **目标进程内** path 用户态地址, dlopen VA |
| STATUS | `0x4B4C0011` | 0 无 / 1 等待 / 2 完成 / `<0` 错误 |
| CLEAR | `0x4B4C0012` | 清任务 |

path 必须是已经写进 **目标进程** 的 C 字符串地址（`stealth_inject` 用 `/proc/pid/mem` 写），不能传本进程里的路径指针。

头文件：`tools/stealth_inject/kload_api.h`。

### wxshadow `prctl`

| option | 值 | 含义 |
|--------|-----|------|
| SET_BP | `0x57580001` | 隐藏断点 |
| SET_REG | `0x57580002` | 断点触发改寄存器 |
| DEL_BP | `0x57580003` | 删断点 |
| SET_TLB_MODE | `0x57580004` | TLB flush 模式 |
| GET_TLB_MODE | `0x57580005` | 读当前模式 |
| PATCH | `0x57580006` | 写 shadow 机器码 |
| RELEASE | `0x57580008` | 释放 shadow，恢复原页 |

定义见 `kpms/wxshadow/wxshadow.h`。

### dbc-rw syscall

```c
long ret = syscall(41, 0x1b1841fd2c1e0000ULL | cmd, a1, a2, a3, a4);
```

用户态不要自己调 libc 的 `socket()` 包装，用 `dbc_rw.c` 里的 `svc` 封装。

---

## 单独编模块（可选）

一般不需要。若只想要其中一项：

```bash
cmake --build build --target hide-maps.kpm -j
cmake --build build --target kload.kpm -j
cmake --build build --target wxshadow.kpm -j
cmake --build build --target dbc-rw.kpm -j
```

加载独立模块时 **不要** 再 load `stealth.kpm`。独立 `dbc-rw.kpm` 的模块名是 `dbc`。

---

## 常见问题

| 现象 | 处理 |
|------|------|
| cmake 找不到头文件 | `./setup-kp.sh` 且 `kernel` → `.kp/kernel` |
| `kpm load` 失败 | superkey 不对，或与已加载模块冲突，先 `kpm list` / `unload` |
| 注入后 maps 仍能看到 so | hide-maps 未就绪，或没登记关键词/区间；`dmesg` 看 hide-maps |
| 游戏刚启动就崩 | 加 `--wait-lib`（引擎 so）和 `--delay` |
| kload 一直 pending | 目标很少 syscall，或 dlopen VA 解析错；可 `--tinject` 对比 |
| `example_rw` version≤0 | 没 load stealth，或同时 load 了独立 `dbc` 把 hook 打乱 |
| `module_base` 为 0 | maps 路径已被 hide-maps 滤掉，改用绝对地址 |
| wxshadow 下断后卡死 | 该页存在自读（CRC 一类），换页或不要对自读代码下断 |
| 读写与注入抢 syscall 41 | 只保留 stealth，unload 独立 `dbc` |

---

## License

GPL v3，见 [LICENSE](LICENSE)。衍生作品须以相同许可证开源。

KernelPatch 框架来自 [bmax121/KernelPatch](https://github.com/bmax121/KernelPatch)，遵循其许可证。

## 免责声明

仅供安全研究与学习交流，**严禁用于任何非法用途**。使用者应遵守所在地区法律法规，因使用产生的一切后果由使用者自行承担。
