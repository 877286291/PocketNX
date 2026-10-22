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
│  │  CPU DBT │  │  GPU     │  │  OS 服务 │  │  FS 格式层   │  │
│  │ AArch64  │  │ Maxwell  │  │ hid/sfc/ │  │ NCA/NRO/NXO  │  │
│  │ → AArch64│  │ → Metal  │  │ sm/pl... │  │ NCM 解析     │  │
│  └──────────┘  └──────────┘  └──────────┘  └──────────────┘  │
│  ┌──────────────────────────────────────────────────────────┐│
│  │  common: 日志 · Result · 内存管理 · 线程池 · PPTC 缓存    ││
│  └──────────────────────────────────────────────────────────┘│
└──────────────────────────────────────────────────────────────┘
```

### 关键设计原则
1. **Core 平台无关**：C++20，不依赖 iOS API。GPU 后端通过接口注入（iOS=Metal，
   未来可加 Vulkan/桌面），便于在 Mac 上开发调试。
2. **单一跨语言边界**：只有 `EmulatorBridge` 暴露给 Swift，内核符号不外泄。
3. **homebrew-first**：内核接口以"运行无加密 NRO"为最小闭环设计，DRM 是后置插件。

---

## 2. CPU：AArch64 → AArch64 二进制翻译（DBT）

这是本项目最值钱的部分，也是最容易做对的部分（因为同 ISA）。

### 2.1 翻译策略
- **块翻译（Block-based DBT）**：以 Basic Block 为单位，一次翻译、缓存执行，
  块间链接（linking）消除翻译开销。
- **近 1:1 映射**：客端 AArch64 指令大多可直通宿主，仅对少数指令做重写：
  - 系统寄存器 / 特权指令（MSR/MRS/ERET 等）→ 陷入处理
  - 异常向量（SVC/BRK/HVC）→ 路由到 OS 服务层
  - 内存屏障 / 原子操作 → 映射宿主等价指令
  - PC 相对寻址修正
- **不做 AOT**：运行时 JIT，写宿主 ARM64 机器码到可执行内存（需 JIT entitlement）。

### 2.2 关键子系统
| 组件 | 职责 |
|---|---|
| `Decoder` | 解码客端 AArch64 指令（可用 LLVM `MC` 或自研 mini-decoder） |
| `IR` / `Emitter` | 直发模式：block → 宿主机器码；预留 IR 模式做优化 pass |
| `BlockCache` | 翻译块缓存 + 链接表（hash block start → translated entry） |
| `MemoryManager` | 客端地址空间（Switch 是 32-bit 用户态 + 保留区），mmap 映射到宿主 |
| `SyscallHandler` | SVC/BRK 分派 → 调用 OS 服务（见 §4） |
| `HWCaps` | 检测宿主特性（FP/NEON/SHA3 等），决定指令直通范围 |

### 2.3 PPTC（Profiled Persistent Translation Cache）
- 翻译块持久化到磁盘，按 (block_hash, 目标 ISA 特性指纹) 索引
- 第二次启动起加载缓存，**显著降低加载时间**（这是 Ryujinx 实测有效的特性，必做）

### 2.4 线程模型
- 主 CPU 线程（游戏主循环）+ 若干工作线程（音频、后台任务）
- 翻译器本身需线程安全（并发翻译多块），BlockCache 用分片锁

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
| 翻译缓存（内存部分） | ≤512MB |
| 音频/其他 | ~200MB |
| 余量 | 给系统 |

老机型（<8GB）走**降分辨率 + 关 PPTC 内存缓存**的降级路径。

---

## 8. 风险与对策

| 风险 | 对策 |
|---|---|
| GPU 着色器翻译工程量爆炸 | M1 用 framebuffer 直出，M2 才碰着色器；必要时先支持简单 2D 游戏验证 |
| 商业游戏兼容性长尾 | homebrew 先行；M5 后按 EmuRank 式清单逐游戏修 |
| JIT/内存 entitlement 依赖侧载 | 文档写清 StikDebug 流程；提供 entitlement 模板 |
| 单人开发速度 | 严守 homebrew-first 里程碑，每阶段可演示、可停下 |
| 法律 | 不含任天堂资产；用户自备 keys/ROM；不做 ROM 分发 |
