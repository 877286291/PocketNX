# PocketNX 架构

> 路线 B（从零构建）的技术蓝图。本文档是开发的事实标准，改动需评审。

## 1. 总览

```
┌──────────────────────────────────────────────────────────────┐
│  iOS 壳 (Swift / Metal View)                                  │
│  游戏库 · 设置 · 触控 · 手柄(MFC/GC) · 存档                    │
└──────────────┬───────────────────────────────────────────────┘
               │  PocketNXBridge (ObjC++)  ← 唯一跨语言边界
┌──────────────▼───────────────────────────────────────────────┐
│  Core (C++20, 平台无关)                                       │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────────┐  │
│  │ CPU 直执行│  │  GPU     │  │  OS 服务 │  │  FS 格式层   │  │
│  │ aarch64   │  │ Maxwell  │  │ hid/sfc/ │  │ NCA/NRO/NXO  │  │
│  │ 原样运行  │  │ → Metal  │  │ sm/pl... │  │ NCM 解析     │  │
│  └──────────┘  └──────────┘  └──────────┘  └──────────────┘  │
│  ┌──────────────────────────────────────────────────────────┐│
│  │  common: 日志 · Result · 内存管理 · 线程池 · SVC 分派     ││
│  └──────────────────────────────────────────────────────────┘│
└──────────────────────────────────────────────────────────────┘
```

### 关键设计原则
1. **Core 平台无关**：C++20，不依赖 iOS API。GPU 后端通过接口注入（iOS=Metal，
   未来可加 Vulkan/桌面），便于在 Mac 上开发调试。
2. **单一跨语言边界**：只有 `EmulatorBridge` 暴露给 Swift，内核符号不外泄。
3. **homebrew-first**：内核接口以"运行无加密 NRO"为最小闭环设计，DRM 是后置插件。

---

## 2. CPU：AArch64 直接执行（同 ISA Direct Execution）

> **路线调整（2026-10）**：放弃通用二进制翻译（DBT），改为**同 ISA 直接执行**——
> 客端 aarch64 代码大部分原样在 Apple Silicon 上运行，只重写少数在苹果内核下
> 非法/受限的指令。参考实现：d2r-mobile（已验证 iOS 60fps 可玩）。
> 同 ISA 下做通用翻译器偏重；直执行工程量更小、性能上限更高，仍是本项目的核心优势。

### 2.1 执行策略
- **原生执行**：游戏自己的 AArch64 代码段加载到宿主可执行内存，直接运行。
  客端与宿主同 ISA，绝大多数用户态指令逐字节保持原样，行为尽量与原版一致。
- **少数指令重写（Program Rewriter）**：加载期扫描二进制，只改写不能原样运行的指令：
  - `SVC #num` → 宿主陷阱入口（后续路由到 OS 服务层，见 §4）
  - 线程指针寄存器（TPIDR_EL0 等）访问 → 映射到宿主 TLS 或软件维护
  - 个别系统寄存器 / 特权指令（MSR/MRS/ERET 等）→ 改写或陷入处理
  - 其他无法直接执行的异常/调试类指令（如 HVC、特殊 BRK）→ 按需求改写
- **不做通用 AOT/DBT**：不再维护翻译块缓存、链接表或 IR；复杂度集中到加载、
  重定位、SVC 分派和服务实现上。
- **硬前提 = JIT entitlement**：直执行需要可执行内存，`com.apple.security.cs.allow-jit`
  （通常通过 StikDebug 注入）是必要条件。无此 entitlement 无法运行 guest 代码，
  因此 PocketNX 只能走侧载/开发者调试签名路径，不能上 App Store。

### 2.2 关键子系统
| 组件 | 职责 |
|---|---|
| `ProgramRewriter` | 扫描客端代码段，重写 SVC/特权/受限指令，生成 PC → 服务调用映射表 |
| `Loader` | 解析 NRO/NSO/NXO 段，映射代码/只读数据，处理重定位，设置入口、栈、TLS |
| `SyscallTrap` | 捕获重写后的 SVC/BRK 陷阱，提取参数并路由到 `OS::Dispatch` |
| `MemoryManager` | 管理客端地址空间：固定地址映射、匿名内存、共享/文件映射、地址转换 |
| `OS 服务层` | 按 SVC/命令号分派到 hid/sfc/sm/pl 等服务（见 §4） |
| `HWCaps` | 检测宿主特性（FP/NEON/SHA3 等），用于兼容性检查或选择指令 fallback |

### 2.3 加载地址策略
- **方案 A：原地址映射**：按 NSO 段地址 `mmap(MAP_FIXED)`。实现最简单，
  但要求目标 VA 可用，并避免与宿主进程自身映射冲突。
- **方案 B：换 base + 重定位**：加载到安全 base，应用 NSO 重定位修正绝对地址。
  更稳健，适合通用加载器；需要正确处理 RELA/RELR 及数据段绝对引用。
- M1 建议先用方案 A 打通简单 homebrew，再评估是否升级到方案 B 以支持更多二进制。

### 2.4 线程模型
- 主 CPU 线程运行游戏主循环；音频、后台任务、渲染提交等可放工作线程。
- 每个 guest 线程需要独立的寄存器上下文、栈和 TLS 槽位。
- Mach 异常处理器按线程分发，SVC 陷阱本身不需要全局翻译锁；
  但服务层内部状态仍需加锁或采用无锁设计。

---

## 3. GPU：Maxwell → Metal

Switch GPU 是 Maxwell（GM20B），走 **GPU→Metal 直译**（不套 Vulkan，省一层）。

### 3.1 分层
```
Switch GPU 命令 (Graphics Engine 寄存器 / 命令缓冲)
   │  前端 Front-End：解析命令、构建 drawable/资源
   ▼
  抽象 Render API（PocketNX 自有，平台无关）
   │
   ▼
  后端 Backend（接口 IRenderer）
     └─ iOS:  MetalBackend (MTLCommandBuffer / MTLRenderCommandEncoder)
     └─ (预留) VulkanBackend / NullBackend(调试)
```

### 3.2 需要做的
- 顶点/片元着色器：Switch 用 Maxwell SASS 式指令 → 翻译到 **MSL**（Metal Shading
  Language）。这是 GPU 侧最大工程，建议先用 LLVM 中间表示再降级 MSL。
- 资源管理：纹理、缓冲区、渲染目标的客端↔宿主映射 + 生命周期
- 命令缓冲 → `MTLCommandBuffer`，帧同步、Present 到 CVMetalTextureCache
- 后处理：分辨率缩放、抗锯齿（M4 体验项）

### 3.3 M1 的务实做法
先做 **Null / Framebuffer 后端**：把客端 framebuffer 直拷到一张 Metal 纹理上屏。
不翻译着色器就能验证"CPU 驱动了画面"，把 GPU 复杂度推迟到 M2。

---

## 4. OS 服务层

模拟 Switch 的 NX OS 服务框架。M1 只需**最小集**让 homebrew 跑起来：

| 服务 | 用途 | 里程碑 |
|---|---|---|
| `sfd` / 内存 | 虚拟内存映射、分配 | M1 |
| `hid` | 手柄/按键/体感输入 | M1(桩) M4(全量) |
| `sfc` | 文件/存储 | M1(最小) M3(全量) |
| `sm` | 服务管理 / 命名 | M1(桩) |
| `pl` | 时区/语言 | M2 |
| `nifm` | 账户/存储卡 | M3 |
| `audio` | 音频输出 | M2 |

每个服务 = 一个 `Service` 对象，按 IPC 消息号（command id）分派。
用一张 **command dispatch 表** 统一管理，未实现的服务返回 `Unimplemented`
并在日志里报出来——这是 homebrew 适配期最快的排错方式。

---

## 5. 文件系统与格式

| 格式 | 内容 | 里程碑 |
|---|---|---|
| **NRO / NXO** | homebrew 可执行（NSO + 头） | **M1 核心** |
| NSO | 可加载段 + 重定位 | M1 |
| NCA | 加密容器（商业游戏/系统文件） | M5 |
| NCM | 内容元数据 | M5 |
| 固件 (Firmware) | 系统模块 | M5（用户自备） |

M1 只实现 NRO/NXO/NSO 加载链：**读头 → 加载段 → 应用重定位 → 设入口 → 交 CPU**。

---

## 6. iOS 壳（Swift）

- **App**：`PocketNXApp` + SceneDelegate，iPad 横屏优化，支持外部键盘
- **游戏库**：扫描用户导入的 NRO/游戏目录，缩略图 + 排序
- **GameView**：`MTKView` 承载内核渲染；覆盖层画触控
- **触控**：可配置布局（虚拟摇杆 + 按钮），手势
- **手柄**：MFC（Game Controller Framework）+ GC（GameController）双框架
- **设置**：分辨率/帧率（30/60）/着色器缓存开关/JIT 状态指示
- **存档**：读写字节到 App 容器，支持导出/导入
- **桥接**：`EmulatorBridge.h`（ObjC++）把内核 C++ API 包成 ObjC 方法给 Swift

### 启动流程
```
用户选游戏 → 桥接 Core::LoadNro(path) → 内核加载段/重定位
→ 启动主 CPU 线程 → MTKView 每帧 pump → 渲染 + 音频 → 返回
```

---

## 7. 内存预算（iOS ~6GB 单 App 上限）

| 用途 | 预算 |
|---|---|
| 客端地址空间（Switch 可见内存） | ~3–4GB 上限，按需映射 |
| 宿主帧缓冲/纹理 | 720p×2 双缓冲 ≈ 200MB |
| 客端可执行段 / 重定位数据 | ≤1GB（视游戏而定；无翻译缓存） |
| 音频/其他 | ~200MB |
| 余量 | 给系统 |

老机型（<8GB）走**降分辨率 + 缩减可执行段常驻内存**的降级路径。

---

## 8. 风险与对策

| 风险 | 对策 |
|---|---|
| GPU 着色器翻译工程量爆炸 | M1 用 framebuffer 直出，M2 才碰着色器；必要时先支持简单 2D 游戏验证 |
| 商业游戏兼容性长尾 | homebrew 先行；M5 后按 EmuRank 式清单逐游戏修 |
| JIT/内存 entitlement 依赖侧载 | 文档写清 StikDebug 流程；提供 entitlement 模板 |
| 单人开发速度 | 严守 homebrew-first 里程碑，每阶段可演示、可停下 |
| 法律 | 不含任天堂资产；用户自备 keys/ROM；不做 ROM 分发 |
