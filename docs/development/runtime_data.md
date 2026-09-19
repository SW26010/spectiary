# 原生数据加载与本地 JSON 边界

状态：现行合同。工具与命令见 [工程环境](engineering_setup.md)；
数据格式和归一化边界见 [data formats](../reference/spectra/data_formats.md) 与
[spectrum snapshot contract](../reference/spectra/spectrum_snapshot_contract.md)。

## Source 加载与前台发布

默认启动仍有 small synthetic fixture 用于 smoke test；命令行源路径和 Files 面板 `Add file...` 支持通过 domain snapshot loader 打开 source。
Files 面板 `Add folder...` 使用 Windows 原生目录选择器添加目录 source，目录本身仍交给 domain snapshot loader 处理。
当前可绘制的真实数据包括 `.npy` 光谱矩阵、简单波长/流量 `.csv`、可识别的单条 SDSS/LAMOST 与 generic FITS table/image 光谱，以及第一层包含 CSV/FITS 文件的 folder collection；受限 image 识别路径不是通用 FITS 支持承诺，catalog/unsupported FITS 由 domain 产出不可绘制的 diagnostic snapshot。
Folder source 非递归加载第一层 CSV/FITS 文件，子文件夹、其它文件类型、CSV/FITS 混用都会写入 warning diagnostics。
`.npy` loader 支持 1D 或行级 2D float32/float64 array，CSV/FITS loader 产出同一类 `SpectrumSnapshotHandle` 进入同一条 UI/plot 路径。
3909 列矩阵使用固定 loglam wavelength grid，其他列数退回 pixel index 并写入 snapshot diagnostics。
辅助数组如 `*_label.npy`、`*_index.npy`、`*_ormask.npy` 和 `*_known_mask.npy` 不作为光谱打开。
Source collection 的前台准备采用有界并发：上限为 `hardware_concurrency` 限制在 1–4 之间（未知时取 1）。
worker 使用 `std::jthread`，完成当前请求后继续处理排队请求，队列空时退出；大批量 session restore 不会按来源数创建线程。
普通打开和批量恢复仍分别按提交顺序、保存顺序发布结果；排队和执行中的 task 都可以独立取消。
单条 speculative prefetch 使用独立的低优先级通道，因此不会占满前台 worker 或阻塞前台结果发布。
被 UI 拒收或替换的大对象仍由专用后台 reclaimer 释放。此上限是保守策略，并非真实数据上的最优并发结论。
当前仍不从 shell 或竖切片得出真实数据性能结论。

## 本地 JSON 解析与写入

`nlohmann-json` 通过原生 DOM 处理本地 JSON 状态及 automation 消息；
`local_user_state_json` 只保留缓存与 schema 边界。解析最多接受 64 MiB、
64 层容器及 2,097,152 个值/容器，拒绝重复的解码后对象键。读取每 64 KiB、
解析每 4096 字节检查取消（包括单个长 token）；取消异常交回调用方。
JSON 浮点语法合法，但整数 schema 字段显式检查类型和范围。
对象按键排序输出，测试验证语义及确定性，不固定空白或转义拼写。
缓存临时文件在原子替换前通过相同的读取限制，避免保存无法重新加载的状态。

## 运行身份与存储边界

普通 build 输出的 `spectiary_metadata.json` 不含 deployment，因此运行身份为 Standalone，
数据目录为 `%LOCALAPPDATA%\Spectiary`。leaf 由 `config/project_identity.json` 显式定义；
旧 `%LOCALAPPDATA%\SpecForge` 目录仅用于有界迁移。Portable 的 application data root 是包根目录。
身份与存储路径的独立性见 [项目身份合同](project_rename.md)。

只有 `config/`、`state/`、`logs/`、`unsaved/` 是保留 namespace；用户主动选择的根目录文件和其他子目录仍归用户所有。
配置重置、普通状态清理和 checkpoint 清理不得递归删除 application data root；可重建缓存继续使用系统临时存储。
具体 storage profile、迁移与路径所有权见 [ADR 0015](../adr/0015-application-storage-cutover.md)，
历史联合验证见 [2026-09-19 阶段 8 验收](../evidence/storage/20260919-storage-acceptance.md)。
