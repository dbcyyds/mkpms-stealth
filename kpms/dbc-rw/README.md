# dbc-rw（内核跨进程 R/W）

- 源码：`dbcrw.c`
- 单独产物：`dbc-rw.kpm`（模块名 `dbc`）
- **推荐**：随 `stealth.kpm` 融合加载（`STEALTH_FUSED`）

用户态封装与示例：**`../../tools/dbc_rw/`**

协议：`syscall(41, 0x1b1841fd2c1e0000|cmd, ...)`  
版本：`0x20260725`（进程缓存）
