# PocketNX 路线图

> homebrew-first：每个里程碑都能独立演示、独立停下。先证明"CPU 能驱动画面"，
> 再叠 GPU、服务、DRM。**不要跳级。**

## M0 — 项目骨架 ✅（本次完成）
- [x] 目录结构、README、架构文档、路线图
- [x] CMake + Xcode 工程入口
- [x] C++ 内核接口骨架（common / cpu / gpu / os / fs）
- [x] iOS Swift 壳结构（App / Views / Services / 桥接）
- **验收**：工程能在 Xcode 里打开并编译出空壳 App

## M1 — 跑通第一个 homebrew（直接执行闭环）
- [x] FS：NRO / NSO / NXO 解析（段表、入口点、重定位表）→ A1 完成
- [x] CPU：加载器——将 guest 段映射到可执行内存并应用重定位 → A2+A3 完成（Loader + .dynamic/.rela 重定位，含自包含单测 pnx-loader-test）
- [ ] CPU：ProgramRewriter——扫描并重写 `SVC` / 特权指令，建立 `PC → svc` 映射
- [ ] CPU：Mach 异常/SVC 陷阱处理，路由到 OS 服务层
- [ ] CPU：内存管理器（guest 地址空间映射、栈/TLS 初始化）
- [ ] OS：最小 sfd/sfc/sm/hid 桩 + command dispatch 表
- [ ] GPU：**Framebuffer 直出后端**（不翻译着色器，把客端 framebuffer 上屏）
- [ ] 依赖：确认 `allow-jit` entitlement 与侧载流程可用
- [ ] 目标 demo：一个纯 CPU 的 homebrew（如 `hello` 计数器 / 极简 2D）能跑、能上屏
- **验收**：iPhone 15 Pro 上运行一个 homebrew，画面/文本可见，不崩

## M2 — GPU 真实渲染
- [ ] GPU 前端：命令缓冲解析、资源管理
- [ ] GPU：Maxwell 着色器 → MSL 翻译（先简单管线）
- [ ] GPU：纹理/顶点/管线/混合/深度
- [ ] OS：audio（音频输出）、pl（时区/语言）
- [ ] 目标 demo：带图形的 homebrew（如 2D 游戏 / 简单 3D）
- **验收**：图形 homebrew 正常渲染 + 有声

## M3 — OS 服务层补全
- [ ] hid 全量（手柄、体感、摇杆、按键状态）
- [ ] sfc 全量（存储卡读写、目录、文件事件）
- [ ] nifm（账户/存储）、其余常用服务
- [ ] FS：游戏更新、DLC 目录合并
- **验收**：更多 homebrew 可跑；输入/存档稳定

## M4 — 体验完善
- [ ] iOS 壳：可配置触控布局、MFC+GC 手柄映射
- [ ] 设置：分辨率缩放、抗锯齿、30/60 帧率锁定、JIT 状态指示
- [ ] 存档：导出/导入、多档
- [ ] 性能：着色器缓存、老机型降级路径、JIT 状态指示
- **验收**：体验接近成品，可长期游玩 homebrew

## M5 — DRM 层（可选，解锁商业游戏）
- [ ] FS：NCA 容器解析
- [ ] 解密：标题密钥 + 每游戏密钥（用户自备 prod.keys / 固件）
- [ ] 固件：加载系统模块
- [ ] 逐游戏兼容性适配（按 EmuRank 式清单推进）
- **验收**：合法自购游戏可运行
- ⚠️ 合规：不含任何任天堂固件/密钥/ROM；全部由用户从自有设备导出

---

## 里程碑原则
1. **每阶段可演示**：M 结束必须有一个能在真机跑的东西
2. **可停下**：任何阶段停下都是自洽状态，不欠技术债黑洞
3. **GPU 最后才上难度**：着色器翻译是最大的坑，前 4 阶段尽量绕开
4. **商业游戏是终点不是起点**：M1–M4 全在 homebrew 上验证
