# 已知环境问题：开发机的非确定性执行异常

> 记录 M1 开发期间在一台特定开发机上遇到的**非确定性 guest 崩溃**，及其排查结论。
> 结论：这是**机器/OS 级问题，不是 PocketNX 代码缺陷**。仓库内无法修复，
> 只能记录、规避（gating）并在健康硬件/真机上验收。

## 1. 结论（TL;DR）

- `pnx-runtime-test` 的 Part B（1280×720）在本机**无法通过**：非确定性 SIGSEGV。
- 触发条件：anon RW→RX JIT 映射代码 + 紧凑 store 密集循环，写缓冲区 ≳0.2MB。
- 放大条件：信号投递 / 上下文切换（每帧 16ms `nanosleep`）。
- **决定性证据**：同一段 92 字节清屏循环**编译为普通宿主代码**（无 JIT、无 mmap、
  无 trampoline）在本机同样失败 → 异常与模拟器代码路径无关。
- 纯寄存器 5000 万次循环 20/20 通过；崩溃时内存中的指令字节 100% 正确
  → 指向**架构寄存器/标志（或瞬态 I-cache）执行异常**，微观机理未定，
  不再本机深挖。
- 验收策略：Part B **gated**（健康硬件 / 真机验证，里程碑 F）；
  Part C（1280×45 像素精确）作为可移植的 demo 逻辑验收信号。

## 2. 机器事实

| 项 | 值 |
|---|---|
| 机型 | Mac16,10（Mac mini） |
| SoC | T700X（M5 代） |
| 系统 | macOS 27.0.1 (26A434) |
| 虚拟化 | 无（`kern.hv_vmm_present = 0`，物理机） |
| 内存 | 16 GiB；故障时 ~4064 个 free page |
| Swap | 453 MB 已用 |
| 负载 | ~3.5 |
| 热事件 / ECC / 硬件错误日志 | 均无异常记录 |

## 3. 失败指纹（用于区分"已知问题"与"新 bug"）

**新崩溃若不匹配以下指纹，就不是本问题，应当重新排查：**

- `SIGSEGV code=2`（写故障）
- `pc` = 清屏列 store 指令（demo entry+0x40）
- `fault_addr` = framebuffer 基址 + capacity（越界写，指针/计数器已被破坏）
- `w18` 下溢（`0xFFFFxxxx`，列计数器绕成负数）
- `x13` 相对 stride 错位
- 而 `x8 / w10 / w11 / w12 / x21` **始终正确**
- `[fault-text]` 内存中指令字节**始终 100% 正确**（无代码损坏）

取证：崩溃时 `str` 已执行约 580 万次，而紧邻的 `sub` 只执行了约 1536 次
→ 逐指令丢失/误执行，或瞬态 I-cache 损坏（D-cache 视角的 dump 看不见）。

**第二种表现**（宿主代码中崩溃）：异常不一定落在 guest 代码里。已观察到 2/5 次
Part C 运行崩在**宿主侧**——pc 位于 dyld 共享缓存（如 `0x18a4daefc`），
故障地址很小（`0x180`），基址寄存器被清零（`x8=0`），其余寄存器带有像活循环
的状态（`w10/w11/w12` 为相邻整数）。该异常可击中本机任何紧凑循环代码
（宿主或 guest）；甄别特征仍是"内存指令字节始终正确 + 只在上述触发条件下出现"。

**注意**：`SIGBUS`（rc=138）是另一现象——执行越过极小 file-backed mmap 的 EOF
（bisect 变体 v2/v4，实验本身无效），**不是**本节的损坏，不计入失败指纹。

## 4. 复现数据

### 4.1 纯宿主代码（无 JIT / 无 trampoline / 无信号）

| 配置 | 缓冲区 | 结果 |
|---|---|---|
| host-control @720h | 3.7 MB | **6/6 失败** |
| host-control @180h | 0.9 MB | 1/5 失败 |
| host-control @45h | 0.23 MB | 5/5 通过 |
| host-control @12h | 0.06 MB | 5/5 通过 |
| counter-torture（纯寄存器，5000 万次） | — | 20/20 通过 |

→ **普通宿主代码在大缓冲区下同样失败**：异常与 JIT / trampoline / 模拟器
代码路径无关。

### 4.2 完整 runtime（demo NRO，1280×45，Part C，10 次/组）

| 配置 | 结果 |
|---|---|
| 0 trap（禁用 SleepThread） | 2/10 失败 |
| 30 trap，无 sleep | 6/10 失败 |
| 30 trap + 每帧 16ms sleep | **10/10 失败** |

→ 信号投递 / 上下文切换显著放大失败率（2/10 → 6/10 → 10/10 梯度）。

### 4.3 bisect 矩阵（45h，每组 10 次）

| 变体 | 映射方式 | 结果 |
|---|---|---|
| v1 | anon RW→RX | 8/10 失败 |
| v2 | file-backed | 0/10（全部 SIGBUS，**实验无效**） |
| v3 | anon RW→RX | 9/10 失败 |
| v4 | file-backed | 0/10（全部 SIGBUS，**实验无效**） |
| v5 | anon RW→RX | 9/10 失败 |
| v6 | anon RW→RX | 8/10 失败 |

非甄别因素（之间无显著差异）：trampoline vs 直接调用；
framebuffer 位置（heap / static / stack）。

### 4.4 复现入口

```sh
# Part B + C（1280×720 / 1280×45）
./pnx-runtime-test demo/hello_counter.nro
# 仅 Part C（可移植验收信号）
./pnx-runtime-test demo/hello_counter.nro c
```

（host-control / counter-torture / bisect 为临时目录下的独立实验二进制，
非仓库文件；如需重建，按 §4 表格配置重写即可。）

## 5. 附带发现：本机内核对 x18 / TPIDR_EL0 的处理（实测）

探针程序（x18probe，`SIGUSR1` + 不同 handler 行为）在本机的实测结果：

| 项 | 实测值 |
|---|---|
| 空闲 `x18` | `0x0` |
| 空闲 `tpidr_el0` | OS 小值（`0x2` / `0x1008`） |
| `&errno` | 固定地址 `0x1f020802c`（**非** TLS 相对） |
| `x18` 跨 signal return | **被重置为 0**；`ucontext.__x[18]` 被忽略（probe1：handler 不改动 → 0；probe2：handler 写入 `0x9999` → 仍 0） |
| `TPIDR_EL0` 跨 signal return | **保留**（probe3：`0xDEAD0000` 存活） |

对模拟器的影响：

- **guest 的 x18 会在每次 SVC trap 后被 macOS 清零**。合规 homebrew 不应在
  x18 中保留跨 SVC 的状态（真机 Switch 上 x18 同样被 OS 保留为 VDSO 指针）。
  当前 demo 不受影响（w18 是逐行局部计数器，SVC 时刻已归零）。
- **TPIDR_EL0 在 signal return 后保留**，是 trampoline guest 返回路径
  （`mrs tpidr_el0` → 取 guest TLS 块 → 恢复宿主 SP）安全的实测前提。
- trap 测试 Pass 5 的 x18 写回检查在 macOS 上豁免（capture 侧仍验证）。

## 6. 安全尺寸指引（仅本机）

- 无 trap 的纯代码：缓冲区 ≲0.2MB 基本安全；≥0.9MB 开始失败；3.7MB 必失败。
- 叠加 trap + 16ms sleep：0.23MB 也被放大到 100% 失败。
- 结论：本机不要把"Part B 通过"当验收信号；以 Part C + 真机为准。

## 7. 对验收的影响（联动 ROADMAP M1）

- **Part B（1280×720，30 帧）**：本机 gated，须在健康硬件 / 真机
  （iPhone 15 Pro，里程碑 F）上验证后关闭 M1。
- **Part C（1280×45，像素精确）**：可移植的 demo 逻辑验收信号。
  本机仍有低概率已知指纹失败（见 §3/§4）；PASS 是强正面信号，
  匹配 §3 指纹的 FAIL 是本文件记录的已知环境问题。
