# hide-maps + stealth_inject

**无痕注入 so（用户态工具 + 内核 maps 隐藏）**

## 架构

```
用户: stealth_inject -p 包名 -s so路径 [--start] [--key KEY]
         │
         ├─ prctl → hide-maps.kpm  REGISTER + 隐藏关键词
         │
         ├─ 优先 kload（无 ptrace dlopen 劫持）
         │     stage → code_cache/<随机hex> → 注入 → unlink
         │
         └─ 失败回退 tinjector
                    │
                    └─ hide-maps 过滤 maps/smaps 中的 so 路径/名
```

- **内核接口**：`prctl(0x484Dxxxx)`（见 `tools/stealth_inject/stealth_inject.h`）
- **用户工具**：只需包名 + 路径

## 一键用法

```bash
# 1) 推文件
adb push hide-maps.kpm /data/local/tmp/
adb push stealth_inject /data/local/tmp/
adb push libdbc.so /data/local/tmp/
adb push tinjector libtcore.so /data/local/tmp/   # 你现有的
adb shell chmod 755 /data/local/tmp/stealth_inject /data/local/tmp/tinjector

# 2) 加载 kpm（或工具 --key 自动 load）
kpatch <key> kpm load /data/local/tmp/hide-maps.kpm

# 3) 注入（进程起来瞬间）
stealth_inject -p com.tencent.letsgo -s /data/local/tmp/libdbc.so --start --key '<key>'
```

## prctl API（给别的用户态程序）

| option | 值 | 含义 |
|--------|-----|------|
| PING | `0x484D0001` | 返回 `0x484D` = 模块在 |
| ADD_KW | `0x484D0002` | arg1=字符串，加 maps 隐藏词 |
| REGISTER | `0x484D0010` | arg1=`struct hidemaps_job*` |
| QUERY | `0x484D0011` | 查登记任务 |
| CLEAR | `0x484D0012` | 清任务 |

```c
struct hidemaps_job {
    char package[128];
    char so_path[256];
    int  flags;
    int  last_pid;
    int  status;
};
```

## 验证无痕

```bash
pid=$(pidof com.tencent.letsgo | awk '{print $1}')
cat /proc/$pid/maps | grep -iE 'dbc|tinject|libdbc'
# 期望：无输出
logcat -s DbcHK
# 期望：so 已跑
```

## 说明

| 能力 | 状态 |
|------|------|
| maps/smaps 藏路径 | ✅ 关键词 |
| maps/smaps 藏地址区间 | ✅ `PR_HIDEMAPS_ADD_RANGE`（匿名 stub / so 段） |
| **map_files 藏区间** | ✅ readdir(`filldir64`)+`get_link`（仅 before；依赖 `ADD_RANGE`） |
| 用户态一键包名+路径 | ✅ stealth_inject |
| 进程出现即注入 | ✅ kload / tinjector |
| 无 ptrace | ✅ kload 路径 |
| 摘 solist | 靠 so 内 DbcHideSoinfo / tinject --hide |

### map_files 说明

- `ls /proc/pid/map_files`：命中 `ADD_RANGE` 的 `start-end` 目录项不出现  
- `readlink` 同区间：返回 `-ENOENT`  
- **不会** hook get_link 的 after（历史不稳定）  
- 仍可能被：内存扫描、`dl_iterate_phdr`、调用栈等发现

「不留痕迹」当前指：**maps 无 so 路径**；ptrace 窗口仍在，后续 kload 再消。
