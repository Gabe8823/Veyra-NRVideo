# Fork 变更说明（Gabe8823/Veyra-NRVideo）

本 fork 基于 Likely7/Veyra-NRVideo 上游，包含三项改动。**全部改动尚未编译验证（本机无 VS/CMake/SDK 依赖），也未在任何 NVIDIA 实卡（尤其 RTX 30）上运行验证**；RTX 30 帧生成路径是实验性支持，失败时按原逻辑 fail-closed。上游 AGENTS.md 的运行时身份与发布授权规则仍然适用。

## 1. RTX 30（Ampere）多倍帧生成支持（实验）

思路来自 [sdli1995/dlssg_for_sm86](https://github.com/sdli1995/dlssg_for_sm86)：Ampere 上 DLSS-G 的限制主要是运行库对 GPU 架构的检查，通过把 NvAPI 架构查询结果中的 0x170（Ampere）改写为 0x1B0（Ada）即可让未修改的 NVIDIA 运行库继续工作。本项目已有同构的 `nr-ampere` 路径（仅作用于 NR 模块自身的 IAT 槽位，不碰系统入口点），本 fork 将其推广到 DLSS 帧生成：

- 新增 `src/ngx/NgxAmpereCompat.cpp` + `include/veyra/ngx/NgxAmpereCompat.h`：可复用的作用域 Ampere 架构改写（仅改写目标模块自身 IAT 中对 `nvapi64!nvapi_QueryInterface` 的解析，安装/恢复成对，逻辑与 `DlssNrRuntimeAdapter::installAmpereCompatibility` 一致）。
- `EnhanceGraph::initialize`：当驱动 NGX 能力查询报告 FG 不可用、且新设置 `fgAmpereCompat`（设置面板"RTX 30 补帧解锁 · 实验"，预设存储 v13）开启时，对已加载的驱动 `nvngx.dll` 安装改写后重试能力查询；成功则保持安装直到 `shutdown()` 恢复（覆盖 Create/Evaluate 全程与所有失败路径，包括 initialize 提前失败的守卫修正）。任何失败都按原样 fail-closed，不改冒充成功。
- 倍率上限从 4X 放开到 6X：`EnhancementSettings::validate`、`DlssFgBackend::evaluate`（multiFrameCount ≤ 5）、`EnhanceGraph` 初始化校验、UI 下拉（2X–6X）、`--fg-multiplier` CLI。**实际可用倍率仍由运行时报告的 `MultiFrameCountMax` 硬性钳制**（6X 需要 310.9+ 运行库；4X 及以上仍依赖驱动侧支持）。
- 为 6X 扩容了帧池与描述符：`genFrame_[6]→[10]`、`fgDisableReadback_[6]→[10]`、`fgDisable_[6]→[2]`（该状态缓冲本就按 parity 使用 2 个）、`generatedLeases_[6]→[10]`、`FrameBatch::frames[4]→[6]` 与 `interpolate` n≤6、`VideoPresenter` SRV 堆 12→16 槽并修正引用偏移；未用槽保持 1×1 占位纹理。

与 NR 的 `nr-ampere` 一样，这是**软件内作用域兼容层**，不修改驱动文件、不做系统级 hook、不加签或修改任何 NVIDIA 二进制。真实 RTX 30 上的效果（能否解锁、画质、性能、6X 稳定性）未经验证，不构成任何型号可用的承诺。

## 2. 播放打开新窗口问题（单实例）

现象：通过文件关联、双击视频或向 exe 拖放文件时，会再启动一个 Veyra 进程、出现第二个播放窗口。修复（`apps/veyra/ui/AppShell.cpp`）：

- 启动时创建 `Local\Veyra.SingleInstance` 互斥体；检测到已有实例时，通过 `WM_COPYDATA`（自有 dwData 标识 + 长度上限 + 内容校验）把文件路径转发给现有窗口并在其中打开，否则仅前置/还原已有窗口，然后新进程退出。
- 导出 worker（`--export-worker`）、导出 CLI（`--export-out`）与 smoke 测试进程不受单实例限制。

## 3. 播放有时不成功

两处修复：

- **失败后无法重试**：打开失败进入 `Failed` 状态后，工具栏"播放"按钮原先不会再开文件；现在 `Failed`/`Ended` 状态点播放都会重新 `openFile`，时间栏提示改为"发生错误，点播放重试或见专业诊断"。
- **瞬时错误直接中止**（`src/source/MediaFileSource.cpp`）：单个损坏包（`AV_PKT_FLAG_CORRUPT`）、解码器瞬时错误（flush 后续读）或单张损坏帧（`decode_error_flags`）原先会立即终止整个播放；现在改为跳过续播，连续 32 次仍失败才按原样停止。demux 硬错误、EOF 之后的错误、中途变分辨率仍保持致命。跳过计数进入日志（`recoverableSkips_`）。

## 验证状态（如实声明）

- 未构建：本机无 Visual Studio/CMake/FFmpeg/DLSS SDK 等依赖（`docs/BUILD.md` 所列全部缺失）。
- 未运行任何 smoke/delivery gate；`RepairPresetTests` 的预设版本断言已同步为 v13，但未执行。
- RTX 30 FG 解锁、6X、以及两项播放修复均未经实机验证。构建与实卡验收方法见 `docs/BUILD.md` 与 `scripts/gates/delivery.ps1`。
