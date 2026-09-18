# SpecForge 工程环境

## 当前提交范围

仓库当前是 native shell + 多格式真实数据 loader。`specforge_native` 是 Win32 + DirectX 11 executable target，
用于初始化 Dear ImGui docking、ImPlot、dock host、主图、文件区、信息/标签区、谱线区、状态栏和可选 JSONL profile sink。
状态栏中的 `ms/frame` 与 `FPS` 使用 ImGui `io.DeltaTime` 的最近有效应用/UI 帧时序样本（FPS 由该样本计算），并在首帧或 idle/minimized 间隔时保留最近有效样本；它不是显示器刷新率、合成器扫描输出率或各 viewport 的 `Present` FPS。空闲时不为刷新该数字强制出帧。
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

## 必需工具

- Visual Studio 2022 Build Tools。
- C++ desktop workload。
- Windows 10/11 SDK。
- CMake 3.24 或更新版本。
- vcpkg。
- Ninja，可选，用于 `ninja-msvc-debug` 和发布前验证使用的
  `ninja-msvc-release-static` preset。

## vcpkg

设置 `VCPKG_ROOT`：

```powershell
[Environment]::SetEnvironmentVariable('VCPKG_ROOT', (Join-Path $env:USERPROFILE 'vcpkg'), 'User')
```

重新打开终端后确认：

```powershell
$env:VCPKG_ROOT
```

`vcpkg.json` 使用 manifest mode，当前目标依赖为：

- `imgui[docking-experimental,win32-binding,dx11-binding]`
- `implot`
- `cfitsio[pthreads]`（`default-features=false`，后台并发加载要求 reentrant build）
- `yaml-cpp`
- `nlohmann-json` (header-only; native JSON DOM, parsing and deterministic serialization)
- `zlib`

manifest 固定 `builtin-baseline`，避免依赖版本跟随本机 `VCPKG_ROOT` checkout 漂移。

DirectX 11 来自 Windows SDK；`specforge_renderer` 封装 DX11/DXGI presentation，`specforge_native` 负责 Win32/DWM shell。
FITS container 解析使用 vcpkg 提供的 CFITSIO。Debug preset 使用
`x64-windows`，允许 vcpkg 依赖以 DLL 形式存在；当前正式 Release preset
统一继承 `x64-windows-static`，因此 CFITSIO 链入 `Spectiary.exe`，Portable
包不携带 `cfitsio.dll`。这是当前 CFITSIO/Release 的具体选择，不是“所有依赖
必须静态”的全局规则；依赖链接策略仍按组件和分发需求分别决定。
`yaml-cpp` 用于 production ASDF sample-labeling 文档的受限 YAML metadata
解析；`zlib` 用于该 codec 的固定压缩 profile，以及受限 `.fits.gz`
单光谱读取路径。

`nlohmann-json` 通过原生 DOM 处理本地 JSON 状态及 automation 消息；
`local_user_state_json` 只保留缓存与 schema 边界。解析最多接受 64 MiB、
64 层容器及 2,097,152 个值/容器，拒绝重复的解码后对象键。读取每 64 KiB、
解析每 4096 字节检查取消（包括单个长 token）；取消异常交回调用方。
JSON 浮点语法合法，但整数 schema 字段显式检查类型和范围。
对象按键排序输出，测试验证语义及确定性，不固定空白或转义拼写。
缓存临时文件在原子替换前通过相同的读取限制，避免保存无法重新加载的状态。

## SDR 色彩与 presentation 契约

SpecForge 的主窗口和 Dear ImGui detached viewport 统一输出 sRGB/Rec.709 SDR：交换链使用
`DXGI_FORMAT_R8G8B8A8_UNORM`、双缓冲 `DXGI_SWAP_EFFECT_FLIP_DISCARD`，并通过
`IDXGISwapChain3::SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709)` 显式向 Windows 声明内容色彩空间。
初始化和 resize 都必须验证该色彩空间可用于 present；不能静默退回未标记或旧式 blt-model 交换链。
该 presentation 契约以 Windows 10 为最低运行环境，因为 `DXGI_SWAP_EFFECT_FLIP_DISCARD` 和
`IDXGISwapChain3` 不支持更早的 Windows 版本。DXGI/D3D 初始化和 resize 失败必须保留具体操作名与
`HRESULT`，避免把颜色空间、factory、swap chain 和 RTV 失败压成同一条通用错误。

`UNORM` 是存储格式，`SetColorSpace1` 才是 compositor 看到的内容色彩空间声明。不要在没有把 ImGui style、纹理和
alpha blending 全部改为 linear-light pipeline 的情况下，把 render-target view 直接改成 `_SRGB`；这会改变 GPU 写入编码，
并不能替代交换链色彩空间声明。HDR 或 Windows 自动色彩管理启用时，广色域映射由系统完成；Advanced Color 未启用时，
应用仍遵循 Windows 传统 sRGB SDR 行为，不自行承担显示器 ICC 转换。

Flip-model `Present` 不提供普通窗口被其他窗口完全覆盖的 `DXGI_STATUS_OCCLUDED` 状态。SpecForge 不通过 Z-order
枚举恢复“完全遮挡”检测，而是让可见窗口在没有消息和维护任务时停止出帧，并用原生 Win32 message wait 保留最后一次
presentation。鼠标、键盘、窗口和 ImGui viewport 消息各请求一帧；本地状态保存使用 `steady_clock` deadline 独立唤醒，
不依赖刷新率推进。Direct Manipulation 使用 `MANUALUPDATE`：在 compositor-clock 路径中，内部 queued update message
只把 input pump 标为 pending，每个 clock tick 最多执行一次 standalone pump；plot `Poll()` 仍保留自身的
`Update()`，因此 gesture wake 在同一 tick 触发 render 时可能再推进一次。实机 A/B 证明强制 single-update 会造成
pinch 卡顿及同接触恢复失败，所以这里不声明“每 tick 最多一次 `Update()`”不变量。只有生成真实
pan/zoom/inertia 增量才请求
render/present，因此接触未释放但静止时可等待，继续移动时又能恢复。clock pacing 不可用时仍保留有界的触控板
fallback cadence；若 compositor waiter 异常退出，其最后一个 tick 只请求一次过渡帧，由该帧建立 fallback deadline，
正常 active tick 仍然只是 render permission；异常边沿另记录 `waiter_failure_fallback` 与 wait result。
最小化或隐藏时仍执行到期维护，但不提交新的 render/present。
后台 source load 只在已发布 completion queue 从空变为非空时合并投递一次专用 Win32 消息；主线程收到消息后请求帧，
仍由 `ShellUi::Render()` drain completion。原有 16ms maintenance deadline 仅保留为消息投递失败等异常边沿的 fallback。

`RenderWakeScheduler` 是唯一的 render/wake 策略边界：Win32 窗口处理器持久记录失效请求，不能用
`PeekMessageW` 的返回值推断 UI 是否变化；调度器再取窗口失效、维护任务、触控板连续更新与 ImGui 时间行为的最早
deadline。弹窗开合只在遮罩渐变的有界时间内逐帧更新，活动文本输入以低频 deadline 推进光标闪烁，消息交互后按
`ImGuiIO::IniSavingRate` 从可能致脏的渲染帧完成时起安排一次保存唤醒。每次渲染后统一进入 Win32 wait；线程级消息
observer 覆盖主窗口与 detached viewport，在 WndProc 派发时持久记录失效，即使 `PeekMessageW` 派发 sent/nonqueued
消息后返回 `FALSE` 也不会丢帧。纯查询 `WM_NCHITTEST` 不产生渲染失效，避免静止 hover 与 presentation 形成反馈环。
Direct Manipulation 的未公开 `0x0096` 只在已挂载该 context 的同一 HWND 上分类为 `PumpUpdates`；消息本身仍正常
dispatch，同号消息在其他窗口仍按普通 invalidation 处理。专用 gesture wake 使用 coalescing latch；`PostMessageW`
失败会在 gesture-state lock 内释放 latch，使后续真实增量可以重试，不会永久失去渲染唤醒。
策略层不依赖 Win32、ImGui internal API 或刷新率，并由纯时间测试、真实 ImGui dirty timer 测试和 Win32 sent-message
顺序测试覆盖。

实验性按需画面捕获默认关闭。仅在启动进程前精确设置
`SPECFORGE_FRAME_CAPTURE=1` 时，`Settings > Diagnostics` 才显示
`Capture Next Main Frame`。请求会通过现有 `RenderWakeScheduler` 安排一个正常事件驱动帧，并只捕获请求帧之后
下一次成功绘制的主 application viewport；detached viewport、桌面拼接和历史最后帧不在首版范围内。捕获点位于
主 viewport 的 ImGui/DX11 draw 完成之后、`Present` 之前，按请求创建 D3D11 staging texture，完成 GPU→CPU
readback 后用 Windows Imaging Component 写入 PNG。普通运行和未请求状态不执行逐帧复制，也不保留最后帧缓存。
最小化或隐藏会明确取消尚未执行的请求，窗口恢复后不会补做。输出位于当前 storage profile 的
`captures/`（Portable 为 `Data/captures/`，其他 profile 位于相应 local user state root）；写入先使用临时文件，
只有完整编码成功后才发布最终 PNG。该同步 readback/编码会扰动帧时间，不应用于性能测量。

`D3D11SdrSwapChain::Resize` 必须显式从 D3D11 context 解绑 render target 后再释放 back buffer；调用方不应依赖先前
成功 `Present` 的隐含解绑行为。

## UI 文本编码与字体

SpecForge 的 UI 字符串边界是 UTF-8。业务状态、JSON cache、sample name、label name、annotation display name 和
ImGui widget buffer 都应继续使用 UTF-8 `std::string`；不要为了 Windows 输入把这些字段改成本地 ANSI code page 或在
业务层传播 `std::wstring`。

新增或已迁移的可见内置 UI 文案归 `src/ui/ui_text.h` 和 `src/ui/ui_text.cpp` 所有，并通过类型化词条 ID 查询。
ImGui 稳定 ID（包括 `###`/`##` 标识）、JSON 字段和领域标识属于程序协议或身份边界，不进入内置文案词条表。

Win32 shell 必须继续使用 `RegisterClassExW`、`CreateWindowExW`、`DefWindowProcW` 和 Dear ImGui 的 Win32 backend，
让中文输入先以 Unicode 进入 ImGui，再由 ImGui 写入 UTF-8 buffer。中文显示依赖 ImGui font atlas 覆盖 CJK glyph。
native app 启动时先加载 ImGui 默认字体作为主字体，保留英文和 ASCII 的默认视觉；然后从 Windows Fonts 目录合并
系统 CJK 字体作为 fallback，例如 `NotoSansSC-VF.ttf`、`Deng.ttf`、`simhei.ttf`、`msyh.ttc` 或 `simsun.ttc`。
如果输入后的字符显示为 `?`，先检查 runtime profile 中的 `dpi_config.ui_font`，确认是否实际合并了 CJK 字体；
不要通过把 UTF-8 文本转成本地代码页来修。

## CMake Presets

共享配置写在 `CMakePresets.json`。本地个人配置写在 `CMakeUserPresets.json`，不要提交。

Ninja configure check：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-ninja-msvc-debug.ps1 -Configure
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-ninja-msvc-debug.ps1 -Preset ninja-msvc-release-static -Configure
```

Visual Studio configure check：

```powershell
cmake --preset vs2022-x64-debug
```

Configure success 验证依赖和生成文件，build success 验证 native shell target。

Build native shell：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-ninja-msvc-debug.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-ninja-msvc-debug.ps1 -Preset ninja-msvc-release-static
```

### CTest 验证层级

完成 `ninja-msvc-debug` configure 并通过上面的包装器构建所需测试 target 后，日常开发的标准
edit/test loop 是从仓库根目录运行：

```powershell
ctest --preset fast
```

`fast` 是确定性、低成本的日常开发反馈层。需要在提交前执行更完整但仍为 headless 的开发验证时，运行：

```powershell
ctest --preset extended
```

2026-09-02 合并后的代表性 GitHub-hosted Windows/MSVC/Ninja 验证在
[repository verification run 33573199441](https://github.com/SW26010/SpecForge/actions/runs/33573199441/job/100071400948)
中通过同一正式入口运行了 `ctest --preset fast`：74/74 项测试通过，
`Total Test time (real) = 54.18 sec`。该结果满足低于 1 分钟的受控耗时门槛，
但尚未达到约 30 秒的首选预算。

`extended` 会运行较慢的 headless 测试，但不会代替需要专门环境或目的的验证。验证入口分工如下：

| 入口 | 用途 | 选择边界 |
| --- | --- | --- |
| `fast` preset | 日常开发 | 快速、确定性的 unit、component 和廉价 contract 测试 |
| `extended` preset | 更完整 headless | 较慢的 headless 开发验证；排除下列专门验证 labels |
| `real-gui` label | 专门 GUI 验证 | 需要交互式 Windows desktop；`gui-integration` 也按同一专门边界处理 |
| `release` label | 专门发布验证 | release artifact、packaging、configured-build 等发布合同 |
| `periodic` label | 专门周期验证 | 显式启用的资源稳定性 smoke/soak 工作负载 |
| `asdf-pinned-oracle` label | 专门 ASDF oracle 验证 | 需要 dedicated ASDF preset、固定 Python ASDF 版本和 oracle 依赖 |

`fast` / `extended` 是每项测试恰好选择一个的执行成本 tier；专门 labels 是与之正交的验证责任。
因此廉价 component test 可以同时属于 `fast` 和 `release`，但完整 release suite 仍必须通过 `release`
label 显式运行。`extended` preset 会排除 `real-gui`、`gui-integration`、`release`、`periodic` 和
`asdf-pinned-oracle`，不会把这些专门验证静默计入普通 headless 结果。未限定的 aggregate `ctest`
不是日常开发入口。

专门验证应在满足相应 build、desktop、数据或 oracle 前置条件后，以对应 label 显式选择：

```powershell
ctest --test-dir <build-directory> --output-on-failure --no-tests=error -L '^<label>$'
```

真实 GUI 的 runner 与证据规则见[自动化 CI](automation_ci.md)，release 构建与 artifact 合同见
[Release artifacts](release_artifacts.md)，periodic 资源稳定性 tier 见[响应速度测试流程](performance_testing.md)，
ASDF pinned oracle 的环境与命令见[ASDF labeling spike 运行说明](../tools/asdf_labeling_spike/README.md)。

### Ninja/MSVC 卡住排查

`ninja-msvc-debug` 和 `ninja-msvc-release-static` preset 依赖 MSVC developer
environment 与 vcpkg manifest mode。不要在普通 PowerShell 里裸跑 `ninja` 或
直接执行对应的 `cmake --build --preset ...`；`cl.exe` 可能找不到标准库头，例如
`cstddef`。

在 Codex 或其它只允许写仓库目录的受限环境里，configure/build 需要用同一套 `vcvars64.bat` 命令形态并允许写
workspace 外缓存。这两个 Ninja preset 的 configure 都会调用 vcpkg，并可能写入
`$env:VCPKG_ROOT\buildtrees\0.vcpkg_dep_info.cmake`、`buildtrees/`、`packages/`、下载缓存或 MSVC
工具链缓存；如果沙箱拦住这些 workspace 外写入，表现可能是 configure 失败或后续 build 看起来卡住。

受限环境中的正确处理方式是把 configure 和 build 都作为需要外部工具链/cache 写入权限的命令执行，并优先使用
`scripts/build-ninja-msvc-debug.ps1`。这个脚本会加载 `vcvars64.bat`、记录 stdout/stderr 到 `logs/build/`，
并把构建进程放进 Windows Job Object。启动真正的 CMake 之前，脚本会先做一次 process-kill preflight：
创建一个短生命周期探针进程、放进同类 job，然后验证当前 shell 能用 `taskkill /T /F` 清掉这棵进程树。
如果 preflight 失败，脚本会在进入 CMake 前直接失败；在 Codex 里这表示普通沙箱不能自救，需要用提权/非沙箱
方式重跑，而不是重复等待同一个命令。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-ninja-msvc-debug.ps1 -Configure -TimeoutSec 180
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-ninja-msvc-debug.ps1 -Target specforge_source_collection_session_tests -TimeoutSec 60 -Explain
```

如果看到类似 `This shell cannot kill job-assigned process trees with taskkill /T /F` 的 preflight 错误，
不要继续在同一个普通受限 shell 里重试。对 Codex agent 来说，这个命令必须通过 sandbox escalation 执行，
因为 vcpkg/MSVC 会写 workspace 外缓存，且超时清理需要能终止 job-assigned `cmd/cmake/ninja` 进程树。

如果一次构建被中断，后续命令可能卡在 Ninja lock。先查是否有残留构建进程：

```powershell
Get-Process | Where-Object { $_.ProcessName -match 'cmake|ninja|cl|link|ctest|msbuild' } |
  Select-Object Id,ProcessName,CPU,StartTime,Path
```

确认是旧的 `cmake.exe`/`ninja.exe` 持锁后，只终止对应进程，再重跑上面的 `vcvars64.bat` configure/build 命令：

```powershell
Stop-Process -Id <cmakeId>,<ninjaId> -Force
```

生成程序位于：

```text
build/ninja-msvc-debug/Spectiary.exe
build/ninja-msvc-release-static/Spectiary.exe
```

普通构建输出的 `spectiary_metadata.json` 不含 deployment，因此 EXE 作为 Standalone 运行，ImGui layout、
panel 显示状态、profile 设置和默认日志分别写入 `%LOCALAPPDATA%\SpecForge` 下的对应文件或目录。
Release 程序可在 `Settings > Diagnostics` 开始/停止性能诊断录制，并可选择 profile 输出目录。
设置 `SPECFORGE_PROFILE=1` 则从启动阶段自动录制；`SPECFORGE_PROFILE_DIR` 仍可为自动化流程覆盖 UI 设置。
录制器使用有界异步写入，单次 5 分钟或 100 MiB 自动停止；分析前检查
`profile_recorder_summary.dropped_events == 0`。`scripts/analyze-profile.ps1` 默认强制检查 summary 位于日志
末尾、停止原因有效且没有丢事件；旧格式日志只有显式传入 `-AllowLegacyIncompleteRecording` 才可继续分析。

## Portable release

第一版 portable 是 no-launcher 包：zip 根目录包含 `Spectiary.exe`、
`spectiary_metadata.json` 和 `Data\`。第三方声明与数据来源内嵌在所有分发形式
共用的 EXE 中，可从 About 阅读。直接从当前工作区文件构建：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-portable.ps1
```

严格忽略 tracked dirty 和 untracked 文件、从构建开始时的本地完整 `HEAD` object ID 构建：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-portable-from-head.ps1
```

HEAD 脚本先解析并冻结完整 `HEAD` object ID，再用 `git archive` 将该精确 revision
展开到经过校验的临时目录，并调用快照内同一份 `build-portable.ps1`。成功或失败都会
清理临时源码和 build 目录，不修改当前工作区或 Git index。

当前工作区 builder 直接调用 CMake 和当前统一的 Visual Studio release preset。HEAD
包装器不把工作区的 preset 或 configuration 默认值下传到快照；这些默认值以及 metadata
和包内容校验均由快照内 builder 按其自身代际负责。外层只确认子进程成功且 package
目录、ZIP 和 SHA-256 文件存在，避免工作区脚本解释旧代 schema。两个入口都预期在正常
开发 shell 或已批准的非沙箱 agent 运行中执行；它们不复用 Ninja debug wrapper 的日志、
timeout 和 preflight 形态。

Working-tree 输出位于 `dist\SpecForge-portable`；HEAD 输出位于
`dist\head\SpecForge-portable`。各自的 ZIP 和 `.sha256` 位于对应输出目录，HEAD
构建不会删除或覆盖 working-tree 包。
共同脚本的 source mode/revision 参数是两个正式入口之间的内部契约；为避免 dirty
checkout 被误标为 HEAD，它在源码根仍包含 `.git` 时拒绝 `head` 模式。
目录和 ZIP 根部只保留文件 `Spectiary.exe`、文件 `spectiary_metadata.json` 和
目录 `Data\`；根目录枚举包含隐藏项，不允许用隐藏文件绕过精确条目合同，也不要求
EXE 旁存在外部法律文档目录。仓库 `legal\` 中的两份文本
`THIRD_PARTY_NOTICES.txt` 和 `DATA_SOURCES.txt` 仍是可审查、可维护的唯一来源，
构建时原样嵌入 EXE。
当前 Release 验证还直接读取 `Spectiary.exe` 的普通与 delay-load PE import table，拒绝
`cfitsio.dll`、PThreads4W runtime DLL，以及因误开 CFITSIO 可选功能而出现的
curl/bzip2 runtime DLL；
Portable 根目录和 ZIP 都不得用相邻 DLL 补足该依赖。

第三方版本号来自当前构建实际安装的 vcpkg SPDX 元数据。`specforge_native` 完成最终链接后，
CMake 的 post-build finalizer tool 读取实际 `Spectiary.exe`，计算 SHA-256 和 UTC 完成时间，
并原子发布 schema 6 `spectiary_metadata.json` 到 EXE 旁；`product`、`build`、`artifact` 和可选
`deployment` 与顶层 `application_id` 是独立维度。普通 build 输出不含 deployment，因此运行身份为 Standalone，
数据目录仍为 `%LOCALAPPDATA%\SpecForge`。#106-A 在 `config/project_identity.json` 中明确最终 leaf 为 `Spectiary`，实际目录切换由 #103 完成。

`build` 中的构建环境字段为 `compiler_id`、`compiler_version`、`cmake_version`、`generator`、
`target_architecture` 和 `windows_sdk_version`，另有严格的 `completed_at_utc`；这些值来自实际配置
当前 target 的 CMake 或 finalizer。制品 ISA 统一记录为 `amd64`，Visual Studio platform、vcpkg triplet
和 preset 中仍保留工具原生的 `x64` 拼写。`artifact.file` 必须是 `Spectiary.exe`，
`artifact.sha256` 必须是该最终 EXE 的小写 64 字符十六进制 SHA-256。`completed_at_utc` 必须是
`YYYY-MM-DDTHH:mm:ssZ` 的有效 UTC 时间；它表示 link 后 finalization 完成时间，不是可复现构建输入。

若生成器没有提供权威的 Windows SDK 选择（例如 Ninja），开发构建写入 `windows_sdk_version: null`，
不会从 SDK 工具安装路径猜测。正式 Portable 打包仍要求 MSVC、x64 和非空合法的 Windows SDK 版本；
PowerShell 不会从调用 shell 的环境重复推导构建信息。Portable 打包阶段只为复制出的 metadata 增加
`deployment: { distribution: "portable", storage_profile: "portable" }`，并验证 build 目录 EXE、
metadata artifact digest、包内 `Spectiary.exe` 以及 ZIP 对应 entry 的 SHA-256 一致；EXE 本身不含渠道或
storage profile 差异。共同打包脚本接受两种严格组合：`working_tree` 必须使用 JSON `null` revision；
`head` 必须显式携带完整 40 位小写十六进制 Git object ID。共同脚本不自行读取 Git；只有隔离 HEAD 入口
负责解析 revision 并将其传入快照构建。打包脚本校验这个旁置文件，将 build provenance 原样保留到 Portable
metadata，并据此校验 `THIRD_PARTY_NOTICES.txt`。不可变构建 metadata 不写入可变用户状态目录 `Data\`。
仅重新 configure 不会改变可打包 EXE 对应的元数据。升级依赖后如未同步审查并更新 notice 标题，配置或打包
必须失败，而不是发布过期版本声明。完整字段、finalizer 顺序和失败清理规则见
[`docs/release_artifacts.md`](release_artifacts.md)。

同一组 CMake build-source 变量还生成
按实际配置生成的
`build\<preset>\generated\<configuration>\specforge\specforge_build_identity.h`
并编译进 EXE。该身份包含产品版本、configuration、目标架构和构建来源，不包含 distribution 或
storage profile。About 以这些 EXE 内字段为 build provenance 权威；它消费现有启动预检从 EXE 同目录
metadata 读取并验证后形成的快照，覆盖受支持的 schema 6，而不是在 About 中重新读取文件；
且仅在 application_id、版本、configuration、架构、source mode/revision 全部匹配时显示
compiler、CMake、generator、Windows SDK 和依赖版本。About 始终显示当前运行中
EXE 的 SHA-256；metadata 缺失，或 metadata 成功解析为受支持 envelope 但其中
build-provenance/artifact 部分无效、不完整、核心字段不匹配或 artifact hash 不匹配时，
仅省略这些 metadata-derived 字段，不显示 sidecar digest、mismatch 或 verification 状态。
无 metadata 时仍保留静态组件和许可证信息。非法 JSON、不支持的 schema 或非法
deployment 仍遵循既有启动失败契约，应用不会进入 About。
所有必填字符串必须非空且没有首尾空白，working-tree 的
`source_revision` 必须严格为 JSON `null`。About 对 working-tree 构建显示
`Source: Working tree`；对隔离提交构建直接显示完整 40 位 `source_revision`，不显示相对的
`HEAD` 标签。复制诊断信息始终包含 source mode，且只有隔离提交构建包含完整 40 位 revision。
About 的 Distribution 则只来自合法的 deployment：Installer、WinGet、Portable、Scoop；
无 metadata 或 schema 6 无 deployment 时显示 Standalone。旧 schema 3/4/5 和旧 sidecar 文件名不再兼容读取。About 另外按需校验当前 EXE 的 SHA-256，并显示 schema 6
的完成时间；无法读取 EXE 时不显示该 hash。合法 storage selection 不受
build provenance 或 artifact identity mismatch 影响；deployment 存在但字段缺失、类型错误或值未知时，
`wWinMain` 在构造任何应用状态对象前明确失败。

schema 6 只记录 link 完成后的 finalization 时间，不把 configure、compile 或 package 时间混为一谈；
该 UTC 时间不参与 EXE 输入，因此不会改变受控构建下的二进制内容。schema 6 build provenance 字段仍只用于
诊断和比较，`artifact.sha256` 提供 sidecar 与最终 EXE 的 identity binding，最终 Portable ZIP 仍由自己的
SHA-256 标识。CI build number、artifact manifest 和 Windows `VERSIONINFO` 分别属于独立契约。

## 仓库卫生

以下内容不进入提交：

- `build/`
- `out/`
- `.vs/`
- `vcpkg_installed/`
- `CMakeUserPresets.json`
- `Data/`
- `imgui.ini`
- `logs/`
- 本地光谱数据。
- `.scratch/` 中的旧 demo 和实验。

如果后续需要测试数据，只提交小型、明确授权的 fixture，并放在专门的 fixture 路径中。

## 后续实现检查

继续实现前先确认：

- 新输入是否继续从 producer 侧产出 `SpectrumSnapshotHandle`，不让 UI/plot 解析 loader 细节。
- profile JSONL schema 是否覆盖本次要判断的交互路径。
- 真实性能判断是否使用真实 `.npy` 数据和新生成日志。
- 真实数据路径是否仍留在仓库外。

## ImGui widget regression tests

See [ImGui widget regression tests](widget_testing.md) for the headless real-widget harness, focused commands, and its boundary with business automation and presentation diagnostics.

Live-resize 的 instrumentation event、字段和机器验收见 [presentation telemetry](presentation/telemetry.md)。这些诊断不改变 resize/presentation policy。#56 的范围、实验结论与下一步统一见 [调查入口](presentation/live-resize/README.md)。
