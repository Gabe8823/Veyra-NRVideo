# 本地整合状态 / Local integration status

更新日期：2026-09-15。源码基线：`dc44c48`。本文是开发状态快照，不是发布说明或实卡验收报告。

## 发布与分支

- 按本地发布记录，现有下载版本为 `v1.2.0`；本次未联网重新核验发布资产。
- `main` 已从 `edabd3c` 快进到 `dc44c48`；后者与保留的 `codex/capture-fg-clock-repair-20260914` 修复分支指向同一代码基线。
- 合并完成时 `git status --short` 为空，原有23个未提交文件已由 `dc44c48` 保存。构建、运行组件、测试媒体和日志继续留在忽略目录，没有通过删除它们制造干净状态。
- 媒体兼容、采集格式、HDR/多声道、RTX30及此前PS5修复分支均已包含于主线历史，不需要再次逐个合并。
- 旧源码归档 `codex/github-source-archive` 与已被取代的 `codex/smooth-motion-experiment` 不合入；原分支保留。正式采用允许用户叠加、不强制互斥的Smooth Motion指引。
- 未推送、未发布、未变更应用版本号。文档后续提交不改变上述代码基线。

## 1.2.0 之后已整合、尚未发布的修复

| 修复 | 提交 | 实际证据与未验证边界 |
| --- | --- | --- |
| AV1软件解码与文件硬解首帧安全回退 | `9b6c414` | AV1 MP4、ProRes MOV等样本通过；本地FFmpeg启用dav1d并保留PS5 slice补丁。不是所有MOV编码组合保证，也未增加AV1/ProRes导出。 |
| 合法负AAC时间戳导致MOV黑屏 | `de17c2b` | 以实际预填和有限PTS启动音频时钟；原问题MOV短测542帧、failed=false。 |
| 采集卡内置音频无法选择 | `d9a94eb` | 增加视频filter内置音频pin绑定、稳定DevicePath及旧连接串兼容；本机独立音频枚举和专项通过，反馈者内置音频实卡未验收。此提交标题为checkpoint，但确实包含该修复。 |
| 长时间实时补帧截止时间漂移 | `ebba6f1` | live帧对重新锚定，源PTS仍保持真实；五分钟漂移模拟、DLSS 2/3/4 Create/Evaluate等通过。未完成反馈者显卡/采集卡同源实测。 |
| 音频连续性、格式与恢复 | `dc44c48` | 保留连续重采样历史、有限浮点余量，区分软件空拉与设备断流；包含validBits/packed24、预填及视频reset隔离修复。撤销cubic和块级AGC方向；真实沙沙声未验收。 |
| WASAPI录音端点输入 | 本轮本地整合 | UI同时显示`[WASAPI]`与`[DirectShow]`；共享模式、稳定endpoint ID、QPC时间轴、静音包和有界重连接入现有PCM链。本机USB3端点和视频+WASAPI组合通过静音回归；用户设备与听感未验收。 |

详细记录：[媒体兼容](MEDIA_FILE_COMPATIBILITY_PLAN_2026-09-14.md)、[音频设备选择](CAPTURE_AUDIO_DEVICE_SELECTION_REPAIR_2026-09-14.md)、[FG时钟](CAPTURE_FG_CLOCK_REPAIR_2026-09-14.md)、[当前音频修复§6](CAPTURE_AUDIO_WAVEFORM_REPAIR_PLAN_2026-09-15.md)。旧音频施工结论仅作追溯，以新方案§6为准。

## 已发布基线中保留的改动

- **1.1.0**：采集格式扩展与步长/UV/高位深SDR修复；Smooth Motion使用指引。
- **1.1.1**：首次启动效果全关，保留已保存设置；RTX30实验兼容选项，RTX30实卡验收仍未完成。
- **1.2.0**：HDR输入/增强/导出、SDR显示开关与文件/采集5.1；同时修正FP16截图行数据、六声道缓冲和单声道/立体声匹配问题。真实HDR显示、5.1扬声器及具体采集设备仍须验收，PS5保持立体声。
- 此前PS5解码细条、重连额度、串流恢复和补帧调度修复继续保留。没有恢复旧32-slice FFmpeg限制，也没有合入废弃的强制互斥实验。

## 当前候选与验证证据

候选为本机完整构建目录，不是新的便携发布包。移动EXE时不能遗漏同目录依赖和shader。

- 路径：`out/build/audio-continuity-repair-20260915/veyra.exe`（相对项目根目录）。
- 大小：11,442,688字节。
- SHA256：`F26C655DDF7D9CF07FBA35AEA323059B9F65708139AB580015CF788B23D1D552`。
- RemotePlay ON；FFmpeg前缀 `C:/veyra-deps/ffmpeg-ps5-dav1d-installed`。SDK、DLL和模型未加入源码Git。
- 前轮最终专项：12组短用例均exit0；离线生产DSP47/47；两组各120秒±1000ppm漂移回归exit0，P95软件偏差4.24594/4.30137ms，missing=0、resets=1（启动），队列高水位89.6458ms。均不冒充用户声学验收。
- 日志：`logs/audio-continuity-repair-20260915/final/`，含 `results.json`、`drift-results.json`、波形/端点/漂移日志；日志被Git忽略，仅本机存在。
- 合并轮复核：`cmd.exe /c out\build\veyra-build-x64-release.cmd` exit0（ninja no work to do）；`veyra_audio_waveform_tests.exe --offline` 47/47；`git diff --check`通过。合并轮没有重跑完整端点/漂移/GPU套件。
- 本次文档更新仅检查文档差异与本地文件链接，不以文档提交宣称新增运行验证。

## 未闭环问题与下一步

用户在火堆、口哨场景听到持续沙沙声，OBS对照无此现象。已确认的音频代码缺陷被修复，不等于已定位并根治整个听感问题；旧 `clipped=0`、静音端点测试和构建成功都不足以证明保真。

用户当前远程无法测试，不要求立即实卡验收。后续唯一诊断主线是取得问题场景同源PCM，在原始输入、转换后、最终输出前定位首次额外失真，再与系统输出/OBS进行条件匹配对照。完整生产链分段tap尚未实现，不能写成已有功能。不再凭音高猜过载，不盲加低通或块级自动增益。

后续发布需要另行授权、完整候选包回归和依赖对应源码审计；不能直接把当前本地EXE标为已发布1.2.0修复包。

WASAPI专项记录见 [WASAPI接入方案与证据](WASAPI_CAPTURE_INPUT_PLAN_2026-09-15.md)。
