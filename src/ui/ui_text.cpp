#include "ui/ui_text.h"

#include "app/local_user_state.h"
#include "domain/sample_annotation_io.h"
#include "domain/sample_labeling.h"
#include "ui/sample_labeling_controller.h"
#include "ui/sample_labeling_issue_text.h"
#include "ui/source_collection_session_types.h"

#include <array>
#include <cstddef>

namespace specforge {
namespace {

constexpr std::size_t kUiLanguageCount = static_cast<std::size_t>(UiLanguage::Count);
constexpr std::size_t kUiTextCount = static_cast<std::size_t>(UiTextId::Count);

struct UiTextEntry {
    std::string_view english;
    std::string_view simplified_chinese;
};

constexpr std::array kTextCatalog = {
    UiTextEntry{"Settings", "设置"},
    UiTextEntry{"Language", "语言"},
    UiTextEntry{
        "Choose the language used throughout SpecForge.",
        "选择 SpecForge 全应用所使用的语言。"},
    UiTextEntry{"Application language", "应用语言"},
    UiTextEntry{"English", "英语"},
    UiTextEntry{"Simplified Chinese", "简体中文"},
    UiTextEntry{
        "UI controls and application-authored messages use the selected language. Scientific names, catalog content, file paths, and diagnostic details remain unchanged.",
        "界面控件与应用生成的消息使用所选语言；科学名称、目录内容、文件路径和诊断详情保持不变。"},
    UiTextEntry{
        "The saved application language could not be loaded. English is being used.",
        "无法加载已保存的应用语言，当前使用英语。"},
    UiTextEntry{
        "The application language could not be saved. The previous language is still in use.",
        "无法保存应用语言，仍继续使用此前的语言。"},
    UiTextEntry{"Appearance", "外观"},
    UiTextEntry{
        "Adjust the application theme without changing scientific plot semantics.",
        "调整应用主题，不改变科学绘图语义。"},
    UiTextEntry{"Theme", "主题"},
    UiTextEntry{"Follow system", "跟随系统"},
    UiTextEntry{"Light", "浅色"},
    UiTextEntry{"Dark", "深色"},
    UiTextEntry{"Accent color", "强调色"},
    UiTextEntry{
        "Not available yet. The current UI uses the built-in dark style.",
        "暂不可用。当前界面使用内置深色样式。"},
    UiTextEntry{"UI scale", "界面缩放"},
    UiTextEntry{"Reset", "重置"},
    UiTextEntry{
        "100% follows Windows display scaling. This setting adds an application-specific multiplier.",
        "100% 跟随 Windows 显示缩放；此设置用于调整应用自身的缩放倍率。"},
    UiTextEntry{
        "The saved UI scale could not be loaded; using 100%.",
        "无法加载已保存的界面缩放比例，当前使用 100%。"},
    UiTextEntry{
        "The requested UI scale is not supported.",
        "请求的界面缩放比例不受支持。"},
    UiTextEntry{
        "The UI scale could not be saved.",
        "无法保存界面缩放比例。"},
    UiTextEntry{
        "The saved value is invalid or unreadable.",
        "已保存的值无效或无法读取。"},
    UiTextEntry{
        "The settings file could not be written.",
        "无法写入设置文件。"},
    UiTextEntry{
        "The requested application language is not supported.",
        "请求的应用语言不受支持。"},
    UiTextEntry{
        "The UI scale must be from 80% through 150%.",
        "界面缩放比例必须在 80% 到 150% 之间。"},
    UiTextEntry{
        "The output directory is controlled by SPECFORGE_PROFILE_DIR.",
        "输出目录由 SPECFORGE_PROFILE_DIR 控制。"},
    UiTextEntry{
        "Stop the current recording before changing its output directory.",
        "请先停止当前录制，再更改其输出目录。"},
    UiTextEntry{
        "The profile output directory cannot be empty.",
        "性能分析输出目录不能为空。"},
    UiTextEntry{"Diagnostic details", "诊断详情"},
    UiTextEntry{"Experimental frame capture", "实验性画面捕获"},
    UiTextEntry{
        "Captures the requested frame from the main application viewport only. Detached viewport windows are excluded.",
        "仅从主应用视口捕获所请求的画面；不包含分离的视口窗口。"},
    UiTextEntry{"Capture Next Main Frame", "捕获下一主画面帧"},
    UiTextEntry{"Waiting for the next frame...", "正在等待下一帧……"},
    UiTextEntry{"PNG via synchronous GPU readback.", "通过同步 GPU 回读写入 PNG。"},
    UiTextEntry{"Output directory", "输出目录"},
    UiTextEntry{"unknown", "未知"},
    UiTextEntry{"file", "文件"},
    UiTextEntry{"folder", "文件夹"},
    UiTextEntry{"none", "无"},
    UiTextEntry{"error", "错误"},
    UiTextEntry{"loaded", "已加载"},
    UiTextEntry{"loaded with diagnostics", "已加载（含诊断）"},
    UiTextEntry{"not plottable", "无法绘图"},
    UiTextEntry{"plain", "普通"},
    UiTextEntry{"external", "外部"},
    UiTextEntry{"local", "本地"},
    UiTextEntry{"SpecForge", "SpecForge"},
    UiTextEntry{"File", "文件"},
    UiTextEntry{"Open File...", "打开文件…"},
    UiTextEntry{"Open Folder...", "打开文件夹…"},
    UiTextEntry{"Open File as Annotation...", "将文件作为标注打开…"},
    UiTextEntry{"View", "视图"},
    UiTextEntry{"Immersive Plot Mode", "沉浸式绘图模式"},
    UiTextEntry{"Show all panels", "显示所有面板"},
    UiTextEntry{"Files", "文件"},
    UiTextEntry{"Navigation", "导航"},
    UiTextEntry{"Annotations", "标注"},
    UiTextEntry{"Labeling", "标注任务"},
    UiTextEntry{"Sample Filters", "样本筛选"},
    UiTextEntry{"Sample Sorting", "样本排序"},
    UiTextEntry{"Smoothing", "平滑"},
    UiTextEntry{"Information", "信息"},
    UiTextEntry{"Spectral Lines", "谱线"},
    UiTextEntry{"Spectrum", "光谱"},
    UiTextEntry{"Sample name matches", "样本名称匹配"},
    UiTextEntry{"Ready", "就绪"},
    UiTextEntry{"Load failed", "加载失败"},
    UiTextEntry{
        "Background source loading failed.",
        "后台源加载失败。"},
    UiTextEntry{
        "The prepared source result is no longer applicable.",
        "已准备的源加载结果已不再适用。"},
    UiTextEntry{
        "The prepared source reuse target is no longer available.",
        "已准备源的复用目标已不可用。"},
    UiTextEntry{
        "The prepared known-source plan is no longer current.",
        "已准备的已知源加载计划已过期。"},
    UiTextEntry{
        "Prepared navigation no longer has a selectable final spectrum.",
        "已准备的导航已无可选择的最终光谱。"},
    UiTextEntry{"Loading source...", "正在加载源…"},
    UiTextEntry{"State save retrying", "正在重试保存状态"},
    UiTextEntry{"State warning", "状态警告"},
    UiTextEntry{"State recovered", "状态已恢复"},
    UiTextEntry{"Frame", "帧"},
    UiTextEntry{"Load failed (click to dismiss):", "加载失败（单击可忽略）："},
    UiTextEntry{"Performance recording", "正在录制性能诊断"},
    UiTextEntry{"Finishing recording...", "正在完成录制…"},
    UiTextEntry{"Start Recording", "开始录制"},
    UiTextEntry{"Stop Recording", "停止录制"},
    UiTextEntry{"Finishing Recording...", "正在完成录制…"},
    UiTextEntry{"REC  Performance", "REC  性能"},
    UiTextEntry{"Add source file", "添加源文件"},
    UiTextEntry{"Add source folder", "添加源文件夹"},
    UiTextEntry{"Spectrum sources", "光谱源"},
    UiTextEntry{"NumPy arrays", "NumPy 数组"},
    UiTextEntry{"CSV files", "CSV 文件"},
    UiTextEntry{"FITS files", "FITS 文件"},
    UiTextEntry{"All files", "所有文件"},
    UiTextEntry{"Open annotation file", "打开标注文件"},
    UiTextEntry{"NumPy annotation arrays", "NumPy 标注数组"},
    UiTextEntry{"Save labeling annotation", "保存标注结果"},
    UiTextEntry{"NumPy label arrays", "NumPy 标签数组"},
    UiTextEntry{
        "Choose performance profile output folder",
        "选择性能分析输出文件夹"},
    UiTextEntry{"General", "常规"},
    UiTextEntry{
        "Choose how SpecForge starts and restores your local workspace.",
        "设置 SpecForge 的启动方式与本地工作区恢复行为。"},
    UiTextEntry{"File opening", "文件打开"},
    UiTextEntry{
        "Restore the previous session at startup",
        "启动时恢复上次会话"},
    UiTextEntry{
        "Not available yet. Session restoration is currently managed automatically.",
        "暂不可用。会话恢复目前由应用自动管理。"},
    UiTextEntry{"Input", "输入"},
    UiTextEntry{
        "Tune mouse, touchpad, and keyboard behavior for spectrum inspection.",
        "调整光谱检视中的鼠标、触控板与键盘行为。"},
    UiTextEntry{
        "Live numeric navigation",
        "实时数值导航"},
    UiTextEntry{
        "When enabled, each valid 1-based source sample or sequence value is submitted while you type. Disable it to navigate only after Enter or focus loss.",
        "启用后，输入过程中每个有效的 1-based 源样本或序列数值都会立即提交；关闭后仅在按下 Enter 或输入框失去焦点时导航。"},
    UiTextEntry{
        "The saved input behavior could not be loaded; live numeric navigation is enabled.",
        "无法加载已保存的输入行为，当前已启用实时数值导航。"},
    UiTextEntry{
        "The input behavior could not be saved. The previous behavior is still in use.",
        "无法保存输入行为，仍继续使用此前的行为。"},
    UiTextEntry{"Mouse zoom sensitivity", "鼠标缩放灵敏度"},
    UiTextEntry{"Touchpad zoom sensitivity", "触控板缩放灵敏度"},
    UiTextEntry{"Reverse zoom direction", "反转缩放方向"},
    UiTextEntry{"View keyboard shortcuts", "查看键盘快捷键"},
    UiTextEntry{
        "Other input behavior settings are not available yet.",
        "其他输入行为设置暂不可用。"},
    UiTextEntry{"External source opening", "外部源打开"},
    UiTextEntry{
        "Open external FITS as a folder source",
        "将外部打开的 FITS 作为文件夹源打开"},
    UiTextEntry{
        "When enabled, an externally opened FITS file uses its containing folder as the source collection. In-app source-opening actions are unchanged.",
        "启用后，从外部打开 FITS 文件时，将其所在文件夹作为源集合。应用内的源打开操作不受影响。"},
    UiTextEntry{
        "Include subfolders (not implemented)",
        "包含子文件夹（尚未实现）"},
    UiTextEntry{
        "The saved external FITS folder preference could not be loaded; it is disabled.",
        "无法加载已保存的外部 FITS 文件夹偏好，当前已关闭。"},
    UiTextEntry{
        "The external FITS folder preference could not be saved. The previous behavior is still in use.",
        "无法保存外部 FITS 文件夹偏好，仍继续使用此前的行为。"},
    UiTextEntry{"Data & Recovery", "数据与恢复"},
    UiTextEntry{
        "Inspect local application storage. Scientific source files and label result files remain user-owned.",
        "查看应用的本地存储。科学源文件与标签结果文件始终归用户所有。"},
    UiTextEntry{"Application data", "应用数据"},
    UiTextEntry{"Open Data Folder", "打开数据文件夹"},
    UiTextEntry{"Copy Path", "复制路径"},
    UiTextEntry{"Configuration portability", "配置迁移"},
    UiTextEntry{"Import Settings...", "导入设置…"},
    UiTextEntry{"Export Settings...", "导出设置…"},
    UiTextEntry{
        "Not available yet. A public, versioned settings-file format has not been defined.",
        "暂不可用。尚未定义公开且带版本的设置文件格式。"},
    UiTextEntry{"Recovery and reset", "恢复与重置"},
    UiTextEntry{"Reset Window Layout", "重置窗口布局"},
    UiTextEntry{"Reset Application Settings", "重置应用设置"},
    UiTextEntry{"Erase All Application State...", "清除全部应用状态…"},
    UiTextEntry{
        "Not available yet. Reset operations need explicit data boundaries and confirmation behavior.",
        "暂不可用。重置操作需要明确的数据边界与确认流程。"},
    UiTextEntry{"Diagnostics", "诊断"},
    UiTextEntry{
        "Record bounded performance profiles for investigating interaction and loading latency.",
        "录制有界性能分析，用于排查交互与加载延迟。"},
    UiTextEntry{"Performance profile", "性能分析"},
    UiTextEntry{"Recording", "正在录制"},
    UiTextEntry{"Finishing...", "正在完成…"},
    UiTextEntry{"Not recording", "未录制"},
    UiTextEntry{
        "Automatically stops after 5 minutes or 100 MiB.",
        "达到 5 分钟或 100 MiB 后自动停止。"},
    UiTextEntry{"Current profile", "当前分析文件"},
    UiTextEntry{"Profile output directory", "性能分析输出目录"},
    UiTextEntry{"Source: storage-profile default", "来源：存储配置默认值"},
    UiTextEntry{"Source: saved setting", "来源：已保存设置"},
    UiTextEntry{
        "Source: SPECFORGE_PROFILE_DIR environment override",
        "来源：SPECFORGE_PROFILE_DIR 环境变量覆盖"},
    UiTextEntry{"Choose Folder...", "选择文件夹…"},
    UiTextEntry{"Restore Default", "恢复默认值"},
    UiTextEntry{"Open Output Folder", "打开输出文件夹"},
    UiTextEntry{
        "Remove SPECFORGE_PROFILE_DIR before changing this path in Settings.",
        "请先移除 SPECFORGE_PROFILE_DIR，再在“设置”中更改此路径。"},
    UiTextEntry{
        "Stop the current recording before changing its output directory.",
        "请先停止当前录制，再更改输出目录。"},
    UiTextEntry{
        "The saved profile output directory could not be loaded; using the current fallback directory.",
        "无法加载已保存的性能分析输出目录，当前使用回退目录。"},
    UiTextEntry{
        "The profile output directory could not be saved.",
        "无法保存性能分析输出目录。"},
    UiTextEntry{
        "The profile output directory could not be changed.",
        "无法更改性能分析输出目录。"},
    UiTextEntry{"About", "关于"},
    UiTextEntry{
        "Version, licensing, and diagnostic information for this build.",
        "当前构建的版本、许可与诊断信息。"},
    UiTextEntry{
        "Local astronomical spectrum inspection and labeling.",
        "本地天文光谱检视与标注。"},
    UiTextEntry{"Copyright (c) 2026 SpecForge.", "版权所有 (c) 2026 SpecForge。"},
    UiTextEntry{
        "Proprietary software. Use is subject to the SpecForge EULA.",
        "专有软件。使用须遵守 SpecForge 最终用户许可协议。"},
    UiTextEntry{"Version", "版本"},
    UiTextEntry{"Distribution", "分发方式"},
    UiTextEntry{"Configuration", "构建配置"},
    UiTextEntry{"Architecture", "体系结构"},
    UiTextEntry{"Source", "源码"},
    UiTextEntry{"Working tree", "工作树"},
    UiTextEntry{"HEAD", "HEAD"},
    UiTextEntry{"Graphics", "图形"},
    UiTextEntry{"Build details", "构建详情"},
    UiTextEntry{"Build metadata unavailable", "构建元数据不可用"},
    UiTextEntry{"Build metadata mismatch", "构建元数据不匹配"},
    UiTextEntry{"Compiler", "编译器"},
    UiTextEntry{"CMake", "CMake"},
    UiTextEntry{"Generator", "生成器"},
    UiTextEntry{"Windows SDK", "Windows SDK"},
    UiTextEntry{"Not reported", "未报告"},
    UiTextEntry{"Third-party components", "第三方组件"},
    UiTextEntry{
        "Dear ImGui %s (docking / Win32 / DirectX 11) - MIT License",
        "Dear ImGui %s（docking / Win32 / DirectX 11）— MIT 许可证"},
    UiTextEntry{
        "Dear ImGui (docking / Win32 / DirectX 11) - MIT License",
        "Dear ImGui（docking / Win32 / DirectX 11）— MIT 许可证"},
    UiTextEntry{"ImPlot %s - MIT License", "ImPlot %s — MIT 许可证"},
    UiTextEntry{"ImPlot - MIT License", "ImPlot — MIT 许可证"},
    UiTextEntry{"zlib %s - zlib License", "zlib %s — zlib 许可证"},
    UiTextEntry{"zlib - zlib License", "zlib — zlib 许可证"},
    UiTextEntry{
        "Modified stb headers bundled with Dear ImGui - MIT License",
        "Dear ImGui 随附的修改版 stb 头文件 — MIT 许可证"},
    UiTextEntry{"Legal documents", "法律文档"},
    UiTextEntry{
        "The complete license, notices, and data attributions are embedded in this executable and can be read here.",
        "完整许可条款、第三方声明与数据来源已内嵌于此可执行文件，可在此处阅读。"},
    UiTextEntry{"End User License Agreement", "最终用户许可协议"},
    UiTextEntry{"Third-Party Notices", "第三方声明"},
    UiTextEntry{"Data Sources", "数据来源"},
    UiTextEntry{"Copy Document", "复制文档"},
    UiTextEntry{"Close", "关闭"},
    UiTextEntry{
        "The embedded document is unavailable in this build.",
        "当前构建中无法读取内嵌文档。"},
    UiTextEntry{"Performance logs", "性能日志"},
    UiTextEntry{"Open Log Folder", "打开日志文件夹"},
    UiTextEntry{"Copy Diagnostic Information", "复制诊断信息"},
    UiTextEntry{"Source mode", "源码模式"},
    UiTextEntry{"Source revision", "源码修订"},
    UiTextEntry{"Data directory", "数据目录"},
    UiTextEntry{"Log directory", "日志目录"},
    UiTextEntry{"Could not prepare the data folder.", "无法准备数据文件夹。"},
    UiTextEntry{"Could not open the data folder.", "无法打开数据文件夹。"},
    UiTextEntry{"Opened the data folder.", "已打开数据文件夹。"},
    UiTextEntry{"Data path copied.", "已复制数据路径。"},
    UiTextEntry{
        "Could not prepare the profile output folder.",
        "无法准备性能分析输出文件夹。"},
    UiTextEntry{
        "Could not open the profile output folder.",
        "无法打开性能分析输出文件夹。"},
    UiTextEntry{
        "Opened the profile output folder.",
        "已打开性能分析输出文件夹。"},
    UiTextEntry{"Profile output path copied.", "已复制性能分析输出路径。"},
    UiTextEntry{"Could not prepare the log folder.", "无法准备日志文件夹。"},
    UiTextEntry{"Could not open the log folder.", "无法打开日志文件夹。"},
    UiTextEntry{"Opened the log folder.", "已打开日志文件夹。"},
    UiTextEntry{"Diagnostic information copied.", "已复制诊断信息。"},
    UiTextEntry{"SpecForge startup error", "SpecForge 启动错误"},
    UiTextEntry{"Unknown startup error.", "未知启动错误。"},
    UiTextEntry{"SpecForge - Local state warning", "SpecForge - 本地状态警告"},
    UiTextEntry{
        "SpecForge could not save all local state before exiting.",
        "SpecForge 退出前无法保存全部本地状态。"},
    UiTextEntry{"Unsaved areas:", "未保存的区域："},
    UiTextEntry{"Application settings", "应用设置"},
    UiTextEntry{"Source session", "源会话"},
    UiTextEntry{"Sample navigation", "样本导航"},
    UiTextEntry{"Sample labeling", "样本标注"},
    UiTextEntry{"Sample workflow", "样本工作流"},
    UiTextEntry{"Spectral-line state", "谱线状态"},
    UiTextEntry{
        "Changes in these areas may not be restored the next time SpecForge starts.",
        "这些区域中的更改可能无法在 SpecForge 下次启动时恢复。"},
    UiTextEntry{
        "Use Settings > Diagnostics to record.",
        "请在“设置”>“诊断”中开始录制。"},
    UiTextEntry{
        "Recording started by SPECFORGE_PROFILE.",
        "已由 SPECFORGE_PROFILE 启动录制。"},
    UiTextEntry{"Could not start recording: ", "无法开始录制："},
    UiTextEntry{"Recording performance diagnostics.", "正在录制性能诊断。"},
    UiTextEntry{
        " events dropped under recorder queue pressure.",
        " 个事件因录制队列压力而丢失。"},
    UiTextEntry{"Recording failed: ", "录制失败："},
    UiTextEntry{
        "Recording failed while writing the log.",
        "写入日志时录制失败。"},
    UiTextEntry{
        "Recording saved after reaching the 5-minute limit.",
        "达到 5 分钟上限后，录制已保存。"},
    UiTextEntry{
        "Recording saved after reaching the 100 MiB limit.",
        "达到 100 MiB 上限后，录制已保存。"},
    UiTextEntry{"Recording saved.", "录制已保存。"},
    UiTextEntry{"Recording stopped.", "录制已停止。"},
    UiTextEntry{" events were dropped.", " 个事件已丢失。"},
    UiTextEntry{
        "Ready. Captures are synchronous and may disturb performance measurements.",
        "就绪。捕获采用同步方式，可能干扰性能测量。"},
    UiTextEntry{
        "Experimental frame capture is not enabled.",
        "实验性画面捕获未启用。"},
    UiTextEntry{
        "Capture unavailable while the main window is minimized or hidden.",
        "主窗口最小化或隐藏时无法捕获画面。"},
    UiTextEntry{
        "Capture requested. Waiting for the next successfully drawn main frame.",
        "已请求捕获，正在等待下一次成功绘制的主画面帧。"},
    UiTextEntry{
        "Captured the requested main application frame.",
        "已捕获所请求的主应用画面帧。"},
    UiTextEntry{
        "Frame capture failed; no image was produced.",
        "画面捕获失败，未生成图像。"},
    UiTextEntry{
        "Frame capture failed while preparing the output directory; no image was produced.",
        "准备输出目录时画面捕获失败，未生成图像。"},
    UiTextEntry{
        "Frame capture failed at %s (%s); no image was produced.",
        "画面捕获在 %s 处失败（%s），未生成图像。"},
    UiTextEntry{"Panel visibility", "面板可见性"},
    UiTextEntry{"Add file...", "添加文件…"},
    UiTextEntry{"Add folder...", "添加文件夹…"},
    UiTextEntry{"source", "个源"},
    UiTextEntry{"sources", "个源"},
    UiTextEntry{
        "No sources added in this session.",
        "此会话中尚未添加源。"},
    UiTextEntry{"Source", "源"},
    UiTextEntry{"Type", "类型"},
    UiTextEntry{"State", "状态"},
    UiTextEntry{"Remove from list", "从列表中移除"},
    UiTextEntry{"No active source", "无活动源"},
    UiTextEntry{"source sample:", "源样本："},
    UiTextEntry{
        "Previous sample (Left Arrow)",
        "上一个样本（左方向键）"},
    UiTextEntry{
        "Next sample (Right Arrow)",
        "下一个样本（右方向键）"},
    UiTextEntry{"sequence:", "序列："},
    UiTextEntry{"name:", "名称："},
    UiTextEntry{"No read-only annotations", "无只读标注"},
    UiTextEntry{"Display name", "显示名称"},
    UiTextEntry{"Value", "值"},
    UiTextEntry{"(missing)", "（缺失）"},
    UiTextEntry{"(output missing)", "（输出缺失）"},
    UiTextEntry{"(metadata missing)", "（元数据缺失）"},
    UiTextEntry{"(metadata ignored)", "（已忽略元数据）"},
    UiTextEntry{"No plottable spectrum", "无可绘制光谱"},
    UiTextEntry{"Show smoothed curve", "显示平滑曲线"},
    UiTextEntry{"Method", "方法"},
    UiTextEntry{"None", "无"},
    UiTextEntry{"Gaussian", "高斯"},
    UiTextEntry{"Median", "中值"},
    UiTextEntry{"Sigma", "标准差 σ"},
    UiTextEntry{"Kernel size", "核大小"},
    UiTextEntry{"Effective kernel:", "有效核大小："},
    UiTextEntry{
        "No smoothing method selected",
        "未选择平滑方法"},
    UiTextEntry{"Show raw overlay", "显示原始曲线叠加"},
    UiTextEntry{"raw spectrum", "原始光谱"},
    UiTextEntry{"Gaussian smoothing", "高斯平滑"},
    UiTextEntry{"Median smoothing", "中值平滑"},
    UiTextEntry{"current spectrum", "当前光谱"},
    UiTextEntry{"Name", "名称"},
    UiTextEntry{"Points", "数据点"},
    UiTextEntry{"none", "无"},
    UiTextEntry{"unknown", "未知"},
    UiTextEntry{"Wavelength medium", "波长介质"},
    UiTextEntry{"Observer correction", "观测者修正"},
    UiTextEntry{"Radial velocity", "径向速度"},
    UiTextEntry{"RV source", "径向速度来源"},
    UiTextEntry{"Redshift", "红移"},
    UiTextEntry{"Redshift warning", "红移警告"},
    UiTextEntry{"Target z", "目标 z"},
    UiTextEntry{"Target z source", "目标 z 来源"},
    UiTextEntry{"Target z status", "目标 z 状态"},
    UiTextEntry{"Target z warning", "目标 z 警告"},
    UiTextEntry{"Heliocentric correction", "日心修正"},
    UiTextEntry{"Target rest frame", "目标静止系"},
    UiTextEntry{"Rest-frame correction", "静止系修正"},
    UiTextEntry{"not applied", "未应用"},
    UiTextEntry{"available, not applied", "可用，但未应用"},
    UiTextEntry{
        "unreliable, not applied",
        "不可靠，未应用"},
    UiTextEntry{
        "RV / c low-speed approximation",
        "径向速度 / c 低速近似"},
    UiTextEntry{"pipeline redshift", "流水线红移"},
    UiTextEntry{"ZWARNING nonzero", "ZWARNING 非零"},
    UiTextEntry{
        "invalid pipeline redshift",
        "无效的流水线红移"},
    UiTextEntry{"No snapshot", "无快照"},
    UiTextEntry{"Fit view", "适配视图"},
    UiTextEntry{"Show points", "显示数据点"},
    UiTextEntry{"Diagnostics", "诊断"},
    UiTextEntry{"No diagnostics", "无诊断信息"},
    UiTextEntry{"info", "信息"},
    UiTextEntry{"warning", "警告"},
    UiTextEntry{"error", "错误"},
    UiTextEntry{"unknown", "未知"},
    UiTextEntry{
        "Ignored sample-name file %s.",
        "已忽略样本名称文件 %s。"},
    UiTextEntry{
        "Ignored annotation file %s.",
        "已忽略标注文件 %s。"},
    UiTextEntry{
        "Ignored metadata file %s.",
        "已忽略元数据文件 %s。"},
    UiTextEntry{
        "Use annotation as labeling task?",
        "将标注用作标注任务？"},
    UiTextEntry{
        "Add sample filter source",
        "添加样本筛选源"},
    UiTextEntry{
        "Add sample sort source",
        "添加样本排序源"},
    UiTextEntry{
        "Delete labeling task?",
        "删除标注任务？"},
    UiTextEntry{"Delete label?", "删除标签？"},
    UiTextEntry{
        "Change used label code?",
        "更改已使用的标签代码？"},
    UiTextEntry{"Ascending", "升序"},
    UiTextEntry{"Descending", "降序"},
    UiTextEntry{
        "Remove sample sorting",
        "移除样本排序"},
    UiTextEntry{
        "internal autosave draft",
        "内部自动保存草稿"},
    UiTextEntry{
        "autosaved to output",
        "已自动保存到输出"},
    UiTextEntry{"pending", "等待保存"},
    UiTextEntry{"save failed", "保存失败"},
    UiTextEntry{
        "Output path is already used by another local labeling task.",
        "该输出路径已被另一个本地标注任务使用。"},
    UiTextEntry{
        "Could not save labeling output.",
        "无法保存标注输出。"},
    UiTextEntry{
        SampleLabelingIssueTextFor(
            SampleLabelingOperationResult::Issue::
                EditLeaseUnavailable).english,
        SampleLabelingIssueTextFor(
            SampleLabelingOperationResult::Issue::
                EditLeaseUnavailable).simplified_chinese},
    UiTextEntry{
        SampleLabelingIssueTextFor(
            SampleLabelingOperationResult::Issue::
                EditLeaseFailed).english,
        SampleLabelingIssueTextFor(
            SampleLabelingOperationResult::Issue::
                EditLeaseFailed).simplified_chinese},
    UiTextEntry{
        SampleLabelingIssueTextFor(
            SampleLabelingOperationResult::Issue::
                EditTargetChanged).english,
        SampleLabelingIssueTextFor(
            SampleLabelingOperationResult::Issue::
                EditTargetChanged).simplified_chinese},
    UiTextEntry{
        "State: temporary local draft; use Save to... to create a labeling annotation.",
        "状态：临时本地草稿；使用“另存为…”创建标注结果。"},
    UiTextEntry{
        "State: output file and metadata sidecar are saved.",
        "状态：输出文件与元数据附属文件均已保存。"},
    UiTextEntry{
        "State: output/metadata autosave is pending; close is disabled until it finishes.",
        "状态：正在自动保存输出/元数据；完成前无法关闭任务。"},
    UiTextEntry{
        "State: Save to... failed; choose this or another output, or pause the recoverable draft.",
        "状态：“另存为…”失败；请选择此输出或其他输出，或暂停此可恢复草稿。"},
    UiTextEntry{
        "State: output/metadata autosave failed; close is disabled until the save succeeds.",
        "状态：自动保存输出/元数据失败；保存成功前无法关闭任务。"},
    UiTextEntry{
        "State: unknown save state.",
        "状态：保存状态未知。"},
    UiTextEntry{"Pause", "暂停"},
    UiTextEntry{"Delete", "删除"},
    UiTextEntry{
        "Select labeling task",
        "选择标注任务"},
    UiTextEntry{
        "New labeling task",
        "新建标注任务"},
    UiTextEntry{
        "Temporary labeling task",
        "临时标注任务"},
    UiTextEntry{
        "Temporary labeling draft",
        "临时标注草稿"},
    UiTextEntry{
        "Resume labeling draft",
        "继续标注草稿"},
    UiTextEntry{
        "Output autosave must finish before this task can be closed.",
        "必须等待输出自动保存完成，才能关闭此任务。"},
    UiTextEntry{
        "Output autosave must finish before this task can be deleted.",
        "必须等待输出自动保存完成，才能删除此任务。"},
    UiTextEntry{
        "Make \"%s\" editable in Labeling. Future autosaves will write to this annotation result and its metadata sidecar.",
        "使“%s”可在“标注任务”中编辑。此后的自动保存将写入该标注结果及其元数据附属文件。"},
    UiTextEntry{
        "This edits the selected annotation result in place. Back up the file first if you need to preserve the original labels.",
        "此操作会直接修改所选标注结果。若需保留原标签，请先备份文件。"},
    UiTextEntry{
        "No metadata sidecar is present; one will be created on save.",
        "不存在元数据附属文件；保存时将创建。"},
    UiTextEntry{
        "Existing label metadata will be reused.",
        "将复用现有标签元数据。"},
    UiTextEntry{"Use annotation", "使用此标注"},
    UiTextEntry{"Cancel", "取消"},
    UiTextEntry{
        "Delete local task \"%s\". Output files are not deleted.",
        "删除本地任务“%s”。不会删除输出文件。"},
    UiTextEntry{"Delete task", "删除任务"},
    UiTextEntry{
        "Progress: %llu labeled / %llu",
        "进度：已标注 %llu / %llu"},
    UiTextEntry{"Current: %s", "当前：%s"},
    UiTextEntry{
        "Remembered row: %llu",
        "记忆行：%llu"},
    UiTextEntry{"Resume", "继续"},
    UiTextEntry{"Save: %s", "保存：%s"},
    UiTextEntry{
        "Local task record: %s",
        "本地任务记录：%s"},
    UiTextEntry{"Auto-advance", "自动前进"},
    UiTextEntry{"Skip labeled", "跳过已标注样本"},
    UiTextEntry{"Save to...", "另存为…"},
    UiTextEntry{"Labels", "标签"},
    UiTextEntry{"Add label", "添加标签"},
    UiTextEntry{"Label %d", "标签 %d"},
    UiTextEntry{"Code", "代码"},
    UiTextEntry{"Shortcut", "快捷键"},
    UiTextEntry{"Press key...", "按键…"},
    UiTextEntry{
        "Press A-Z or 0-9. Backspace clears the binding; Escape cancels.",
        "按 A–Z 或 0–9。Backspace 清除绑定，Escape 取消。"},
    UiTextEntry{
        "Waiting for an unmodified letter or digit",
        "正在等待不带修饰键的字母或数字"},
    UiTextEntry{
        "Capture a label shortcut",
        "捕获标签快捷键"},
    UiTextEntry{"Clear", "清除"},
    UiTextEntry{
        "The shortcut will be unbound when this label is saved.",
        "保存此标签时将解除该快捷键绑定。"},
    UiTextEntry{
        "Only unmodified A-Z and 0-9 keys can be assigned.",
        "只能分配不带修饰键的 A–Z 和 0–9。"},
    UiTextEntry{
        "Shortcut %s selected. Save the label to apply it.",
        "已选择快捷键 %s。保存标签后生效。"},
    UiTextEntry{
        "Shortcut %s will move from %s when this label is saved.",
        "保存此标签时，快捷键 %s 将从“%s”移至此处。"},
    UiTextEntry{
        "%s is assigned to %s. Press %s again to move it.",
        "%s 已分配给“%s”。再次按 %s 可将其移至此处。"},
    UiTextEntry{"Name is required", "名称不能为空"},
    UiTextEntry{
        "Code must be an integer",
        "代码必须是整数"},
    UiTextEntry{
        "Code -1 is reserved for unlabeled samples",
        "代码 -1 保留给未标注样本"},
    UiTextEntry{
        "Code %d is already used by a label or sample value",
        "代码 %d 已被标签或样本值使用"},
    UiTextEntry{
        "Shortcut must be one letter or digit",
        "快捷键必须是一个字母或数字"},
    UiTextEntry{
        "Changing this code rewrites %llu assigned sample value(s)",
        "更改此代码将重写 %llu 个已分配样本值"},
    UiTextEntry{
        "Saving moves this shortcut from %s",
        "保存后会将此快捷键从“%s”移至当前标签"},
    UiTextEntry{"Save label", "保存标签"},
    UiTextEntry{"Cancel editing", "取消编辑"},
    UiTextEntry{"Edit label", "编辑标签"},
    UiTextEntry{"Delete label", "删除标签"},
    UiTextEntry{
        "Delete label and clear %llu sample(s)",
        "删除标签并清除 %llu 个样本"},
    UiTextEntry{
        "Label code %d is assigned to %llu sample(s).",
        "标签代码 %d 已分配给 %llu 个样本。"},
    UiTextEntry{
        "Changing it to %d will rewrite every assigned sample value.",
        "将其更改为 %d 会重写所有已分配的样本值。"},
    UiTextEntry{"Change code", "更改代码"},
    UiTextEntry{
        "Label \"%s\" is assigned to %llu sample(s).",
        "标签“%s”已分配给 %llu 个样本。"},
    UiTextEntry{
        "Deleting it will change those values to Unlabeled (-1) and remove the label definition.",
        "删除后，这些值将改为“未标注（-1）”，并移除标签定义。"},
    UiTextEntry{
        "Its selected sample-filter value will also be removed, which may move the current sample.",
        "同时还会移除其选中的样本筛选值，当前样本可能因此移动。"},
    UiTextEntry{
        "Add annotation sample filter",
        "添加基于标注的样本筛选"},
    UiTextEntry{
        "No available annotations",
        "无可用标注"},
    UiTextEntry{
        "Reset sample filters",
        "重置样本筛选"},
    UiTextEntry{
        "Visible: %llu / %llu",
        "可见：%llu / %llu"},
    UiTextEntry{
        "Current sample is outside the active sample filters",
        "当前样本不在活动样本筛选范围内"},
    UiTextEntry{
        "Ignored a filter because its source is not loaded.",
        "由于源未加载，已忽略一个筛选条件。"},
    UiTextEntry{
        "Ignored %s because it is not filterable.",
        "由于“%s”不可筛选，已忽略该条件。"},
    UiTextEntry{
        "Ignored %s because its sample count changed.",
        "由于“%s”的样本数已变化，已忽略该条件。"},
    UiTextEntry{
        "No sample filters",
        "无样本筛选"},
    UiTextEntry{
        "Remove sample filter",
        "移除样本筛选"},
    UiTextEntry{
        "Add annotation sample sorting",
        "添加基于标注的样本排序"},
    UiTextEntry{"Reset sorting", "重置排序"},
    UiTextEntry{"Source order", "源顺序"},
    UiTextEntry{"Sample name", "样本名称"},
    UiTextEntry{
        "No comparable sort sources",
        "无可比较的排序源"},
    UiTextEntry{"Unlabeled", "未标注"},
    UiTextEntry{"Public catalog", "公共目录"},
    UiTextEntry{
        "Current snapshot does not expose a wavelength axis for spectral-line overlays.",
        "当前快照未提供可用于谱线叠加的波长轴。"},
    UiTextEntry{
        "Wavelength frame is unknown; rest-frame overlays are reference-only.",
        "波长参考系未知；静止系叠加仅供参考。"},
    UiTextEntry{"Catalog", "目录"},
    UiTextEntry{"Catalog load failed: ", "目录加载失败："},
    UiTextEntry{
        "No public catalog markers loaded.",
        "未加载公共目录标记。"},
    UiTextEntry{
        "Could not read spectral-line grouping cache.",
        "无法读取谱线分组缓存。"},
    UiTextEntry{
        "Ignored invalid spectral-line grouping cache.",
        "已忽略无效的谱线分组缓存。"},
    UiTextEntry{
        "Ignored unsupported spectral-line grouping cache.",
        "已忽略不受支持的谱线分组缓存。"},
    UiTextEntry{
        "Could not save spectral-line grouping cache. Retrying.",
        "无法保存谱线分组缓存，正在重试。"},
    UiTextEntry{
        "Spectral-line state persistence recovered.",
        "谱线状态持久化已恢复。"},
    UiTextEntry{"Search", "搜索"},
    UiTextEntry{
        "id, label, catalog group, or plot label",
        "ID、名称、目录分组或绘图标签"},
    UiTextEntry{"Duplicate", "创建副本"},
    UiTextEntry{
        "Duplicate as user view",
        "复制为用户视图"},
    UiTextEntry{"Rename", "重命名"},
    UiTextEntry{"Rename grouping view", "重命名分组视图"},
    UiTextEntry{"Delete grouping view", "删除分组视图"},
    UiTextEntry{
        "Delete grouping view \"%s\"?",
        "删除分组视图“%s”？"},
    UiTextEntry{
        "Catalog markers and marker visibility are not deleted.",
        "不会删除目录标记及其可见性设置。"},
    UiTextEntry{
        "This catalog has no catalog grouping view.",
        "此目录没有目录分组视图。"},
    UiTextEntry{"+ New grouping view", "+ 新建分组视图"},
    UiTextEntry{
        "New user grouping view",
        "新建用户分组视图"},
    UiTextEntry{"Catalog grouping view", "目录分组视图"},
    UiTextEntry{"Grouping ", "分组视图 "},
    UiTextEntry{" copy", " 副本"},
    UiTextEntry{"+ Group", "+ 分组"},
    UiTextEntry{
        "%zu plot-visible / %zu catalog markers",
        "绘图中可见 %zu 个 / 目录共 %zu 个标记"},
    UiTextEntry{
        "No groups in this view.",
        "此视图中没有分组。"},
    UiTextEntry{
        "Search is filtering this group; bulk visibility is disabled.",
        "搜索正在筛选此分组；批量可见性已禁用。"},
    UiTextEntry{
        "No resolved markers in this group.",
        "此分组中没有已解析的标记。"},
    UiTextEntry{
        "Show or hide all resolved markers in this group.",
        "显示或隐藏此分组中的所有已解析标记。"},
    UiTextEntry{
        "Shared marker reference: this marker also appears in another group in this view.",
        "共享标记引用：此标记也出现在该视图的其他分组中。"},
    UiTextEntry{"Drop: copy", "拖放：复制"},
    UiTextEntry{
        "Drop: move, Ctrl+drop: copy",
        "拖放：移动，按住 Ctrl 拖放：复制"},
    UiTextEntry{
        "Drop between groups to reorder",
        "拖放到分组之间以重新排序"},
    UiTextEntry{"Disband group", "解散分组"},
    UiTextEntry{"Show on plot", "在绘图中显示"},
    UiTextEntry{"unresolved", "未解析"},
    UiTextEntry{
        "Unresolved marker references are not plotted.",
        "未解析的标记引用不会绘制。"},
    UiTextEntry{"Copy to group", "复制到分组"},
    UiTextEntry{"No other groups", "没有其他分组"},
    UiTextEntry{"Remove from this group", "从此分组中移除"},
    UiTextEntry{"Rename group", "重命名分组"},
    UiTextEntry{"Unassigned", "未分组"},
    UiTextEntry{"Group ", "分组 "},
};

static_assert(kTextCatalog.size() == kUiTextCount);
static_assert(kUiLanguageCount == 2);

}  // namespace

std::string_view UiText(UiLanguage language, UiTextId text_id) noexcept
{
    const std::size_t text_index = static_cast<std::size_t>(text_id);
    if (text_index >= kUiTextCount) {
        return {};
    }

    const UiTextEntry& text = kTextCatalog[text_index];
    if (language == UiLanguage::SimplifiedChinese &&
        !text.simplified_chinese.empty()) {
        return text.simplified_chinese;
    }
    return text.english;
}

std::string StableUiLabel(
    UiLanguage language,
    UiTextId text_id,
    std::string_view stable_id)
{
    std::string label(UiText(language, text_id));
    label += "###";
    label += stable_id;
    return label;
}

std::string_view SourceTypeDisplayText(
    UiLanguage language,
    std::string_view source_type) noexcept
{
    if (source_type == "file") {
        return UiText(
            language,
            UiTextId::FileSourceType);
    }
    if (source_type == "folder") {
        return UiText(
            language,
            UiTextId::FolderSourceType);
    }
    return source_type;
}

std::string_view UiText(
    UiLanguage language,
    SourceCollectionLoadErrorKind error_kind) noexcept
{
    switch (error_kind) {
    case SourceCollectionLoadErrorKind::
        BackgroundLoadingFailed:
        return UiText(
            language,
            UiTextId::
                BackgroundSourceLoadingFailed);
    case SourceCollectionLoadErrorKind::
        PreparedResultNotApplicable:
        return UiText(
            language,
            UiTextId::
                PreparedSourceResultNotApplicable);
    case SourceCollectionLoadErrorKind::
        PreparedReuseTargetUnavailable:
        return UiText(
            language,
            UiTextId::
                PreparedSourceReuseTargetUnavailable);
    case SourceCollectionLoadErrorKind::
        PreparedKnownSourcePlanStale:
        return UiText(
            language,
            UiTextId::
                PreparedKnownSourcePlanStale);
    case SourceCollectionLoadErrorKind::
        PreparedNavigationUnavailable:
        return UiText(
            language,
            UiTextId::
                PreparedNavigationUnavailable);
    case SourceCollectionLoadErrorKind::None:
        break;
    }
    return {};
}

std::string FormatSourceCollectionLoadFailures(
    UiLanguage language,
    std::span<const SourceCollectionLoadFailure>
        failures)
{
    const std::string_view separator =
        language == UiLanguage::SimplifiedChinese
            ? std::string_view{"："}
            : std::string_view{": "};
    std::string result;
    for (const SourceCollectionLoadFailure& failure :
         failures) {
        if (!result.empty()) {
            result += "\n\n";
        }
        const auto source_path =
            failure.source_path.u8string();
        if (!source_path.empty()) {
            result.append(
                reinterpret_cast<const char*>(
                    source_path.data()),
                source_path.size());
            result += separator;
        }
        result += UiText(
            language,
            failure.error.kind);
        if (!failure.error
                 .diagnostic_detail.empty()) {
            result.push_back('\n');
            result += UiText(
                language,
                UiTextId::DiagnosticDetails);
            result += separator;
            result += failure.error
                          .diagnostic_detail;
        }
    }
    return result;
}

std::string_view UiText(
    UiLanguage language,
    SourceCollectionSourceState state) noexcept
{
    switch (state) {
    case SourceCollectionSourceState::Unavailable:
        return UiText(language, UiTextId::SourceStateUnavailable);
    case SourceCollectionSourceState::Error:
        return UiText(language, UiTextId::SourceStateError);
    case SourceCollectionSourceState::Loaded:
        return UiText(language, UiTextId::SourceStateLoaded);
    case SourceCollectionSourceState::LoadedWithDiagnostics:
        return UiText(
            language,
            UiTextId::SourceStateLoadedWithDiagnostics);
    case SourceCollectionSourceState::NotPlottable:
        return UiText(language, UiTextId::SourceStateNotPlottable);
    }
    return UiText(language, UiTextId::SourceStateUnavailable);
}

std::string_view UiText(
    UiLanguage language,
    SampleAnnotationWorkflowRelationship relationship) noexcept
{
    switch (relationship) {
    case SampleAnnotationWorkflowRelationship::PlainAnnotation:
        return UiText(
            language,
            UiTextId::AnnotationRelationshipPlain);
    case SampleAnnotationWorkflowRelationship::ExternalLabelResult:
        return UiText(
            language,
            UiTextId::AnnotationRelationshipExternal);
    case SampleAnnotationWorkflowRelationship::LocalLabelingTask:
        return UiText(
            language,
            UiTextId::AnnotationRelationshipLocal);
    }
    return UiText(language, UiTextId::AnnotationRelationshipPlain);
}

std::string_view UiText(
    UiLanguage language,
    SampleLabelSaveMessageKind message_kind) noexcept
{
    switch (message_kind) {
    case SampleLabelSaveMessageKind::OutputPathAlreadyUsed:
        return UiText(
            language,
            UiTextId::OutputPathAlreadyUsed);
    case SampleLabelSaveMessageKind::OutputSaveFailed:
        return UiText(
            language,
            UiTextId::CouldNotSaveLabelingOutput);
    case SampleLabelSaveMessageKind::SystemDetail:
    case SampleLabelSaveMessageKind::None:
    default:
        return {};
    }
}

UiTextId LabelingIssueTextId(int issue_value) noexcept
{
    return SampleLabelingIssueTextForValue(issue_value).text_id;
}

std::string_view UiText(
    UiLanguage language,
    LocalUserStateArea area) noexcept
{
    switch (area) {
    case LocalUserStateArea::SourceSession:
        return UiText(
            language,
            UiTextId::SourceSessionArea);
    case LocalUserStateArea::SampleNavigation:
        return UiText(
            language,
            UiTextId::SampleNavigationArea);
    case LocalUserStateArea::SampleLabeling:
        return UiText(
            language,
            UiTextId::SampleLabelingArea);
    case LocalUserStateArea::SampleWorkflow:
        return UiText(
            language,
            UiTextId::SampleWorkflowArea);
    case LocalUserStateArea::Language:
        return UiText(language, UiTextId::Language);
    case LocalUserStateArea::UiScale:
        return UiText(language, UiTextId::UiScale);
    case LocalUserStateArea::Input:
        return UiText(language, UiTextId::Input);
    case LocalUserStateArea::ExternalSource:
        return UiText(
            language,
            UiTextId::ExternalSourceOpening);
    case LocalUserStateArea::ProfileOutputDirectory:
        return UiText(
            language,
            UiTextId::ProfileOutputDirectory);
    case LocalUserStateArea::PanelVisibility:
        return UiText(
            language,
            UiTextId::PanelVisibility);
    case LocalUserStateArea::SpectralLines:
        return UiText(
            language,
            UiTextId::SpectralLineStateArea);
    }
    return {};
}

std::string FormatLocalUserStateHealthMessage(
    UiLanguage language,
    const LocalUserStateHealthMessage& message)
{
    UiTextId state_text = UiTextId::StateWarning;
    switch (message.kind) {
    case LocalUserStateHealthMessageKind::LoadWarning:
    case LocalUserStateHealthMessageKind::SaveWarning:
        break;
    case LocalUserStateHealthMessageKind::SaveRetrying:
        state_text = UiTextId::StateSaveRetrying;
        break;
    case LocalUserStateHealthMessageKind::Recovered:
        state_text = UiTextId::StateRecovered;
        break;
    }

    const std::string_view separator =
        language == UiLanguage::SimplifiedChinese
            ? std::string_view{"："}
            : std::string_view{": "};
    std::string result(UiText(language, message.area));
    result += separator;
    result += UiText(language, state_text);
    if (!message.diagnostic_detail.empty()) {
        result.push_back('\n');
        result += UiText(
            language,
            UiTextId::DiagnosticDetails);
        result += separator;
        result += message.diagnostic_detail;
    }
    return result;
}

}  // namespace specforge
