# PocketNX

> **iOS 上的 Nintendo Switch 模拟器 —— 原生路线（路线 B）**

PocketNX 是一个**从零构建**的 Nintendo Switch 模拟器，目标是让 iPhone / iPad
直接运行 Switch 游戏，不依赖 Yuzu / Ryujinx 等既有内核。

## 为什么从零写（路线 B 的立场）

1. **架构红利**：Switch 的 CPU 是 Cortex-A57（AArch64），苹果芯片也是 AArch64。
   客端与宿主同 ISA，**客端代码可直接原样执行，只需重写少数特权/陷阱指令**
   （SVC、线程指针寄存器等），比通用二进制翻译（DBT）简单一个量级、
   性能上限更高。这是本项目的核心优势，必须吃透。
2. **无 GPL 包袱**：不继承 Ryujinx（MIT 但生态被 DMCA 盯上）/ Yuzu（GPLv3）代码，
   许可与分发更干净，也避免被"派生作品"身份牵连。
3. **深度针对 Apple Silicon 优化**：Metal 原生后端、代码段常驻映射、内存布局，
   全部为 iOS 单 App ~6GB 内存上限量身设计。

代价：工程量约为"拿现成内核套壳"的 5–10 倍。**因此走 homebrew-first 策略**（见下）。

## 当前状态

**阶段 M1（代码侧已闭环）**：CPU 直执行 + 最小 OS 服务 + 帧缓冲直出后端已实现，
内置 demo（`demo/hello_counter.nro`：HELLO + 帧计数器）可在开发机 / 真机运行；
iOS 桥接（`EmulatorBridge`，ObjC++）已接通 C++ `Runtime` 与 Metal 直出。
iOS App 已在 iOS 27 模拟器构建并运行：完整管线（载入 → 重写 → 直执行 →
trap 分派 → 帧缓冲 → Metal blit）经 App 日志验证通过；Metal blit 路径另经
`-testPattern` 自检在模拟器上复验（四象限图案正确上屏，期间修复
presentDrawable 缺失等 4 处渲染缺陷，见
[docs/ENVIRONMENT-ISSUES.md](docs/ENVIRONMENT-ISSUES.md) §4.5）；本机 store
循环异常同样影响模拟器内 guest 运行（可视验收 gated 到真机）。
**待办**：iPhone 15 Pro 真机 1280×720 验收（里程碑 F，见
[docs/ROADMAP.md](docs/ROADMAP.md)）。

## 开发顺序（homebrew-first）

```
M0 项目骨架          ✅ 已完成
M1 跑通 homebrew     ✅ 代码侧闭环（真机 1280×720 验收为里程碑 F）
M2 GPU 真实渲染      Maxwell → Metal，纹理/顶点/管线
M3 OS 服务层         hid/sfc/sm/pl/nifm 等核心服务，文件系统
M4 体验完善          触控/手柄、存档、分辨率缩放、性能模式
M5 DRM 层（可选）    NCA 解密 + 密钥，解锁商业游戏（需用户自备 keys/固件）
```

> **合规红线**：PocketNX 不内置、不分发任天堂固件、prod.keys 或任何游戏 ROM。
> 用户必须从**自己拥有的**设备与合法购买的游戏导出。M5 之前全部功能在
> homebrew / 无 DRM 场景下即可验证，法律风险最低。

## 目录结构

```
PocketNX/
├── Core/src/             C++20 模拟内核（平台无关）
│   ├── common/           日志、类型、Result
│   ├── cpu/              AArch64 直执行：加载器 + 指令重写 + SVC 陷阱 + trampoline
│   ├── gpu/              帧缓冲直出后端（M1）；Maxwell → Metal（M2 待建）
│   ├── os/               NX OS 服务桩 + command dispatch 表
│   ├── fs/               NRO/NSO/NXO 格式解析
│   └── runtime/          Runtime：直执行管线编排 + 共享 framebuffer 契约
├── iOS/PocketNX/         Swift 外壳
│   ├── App/              入口
│   ├── Views/            游戏库 / 游戏画面 / 设置
│   └── Services/         EmulatorBridge（ObjC++ 跨语言边界）
├── demo/                 hello_counter（.s 源 + 构建好的 .nro）
├── docs/                 架构、路线图、已知环境问题、第三方依赖
├── tools/                单测/集成测试 + NRO 构建/检视工具
└── CMakeLists.txt        Core 构建（CI 在 Linux/macOS 上编译链接）
```

## 构建与测试

**Core（平台无关 C++20）** 用 CMake 构建；CI 在 Linux（GCC）与 macOS（AppleClang）
上编译并链接出 `libPocketNXCore.a`：

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

**iOS 壳**通过 `EmulatorBridge`（ObjC++）调用 C++ 内核，见
[iOS/PocketNX/Services/EmulatorBridge](iOS/PocketNX/Services/EmulatorBridge.mm)。
Xcode 工程由 [project.yml](project.yml) 经 `xcodegen generate` 生成（链接 Core
全部源 + `GuestThreadTrampoline.S`；`.xcodeproj` 不入库），在 iOS 模拟器上：

```bash
xcodegen generate
xcodebuild -project PocketNX.xcodeproj -scheme PocketNX \
    -destination 'platform=iOS Simulator,name=iPhone 17' -configuration Debug build
xcrun simctl install booted <derived-data>/Debug-iphonesimulator/PocketNX.app
xcrun simctl launch booted com.pocketnx.app -autoDemo   # 直达内置 demo
```

调试启动参数：`-autoDemo`（直达内置 demo，免文件选择器）、
`-frameW <w> -frameH <h>`（帧缓冲尺寸覆盖）、`-testPattern`
（host 端 Metal 自检：不走 guest，直接 blit 四象限色图验证渲染路径）、
`-logFile <name>`（PNX_LOG 落盘到 App 沙盒 Documents，便于抓取）。

### 开发与验收测试（开发机，无需 CMake）

单测 / 集成测试直接编译即可跑完整回归（6 项测试）：
`pnx-loader-test` / `pnx-program-rewriter-test` / `pnx-syscall-trap-test` /
`pnx-guest-thread-test` / `pnx-os-dispatch-test` / `pnx-runtime-test`。

```bash
# 仅 Part C（1280×45 像素精确，可移植 demo 逻辑验收信号）
pnx-runtime-test demo/hello_counter.nro c
# Part B + C（1280×720 / 1280×45）
pnx-runtime-test demo/hello_counter.nro
```

- **Demo**：`demo/hello_counter.s` 经 `tools/pnx-nro-build` 打包为
  `hello_counter.nro`（HELLO + 帧计数器，30 帧后 `ret`，每帧 `SleepThread`）。
- **iOS App 模拟器验证**：`project.yml`（xcodegen）描述 iOS 工程，
  `xcodegen generate` 后 `xcodebuild -destination 'platform=iOS Simulator,name=iPhone 17'`
  即可构建；`simctl launch ... -autoDemo` 可直接跑内置 demo（无需文件选择器）。
  调试启动参数：`-autoDemo`（直达内置 demo）、`-frameW/-frameH`（帧尺寸）、
  `-testPattern`（host 端 Metal 自检，不走 guest，blit 四象限色图验证渲染路径）、
  `-logFile <name>`（PNX_LOG 落盘到沙盒 Documents，便于抓日志）。
- **已知环境问题**：本机（Mac mini M5 / macOS 27.0.1）在紧凑 store 循环写
  ≳0.2MB 缓冲区时存在非确定性执行异常（机器/OS 级，**非代码缺陷**），故
  Part B（1280×720）gated 到真机验收，见
  [docs/ENVIRONMENT-ISSUES.md](docs/ENVIRONMENT-ISSUES.md)。

## 侧载与 allow-jit

- **硬约束**：直执行需要可执行匿名内存（`.text` 映射后 `mprotect RX`），
  iOS 上必须由签名 profile 携带 `com.apple.security.cs.allow-jit` 才能通过，
  故**只能侧载**，无法上架 App Store。
- 推荐用 **StikDebug** 侧载并为 IPA 注入 `allow-jit`（+ 内存上限 entitlement）；
  PlumeImpactor / AltStore 亦可侧载，但 allow-jit 需配合 StikDebug。
- 设置页 "JIT enabled" 会实时探测该能力（`mmap RW` + `mprotect RX`）。
- 推荐机型：iPhone 15 Pro 起；体验最佳：iPad Pro（M4/M5，16GB）。

## 许可

- 内核：**MIT**（占位，正式定稿前请宇吉确认）
- 第三方依赖：M1 当前无第三方运行时依赖，见 [docs/THIRD_PARTY.md](docs/THIRD_PARTY.md)
- Nintendo、Switch、相关商标归任天堂所有。本项目与其无任何关联。
