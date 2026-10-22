# PocketNX

> **iOS 上的 Nintendo Switch 模拟器 —— 原生路线（路线 B）**

PocketNX 是一个**从零构建**的 Nintendo Switch 模拟器，目标是让 iPhone / iPad
直接运行 Switch 游戏，不依赖 Yuzu / Ryujinx 等既有内核。

## 为什么从零写（路线 B 的立场）

1. **架构红利**：Switch 的 CPU 是 Cortex-A57（AArch64），苹果芯片也是 AArch64。
   客端与宿主同 ISA，CPU 模拟可以做成**近 1:1 的二进制翻译 + 块链接（DBT）**，
   复杂度比 x86 宿主低一个量级。这是本项目的核心优势，必须吃透。
2. **无 GPL 包袱**：不继承 Ryujinx（MIT 但生态被 DMCA 盯上）/ Yuzu（GPLv3）代码，
   许可与分发更干净，也避免被"派生作品"身份牵连。
3. **深度针对 Apple Silicon 优化**：Metal 原生后端、PPTC 翻译缓存、内存布局，
   全部为 iOS 单 App ~6GB 内存上限量身设计。

代价：工程量约为"拿现成内核套壳"的 5–10 倍。**因此走 homebrew-first 策略**（见下）。

## 当前状态

**阶段 0 / 1**：项目骨架。内核为接口骨架（未实现），iOS 壳为可编译结构。
还没有任何游戏能运行。请阅读 [docs/ROADMAP.md](docs/ROADMAP.md) 了解里程碑。

## 开发顺序（homebrew-first）

```
M0 骨架            ← 你现在在这里
M1 跑通 homebrew    无加密 NRO/NXO，CPU DBT + 最小 OS + 帧缓冲直出（Metal）
M2 GPU 真实渲染    Maxwell → Metal，纹理/顶点/管线
M3 OS 服务层       hid/sfc/sm/pl/nifm 等核心服务，文件系统
M4 体验完善        触控/手柄、存档、分辨率缩放、性能模式
M5 DRM 层（可选）  NCA 解密 + 密钥，解锁商业游戏（需用户自备 keys/固件）
```

> **合规红线**：PocketNX 不内置、不分发任天堂固件、prod.keys 或任何游戏 ROM。
> 用户必须从**自己拥有的**设备与合法购买的游戏导出。M5 之前全部功能在
> homebrew / 无 DRM 场景下即可验证，法律风险最低。

## 目录结构

```
PocketNX/
├── Core/                 C++20 模拟内核（平台无关）
│   └── src/
│       ├── common/       日志、类型、Result、内存
│       ├── cpu/          AArch64 二进制翻译器（DBT）+ 内存模型
│       ├── gpu/          Maxwell → Metal 后端（前/后端解耦）
│       ├── os/           NX OS 服务、内存区、总线
│       └── fs/           NCA/NRO/NXO/NCM 格式解析
├── iOS/PocketNX/         Swift 外壳
│   ├── App/              入口
│   ├── Views/            游戏库 / 游戏画面 / 设置 / 触控
│   ├── Services/         内核桥接、文件、存档、手柄
│   └── Resources/
├── cmake/                构建脚本
├── docs/                 架构与路线图
└── tools/                调试 / 导出辅助
```

## 构建

需要：Xcode 15+ / iOS 17 SDK，CMake 3.25+，一个 AArch64 主机。

```bash
cd PocketNX
cmake -B build -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build build --config Release
```

iOS 壳通过 `PocketNXBridge.h`（ObjC++）调用 C++ 内核，见
[iOS/PocketNX/Services/EmulatorBridge](iOS/PocketNX/Services)。

## 侧载与 JIT

- 通过 **PlumeImpactor / AltStore** 侧载 IPA
- CPU 模拟依赖 **JIT**：需用 StikDebug 为 IPA 注入 `allow-jit` + 内存上限 entitlement
- 推荐机型：iPhone 15 Pro 起；体验最佳：iPad Pro（M4/M5，16GB）

## 许可

- 内核：**MIT**（占位，正式定稿前请宇吉确认）
- 第三方依赖：见 [docs/THIRD_PARTY.md](docs/THIRD_PARTY.md)（待补）
- Nintendo、Switch、相关商标归任天堂所有。本项目与其无任何关联。
