# 第三方依赖

> 本项目起步为纯自研，无外部运行时依赖。引入任何第三方库时，在此登记
> 名称 / 版本 / 许可 / 用途，并确保许可与本项目兼容。

**当前状态（M1）**：无第三方运行时依赖，Core 仅依赖 C/C++ 标准库与平台系统库。
iOS App 仅依赖 Apple 系统框架（Foundation / UIKit / SwiftUI / Metal / MetalKit /
UniformTypeIdentifiers）。`xcodegen` 仅为本地开发工具，用于从 `project.yml`
生成 Xcode 工程，不进入 App 产物，不视为运行时依赖。

## 规划中（M2+ 可能引入）

| 依赖 | 用途 | 许可 | 状态 |
|---|---|---|---|
| — | — | — | 待引入 |

## 注意事项
- 若引入 LLVM `MC` 做 AArch64 指令解码：Apache-2.0 WITH LLVM-exception，需登记
- 若引入 `zlib`/`zstd` 做 NCA 解压：BSD / BSD-like，需登记
- 不得引入任何含任天堂专有代码的仓库
