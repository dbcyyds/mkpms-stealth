# mkpms-stealth

KernelPatch 模块栈：**maps 隐藏 + 无 ptrace 注入 + W^X 无痕 hook + 跨进程读写**。

一次加载 `stealth.kpm`：

| 组件 | 职责 | 用户接口 |
|------|------|----------|
| hide-maps | 过滤 `maps` / `smaps` / `map_files` | `prctl(0x484Dxxxx)` |
| kload | 内核侧 dlopen 劫持（无 ptrace） | `prctl(0x4B4Cxxxx)` |
| wxshadow | 读/执行分离，隐藏代码修改 | `prctl(0x5758xxxx)` |
| dbc-rw | 跨进程内存读写 | `syscall(41)` + magic |

用户态：

- `tools/stealth_inject` — 包名 + so 路径一键注入
- `tools/dbc_rw` — 读写库与示例

## 依赖

- aarch64 交叉编译器（`aarch64-linux-gnu-gcc`）
- CMake ≥ 3.10
- [KernelPatch](https://github.com/bmax121/KernelPatch) 框架（`.kp` 子模块）
- 设备已用 KernelPatch / APatch，并知道 superkey

```bash
git clone --recurse-submodules https://github.com/dbcyyds/mkpms-stealth.git
cd mkpms-stealth
# 若未带子模块：
git submodule update --init --recursive
ln -sfn .kp/kernel kernel
```

## 编译

```bash
mkdir -p build && cd build
cmake -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc ..
cmake --build . --target stealth.kpm -j
# 产物: build/kpms/stealth/stealth.kpm
```

用户态注入器：

```bash
cd tools/stealth_inject
./build.sh
```

读写示例：

```bash
cd tools/dbc_rw
aarch64-linux-gnu-gcc -O2 -static -o example_rw example_rw.c dbc_rw.c
```

单独模块（`hide-maps.kpm` / `kload.kpm` / `wxshadow.kpm` / `dbc-rw.kpm`）也可分别编，但不要与 `stealth.kpm` 同时 load 同一能力（尤其是 rw：会双 hook syscall 41）。

## 部署

```bash
KEY='your-superkey'
adb push build/kpms/stealth/stealth.kpm /data/local/tmp/
adb push tools/stealth_inject/stealth_inject /data/local/tmp/
adb shell su -c "/data/local/tmp/kpatch \"$KEY\" kpm load /data/local/tmp/stealth.kpm"

# 注入（示例）
adb shell su -c "/data/local/tmp/stealth_inject -p <package> -s /data/local/tmp/libxxx.so --key \"$KEY\""
```

dmesg 应见：

```
stealth: [1/4] hide-maps ready
stealth: [2/4] kload ready
stealth: [3/4] wxshadow ready
stealth: [4/4] dbc-rw ready (syscall 41)
```

## License

GPL v3，见 [LICENSE](LICENSE)。基于本项目的衍生作品须以相同许可证开源。

## 免责声明

仅供安全研究与学习交流，**严禁用于任何非法用途**。使用者应遵守所在地区法律法规，因使用产生的一切后果由使用者自行承担。
