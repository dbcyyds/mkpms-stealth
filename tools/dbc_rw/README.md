# dbc_rw — 内核跨进程读写（用户态）

## 内核

| KPM | 说明 |
|-----|------|
| **stealth.kpm** | 推荐：hide-maps + kload + wxshadow + **dbc-rw** |
| **dbc-rw.kpm**（名 `dbc`） | 仅读写，可单独 load |

通道：**syscall(41)** + magic `0x1b1841fd2c1e0000 | cmd`

| cmd | 含义 | 参数 | 返回 |
|-----|------|------|------|
| 0 | VERSION | — | `0x20260725` |
| 1 | READ | pid, addr, buf, size | size / 0 |
| 2 | WRITE | pid, addr, buf, size | size / 0 |

同 pid 内核侧有进程缓存（mm/pgd）+ 同页 VA→PA 缓存。

## 编译用户态

```bash
cd /home/dbc/mkpms/tools/dbc_rw
aarch64-linux-gnu-gcc -O2 -static -o example_rw example_rw.c dbc_rw.c
# 或只做库对象
aarch64-linux-gnu-gcc -O2 -c dbc_rw.c -o dbc_rw.o
```

## 部署

```bash
KEY='your-superkey'
adb push ../../build/kpms/stealth/stealth.kpm /data/local/tmp/
adb push example_rw /data/local/tmp/
adb shell "su -c '/data/local/tmp/kpatch \"$KEY\" kpm load /data/local/tmp/stealth.kpm'"
adb shell "su -c '/data/local/tmp/example_rw'"
```

**注意：** 不要同时 load 独立 `dbc` 与含 rw 的 `stealth`（会双 hook syscall 41）。

## API（`dbc_rw.h`）

```c
#include "dbc_rw.h"

long ver = dbc_rw_version();          // >0 表示模块在
dbc_rw_bind(pid);

uint64_t v = dbc_rw_r64(addr);
dbc_rw_w32(addr, 123);

/* 或显式 pid */
dbc_rw_read_pid(pid, addr, buf, size);
dbc_rw_write_pid(pid, addr, buf, size);

uint64_t base = dbc_rw_module_base(pid, "libUE4.so");
pid_t p = dbc_rw_pidof("com.tencent.letsgo");
```

## 示例

```bash
# 本进程自测
./example_rw

# 读目标模块 +0
./example_rw -p com.tencent.letsgo -m libUE4.so -o 0

# 绝对地址读
./example_rw -p 12345 -a 0x7b12340000

# 写再读（慎用）
./example_rw -p 12345 -a 0x7b12340000 -w 0x1
```

## C++

可继续用 `kpms/dbc-rw/Dbc.hpp`（`namespace Dbc`，内部同类 syscall），或直接链 `dbc_rw.c`。
