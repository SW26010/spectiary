# Windows 平台呈现与 UI 文本合同

状态：现行合同。构建入口见 [工程环境](../development/engineering_setup.md)，
呈现策略见 [presentation policy](policy.md)，测量字段见 [telemetry](telemetry.md)。
本页集中记录 Win32/DXGI 平台行为和 UI 字符串边界；日期实验不作为新的平台保证。

## SDR 色彩与 presentation 契约

Spectiary 的主窗口和 Dear ImGui detached viewport 共享 sRGB/Rec.709 SDR 色彩合同：
像素存储使用 `DXGI_FORMAT_R8G8B8A8_UNORM`，内容色彩空间显式声明为
`DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709`。每个 HWND 的 `D3D11WindowPresentation`
默认优先选择 Composition，能力不足或不可恢复错误时使用 DXGI 回退；两条路径的 buffer 和色彩声明 API 不同。

| 后端 | Buffer 与色彩声明 |
| --- | --- |
| Composition：`D3D11CompositionSwapChain` | 三张 displayable RGBA8 buffer，通过 `IPresentationSurface::SetColorSpace` 声明 P709。独立 viewport 默认采用增量替换；主窗口保持自身既有策略，完整生命周期与预算见 [呈现策略](policy.md#独立面板缩放资源策略)。 |
| DXGI：`D3D11SdrSwapChain` | 双缓冲 `DXGI_SWAP_EFFECT_FLIP_DISCARD`，通过 `IDXGISwapChain3::SetColorSpace1` 声明 P709；初始化和 resize 都通过 `CheckColorSpaceSupport` 验证 `PRESENT` 支持，并重新设置色彩空间。 |

色彩声明失败必须按既定后端回退或失败路径处理，不能继续使用未标记内容或静默退回旧式 blt-model。
DXGI 路径以 Windows 10 为最低运行环境，因为 `DXGI_SWAP_EFFECT_FLIP_DISCARD` 和 `IDXGISwapChain3`
不支持更早版本；Composition 是否可用还取决于实际 API、presentation 和 displayable texture 能力探测。
初始化和 resize 失败必须保留具体操作名与 `HRESULT`，避免把颜色空间、factory、buffer 和 RTV 失败压成同一条通用错误。

`UNORM` 是存储格式；DXGI 的 `SetColorSpace1` 或 Composition 的 `IPresentationSurface::SetColorSpace`
才是 compositor 看到的内容色彩空间声明。不要在没有把 ImGui style、纹理和 alpha blending 全部改为
linear-light pipeline 的情况下，把 render-target view 直接改成 `_SRGB`；这会改变 GPU 写入编码，
并不能替代后端的色彩空间声明。HDR 或 Windows 自动色彩管理启用时，广色域映射由系统完成；
Advanced Color 未启用时，应用仍遵循 Windows 传统 sRGB SDR 行为，不自行承担显示器 ICC 转换。

`D3D11SdrSwapChain::Resize` 必须显式从 D3D11 context 解绑 render target 后再释放 back buffer；调用方不应依赖先前
成功 `Present` 的隐含解绑行为。

## 消息失效、时钟与按需唤醒

DXGI flip-model `Present` 不提供普通窗口被其他窗口完全覆盖的 `DXGI_STATUS_OCCLUDED` 状态。Spectiary 不通过 Z-order
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

### 平台适配器职责

- `platform/win32_message_render_observer`：主消息泵显式转交 queued message，thread-local
  `WH_CALLWNDPROC` hook 补充非队列 sent message。两条路径复用同一 render-invalidation
  谓词，使主窗口和 secondary viewport 遵循上面的失效、`0x0096` 分类和 wake latch 边界。
- `platform/win32_compositor_clock`：动态发现 Windows 11 API，管理成对 boost 生命周期和 clock tick 唤醒。
  UI 主循环将普通输入无效化合并到下一次 tick；renderer 只负责 capability-gated DXGI present flags，
  不把 DRR API 细节扩散到 plot/UI。正常 tick 与 waiter 异常过渡帧遵循上面的不同请求语义。

不能把单次实测的一帧一个 gesture 提升为 `Update()` 调用次数不变量；standalone pump
与 plot `Poll()` 的两个入口仍各有职责。

## 按需帧捕获

实验性按需画面捕获默认关闭。仅在启动进程前精确设置
`SPECTIARY_FRAME_CAPTURE=1` 时，`Settings > Diagnostics` 才显示
`Capture Next Main Frame`。请求会通过现有 `RenderWakeScheduler` 安排一个正常事件驱动帧，并只捕获请求帧之后
下一次成功绘制的主 application viewport；detached viewport、桌面拼接和历史最后帧不在首版范围内。捕获点位于
主 viewport 的 ImGui/DX11 draw 完成之后、`Present` 之前，按请求创建 D3D11 staging texture，完成 GPU→CPU
readback 后用 Windows Imaging Component 写入 PNG。普通运行和未请求状态不执行逐帧复制，也不保留最后帧缓存。
最小化或隐藏会明确取消尚未执行的请求，窗口恢复后不会补做。输出位于当前 storage profile 的
`logs/captures/`（位于当前 application data root 下）；写入先使用临时文件，
只有完整编码成功后才发布最终 PNG。该同步 readback/编码会扰动帧时间，不应用于性能测量。

## 状态栏帧时序

状态栏中的 `ms/frame` 与 `FPS` 使用 ImGui `io.DeltaTime` 的最近有效应用/UI 帧时序样本（FPS 由该样本计算），并在首帧或 idle/minimized 间隔时保留最近有效样本；它不是显示器刷新率、合成器扫描输出率或各 viewport 的 `Present` FPS。空闲时不为刷新该数字强制出帧。

## UI 文本编码与字体

Spectiary 的 UI 字符串边界是 UTF-8。业务状态、JSON cache、sample name、label name、annotation display name 和
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
