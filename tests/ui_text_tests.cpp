#include "domain/sample_annotation_io.h"
#include "domain/sample_labeling.h"
#include "ui/source_collection_session_types.h"
#include "ui/ui_text.h"

#include <array>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

bool IsPrintfConversion(char character)
{
    constexpr std::string_view kConversions =
        "diuoxXfFeEgGaAcspn";
    return kConversions.find(character) !=
           std::string_view::npos;
}

std::vector<std::string> PrintfPlaceholderSignature(
    std::string_view text)
{
    std::vector<std::string> signature;
    for (std::size_t index = 0;
         index < text.size();
         ++index) {
        if (text[index] != '%' ||
            index + 1 >= text.size()) {
            continue;
        }

        std::size_t cursor = index + 1;
        if (text[cursor] == '%') {
            index = cursor;
            continue;
        }

        while (cursor < text.size() &&
               std::string_view("-+0#").find(
                   text[cursor]) !=
                   std::string_view::npos) {
            ++cursor;
        }
        while (cursor < text.size() &&
               text[cursor] >= '0' &&
               text[cursor] <= '9') {
            ++cursor;
        }
        if (cursor < text.size() &&
            text[cursor] == '*') {
            signature.emplace_back("*");
            ++cursor;
        }
        if (cursor < text.size() &&
            text[cursor] == '.') {
            ++cursor;
            if (cursor < text.size() &&
                text[cursor] == '*') {
                signature.emplace_back("*");
                ++cursor;
            } else {
                while (cursor < text.size() &&
                       text[cursor] >= '0' &&
                       text[cursor] <= '9') {
                    ++cursor;
                }
            }
        }

        const std::size_t length_begin = cursor;
        if (cursor < text.size() &&
            (text[cursor] == 'h' ||
             text[cursor] == 'l')) {
            const char length = text[cursor++];
            if (cursor < text.size() &&
                text[cursor] == length) {
                ++cursor;
            }
        } else if (
            cursor < text.size() &&
            std::string_view("jztL").find(
                text[cursor]) !=
                std::string_view::npos) {
            ++cursor;
        }

        if (cursor >= text.size() ||
            !IsPrintfConversion(text[cursor])) {
            continue;
        }
        signature.emplace_back(
            text.substr(
                length_begin,
                cursor - length_begin + 1));
        index = cursor;
    }
    return signature;
}

void TestEveryDisplayTextIsPresent()
{
    constexpr std::size_t kLanguageCount =
        static_cast<std::size_t>(specforge::UiLanguage::Count);
    constexpr std::size_t kTextCount =
        static_cast<std::size_t>(specforge::UiTextId::Count);

    for (std::size_t language_index = 0; language_index < kLanguageCount; ++language_index) {
        const auto language = static_cast<specforge::UiLanguage>(language_index);
        for (std::size_t index = 0; index < kTextCount; ++index) {
            const auto text_id = static_cast<specforge::UiTextId>(index);
            Require(!specforge::UiText(language, text_id).empty(), "display text should not be empty");
        }
    }
}

void TestLocalizedPrintfPlaceholderSignaturesMatch()
{
    constexpr std::size_t kTextCount =
        static_cast<std::size_t>(
            specforge::UiTextId::Count);
    for (std::size_t index = 0;
         index < kTextCount;
         ++index) {
        const auto text_id =
            static_cast<specforge::UiTextId>(
                index);
        const std::vector<std::string>
            english_signature =
                PrintfPlaceholderSignature(
                    specforge::UiText(
                        specforge::UiLanguage::
                            English,
                        text_id));
        const std::vector<std::string>
            chinese_signature =
                PrintfPlaceholderSignature(
                    specforge::UiText(
                        specforge::UiLanguage::
                            SimplifiedChinese,
                        text_id));
        Require(
            english_signature ==
                chinese_signature,
            "localized printf placeholder signatures should match");
    }
}

void TestRepresentativeMappingsAreExact()
{
    using specforge::UiLanguage;
    using specforge::UiText;
    using specforge::UiTextId;

    Require(UiText(UiLanguage::English, UiTextId::Language) == "Language", "English language label");
    Require(UiText(UiLanguage::SimplifiedChinese, UiTextId::Language) == "语言", "Chinese language label");
    Require(
        UiText(UiLanguage::English, UiTextId::EnglishLanguageName) == "English",
        "English name in English");
    Require(
        UiText(UiLanguage::SimplifiedChinese, UiTextId::EnglishLanguageName) == "英语",
        "English name in Chinese");
    Require(
        UiText(UiLanguage::English, UiTextId::SimplifiedChineseLanguageName) ==
            "Simplified Chinese",
        "Chinese name in English");
    Require(
        UiText(UiLanguage::SimplifiedChinese, UiTextId::SimplifiedChineseLanguageName) ==
            "简体中文",
        "Chinese name in Chinese");
    Require(
        UiText(UiLanguage::English, UiTextId::LocalizationInProgress) ==
            "The application shell and Settings use the selected language. Specialized scientific tools may still remain in English.",
        "English scope notice should be exact");
    Require(
        UiText(UiLanguage::SimplifiedChinese, UiTextId::LocalizationInProgress) ==
            "应用壳层与“设置”使用所选语言；部分专业科学工具可能仍保持英文。",
        "Chinese scope notice should be exact");
    Require(
        UiText(UiLanguage::SimplifiedChinese, UiTextId::LanguageSaveError) ==
            "无法保存应用语言，仍继续使用此前的语言。",
        "Chinese save failure should be exact");
    Require(
        UiText(
            UiLanguage::SimplifiedChinese,
            UiTextId::SettingsFileWriteFailed) ==
            "无法写入设置文件。",
        "Chinese structured settings failure should be exact");
    Require(
        UiText(
            UiLanguage::SimplifiedChinese,
            UiTextId::DiagnosticDetails) ==
            "诊断详情",
        "raw settings detail should retain a localized diagnostic label");
    Require(
        UiText(
            UiLanguage::English,
            UiTextId::CaptureNextMainFrame) ==
            "Capture Next Main Frame",
        "English frame capture action should be exact");
    Require(
        UiText(
            UiLanguage::SimplifiedChinese,
            UiTextId::FrameCaptureOutputDirectory) ==
            "输出目录",
        "Chinese frame capture output label should be exact");
}

void TestShellAndSettingsMappingsAreExact()
{
    using specforge::StableUiLabel;
    using specforge::UiLanguage;
    using specforge::UiText;
    using specforge::UiTextId;

    Require(
        UiText(
            UiLanguage::SimplifiedChinese,
            UiTextId::FileMenu) == "文件",
        "File menu should be localized");
    Require(
        UiText(
            UiLanguage::SimplifiedChinese,
            UiTextId::LoadingSource) ==
            "正在加载源…",
        "top-bar loading status should be localized");
    Require(
        UiText(
            UiLanguage::SimplifiedChinese,
            UiTextId::SpectralLines) == "谱线",
        "shell window titles should be localized");
    Require(
        UiText(
            UiLanguage::SimplifiedChinese,
            UiTextId::GeneralPageDescription) ==
            "设置 SpecForge 的启动方式与本地工作区恢复行为。",
        "General page should be localized");
    Require(
        UiText(
            UiLanguage::SimplifiedChinese,
            UiTextId::InputBehaviorUnavailable) ==
            "暂不可用。输入行为目前遵循内置交互模型。",
        "Input page should be localized");
    Require(
        UiText(
            UiLanguage::SimplifiedChinese,
            UiTextId::DataAndRecovery) ==
            "数据与恢复",
        "Data and Recovery page should be localized");
    Require(
        UiText(
            UiLanguage::SimplifiedChinese,
            UiTextId::ProfileAutoStopNote) ==
            "达到 5 分钟或 100 MiB 后自动停止。",
        "Diagnostics page should be localized");
    Require(
        UiText(
            UiLanguage::SimplifiedChinese,
            UiTextId::BuildMetadataMismatch) ==
            "构建元数据不匹配",
        "About page should be localized");
    Require(
        UiText(
            UiLanguage::SimplifiedChinese,
            UiTextId::LocalStateWarningTitle) ==
            "SpecForge - 本地状态警告",
        "native warning title should be localized");

    const std::string english = StableUiLabel(
        UiLanguage::English,
        UiTextId::OpenFile,
        "SpecForgeOpenFile");
    const std::string chinese = StableUiLabel(
        UiLanguage::SimplifiedChinese,
        UiTextId::OpenFile,
        "SpecForgeOpenFile");
    Require(
        english ==
                "Open File...###SpecForgeOpenFile" &&
            chinese ==
                "打开文件…###SpecForgeOpenFile",
        "localized actions should retain their stable ID suffix");
}

void TestAppearanceMappingsAreExact()
{
    using specforge::UiLanguage;
    using specforge::UiText;
    using specforge::UiTextId;

    struct ExpectedText {
        UiTextId text_id;
        std::string_view english;
        std::string_view simplified_chinese;
    };
    constexpr std::array kExpectedTexts = {
        ExpectedText{
            UiTextId::Appearance,
            "Appearance",
            "外观"},
        ExpectedText{
            UiTextId::AppearancePageDescription,
            "Adjust the application theme without changing scientific plot semantics.",
            "调整应用主题，不改变科学绘图语义。"},
        ExpectedText{
            UiTextId::Theme,
            "Theme",
            "主题"},
        ExpectedText{
            UiTextId::FollowSystemTheme,
            "Follow system",
            "跟随系统"},
        ExpectedText{
            UiTextId::LightTheme,
            "Light",
            "浅色"},
        ExpectedText{
            UiTextId::DarkTheme,
            "Dark",
            "深色"},
        ExpectedText{
            UiTextId::AccentColor,
            "Accent color",
            "强调色"},
        ExpectedText{
            UiTextId::AppearanceThemeUnavailable,
            "Not available yet. The current UI uses the built-in dark style.",
            "暂不可用。当前界面使用内置深色样式。"},
        ExpectedText{
            UiTextId::UiScale,
            "UI scale",
            "界面缩放"},
        ExpectedText{
            UiTextId::Reset,
            "Reset",
            "重置"},
        ExpectedText{
            UiTextId::UiScaleDescription,
            "100% follows Windows display scaling. This setting adds an application-specific multiplier.",
            "100% 跟随 Windows 显示缩放；此设置用于调整应用自身的缩放倍率。"},
        ExpectedText{
            UiTextId::UiScaleLoadWarning,
            "The saved UI scale could not be loaded; using 100%.",
            "无法加载已保存的界面缩放比例，当前使用 100%。"},
        ExpectedText{
            UiTextId::UiScaleRejected,
            "The requested UI scale is not supported.",
            "请求的界面缩放比例不受支持。"},
        ExpectedText{
            UiTextId::UiScaleSaveError,
            "The UI scale could not be saved.",
            "无法保存界面缩放比例。"},
    };

    for (const ExpectedText& expected : kExpectedTexts) {
        Require(
            UiText(UiLanguage::English, expected.text_id) ==
                expected.english,
            "English Appearance text should be exact");
        Require(
            UiText(
                UiLanguage::SimplifiedChinese,
                expected.text_id) ==
                expected.simplified_chinese,
            "Chinese Appearance text should be exact");
    }
}

void TestSourceInspectionMappingsAreExact()
{
    using specforge::StableUiLabel;
    using specforge::UiLanguage;
    using specforge::UiText;
    using specforge::UiTextId;

    struct ExpectedText {
        UiTextId text_id;
        std::string_view english;
        std::string_view simplified_chinese;
    };
    constexpr std::array kExpectedTexts = {
        ExpectedText{UiTextId::AddFile, "Add file...", "添加文件…"},
        ExpectedText{UiTextId::AddFolder, "Add folder...", "添加文件夹…"},
        ExpectedText{UiTextId::SourceSingular, "source", "个源"},
        ExpectedText{UiTextId::SourcesPlural, "sources", "个源"},
        ExpectedText{
            UiTextId::NoSourcesInSession,
            "No sources added in this session.",
            "此会话中尚未添加源。"},
        ExpectedText{UiTextId::SourceColumn, "Source", "源"},
        ExpectedText{UiTextId::TypeColumn, "Type", "类型"},
        ExpectedText{UiTextId::StateColumn, "State", "状态"},
        ExpectedText{UiTextId::RemoveFromList, "Remove from list", "从列表中移除"},
        ExpectedText{UiTextId::NoActiveSource, "No active source", "无活动源"},
        ExpectedText{UiTextId::SourceSample, "source sample:", "源样本："},
        ExpectedText{
            UiTextId::PreviousSampleShortcut,
            "Previous sample (Left Arrow)",
            "上一个样本（左方向键）"},
        ExpectedText{
            UiTextId::NextSampleShortcut,
            "Next sample (Right Arrow)",
            "下一个样本（右方向键）"},
        ExpectedText{UiTextId::Sequence, "sequence:", "序列："},
        ExpectedText{UiTextId::SampleName, "name:", "名称："},
        ExpectedText{
            UiTextId::NoReadOnlyAnnotations,
            "No read-only annotations",
            "无只读标注"},
        ExpectedText{UiTextId::DisplayNameColumn, "Display name", "显示名称"},
        ExpectedText{UiTextId::ValueColumn, "Value", "值"},
        ExpectedText{UiTextId::AnnotationMissing, "(missing)", "（缺失）"},
        ExpectedText{
            UiTextId::AnnotationOutputMissing,
            "(output missing)",
            "（输出缺失）"},
        ExpectedText{
            UiTextId::AnnotationMetadataMissing,
            "(metadata missing)",
            "（元数据缺失）"},
        ExpectedText{
            UiTextId::AnnotationMetadataIgnored,
            "(metadata ignored)",
            "（已忽略元数据）"},
        ExpectedText{
            UiTextId::NoPlottableSpectrum,
            "No plottable spectrum",
            "无可绘制光谱"},
        ExpectedText{
            UiTextId::ShowSmoothedCurve,
            "Show smoothed curve",
            "显示平滑曲线"},
        ExpectedText{UiTextId::SmoothingMethod, "Method", "方法"},
        ExpectedText{UiTextId::SmoothingNone, "None", "无"},
        ExpectedText{UiTextId::SmoothingGaussian, "Gaussian", "高斯"},
        ExpectedText{UiTextId::SmoothingMedian, "Median", "中值"},
        ExpectedText{UiTextId::GaussianSigma, "Sigma", "标准差 σ"},
        ExpectedText{UiTextId::MedianKernelSize, "Kernel size", "核大小"},
        ExpectedText{UiTextId::EffectiveKernel, "Effective kernel:", "有效核大小："},
        ExpectedText{
            UiTextId::NoSmoothingMethodSelected,
            "No smoothing method selected",
            "未选择平滑方法"},
        ExpectedText{UiTextId::ShowRawOverlay, "Show raw overlay", "显示原始曲线叠加"},
        ExpectedText{UiTextId::RawSpectrum, "raw spectrum", "原始光谱"},
        ExpectedText{UiTextId::GaussianSmoothing, "Gaussian smoothing", "高斯平滑"},
        ExpectedText{UiTextId::MedianSmoothing, "Median smoothing", "中值平滑"},
        ExpectedText{UiTextId::CurrentSpectrum, "current spectrum", "当前光谱"},
        ExpectedText{UiTextId::Name, "Name", "名称"},
        ExpectedText{UiTextId::Points, "Points", "数据点"},
        ExpectedText{UiTextId::NoneValue, "none", "无"},
        ExpectedText{UiTextId::UnknownValue, "unknown", "未知"},
        ExpectedText{UiTextId::WavelengthMedium, "Wavelength medium", "波长介质"},
        ExpectedText{UiTextId::ObserverCorrection, "Observer correction", "观测者修正"},
        ExpectedText{UiTextId::RadialVelocity, "Radial velocity", "径向速度"},
        ExpectedText{UiTextId::RadialVelocitySource, "RV source", "径向速度来源"},
        ExpectedText{UiTextId::Redshift, "Redshift", "红移"},
        ExpectedText{UiTextId::RedshiftWarning, "Redshift warning", "红移警告"},
        ExpectedText{UiTextId::TargetRedshift, "Target z", "目标 z"},
        ExpectedText{UiTextId::TargetRedshiftSource, "Target z source", "目标 z 来源"},
        ExpectedText{UiTextId::TargetRedshiftStatus, "Target z status", "目标 z 状态"},
        ExpectedText{UiTextId::TargetRedshiftWarning, "Target z warning", "目标 z 警告"},
        ExpectedText{UiTextId::HeliocentricCorrection, "Heliocentric correction", "日心修正"},
        ExpectedText{UiTextId::TargetRestFrame, "Target rest frame", "目标静止系"},
        ExpectedText{UiTextId::RestFrameCorrection, "Rest-frame correction", "静止系修正"},
        ExpectedText{UiTextId::MetadataNotApplied, "not applied", "未应用"},
        ExpectedText{
            UiTextId::MetadataAvailableNotApplied,
            "available, not applied",
            "可用，但未应用"},
        ExpectedText{
            UiTextId::MetadataUnreliableNotApplied,
            "unreliable, not applied",
            "不可靠，未应用"},
        ExpectedText{
            UiTextId::MetadataLowSpeedApproximation,
            "RV / c low-speed approximation",
            "径向速度 / c 低速近似"},
        ExpectedText{UiTextId::MetadataPipelineRedshift, "pipeline redshift", "流水线红移"},
        ExpectedText{UiTextId::MetadataZWarningNonzero, "ZWARNING nonzero", "ZWARNING 非零"},
        ExpectedText{
            UiTextId::MetadataInvalidPipelineRedshift,
            "invalid pipeline redshift",
            "无效的流水线红移"},
        ExpectedText{UiTextId::NoSnapshot, "No snapshot", "无快照"},
        ExpectedText{UiTextId::FitView, "Fit view", "适配视图"},
        ExpectedText{UiTextId::ShowPoints, "Show points", "显示数据点"},
        ExpectedText{UiTextId::DiagnosticsHeading, "Diagnostics", "诊断"},
        ExpectedText{UiTextId::NoDiagnostics, "No diagnostics", "无诊断信息"},
        ExpectedText{UiTextId::DiagnosticSeverityInfo, "info", "信息"},
        ExpectedText{UiTextId::DiagnosticSeverityWarning, "warning", "警告"},
        ExpectedText{UiTextId::DiagnosticSeverityError, "error", "错误"},
        ExpectedText{UiTextId::DiagnosticSeverityUnknown, "unknown", "未知"},
        ExpectedText{
            UiTextId::SampleNamesFileIgnored,
            "Ignored sample-name file %s.",
            "已忽略样本名称文件 %s。"},
        ExpectedText{
            UiTextId::AnnotationFileIgnored,
            "Ignored annotation file %s.",
            "已忽略标注文件 %s。"},
        ExpectedText{
            UiTextId::AnnotationMetadataFileIgnored,
            "Ignored metadata file %s.",
            "已忽略元数据文件 %s。"},
    };

    constexpr std::size_t kFirstSourceInspectionText =
        static_cast<std::size_t>(UiTextId::AddFile);
    static_assert(
        kExpectedTexts.size() ==
        static_cast<std::size_t>(
            UiTextId::AnnotationMetadataFileIgnored) +
            1 -
            kFirstSourceInspectionText);

    for (std::size_t index = 0;
         index < kExpectedTexts.size();
         ++index) {
        const ExpectedText& expected =
            kExpectedTexts[index];
        Require(
            static_cast<std::size_t>(
                expected.text_id) ==
                kFirstSourceInspectionText + index,
            "source inspection table should cover every appended text ID in order");
        Require(
            UiText(
                UiLanguage::English,
                expected.text_id) == expected.english,
            "English source inspection text should be exact");
        Require(
            UiText(
                UiLanguage::SimplifiedChinese,
                expected.text_id) ==
                expected.simplified_chinese,
            "Chinese source inspection text should be exact");
    }

    Require(
        StableUiLabel(
            UiLanguage::SimplifiedChinese,
            UiTextId::ShowRawOverlay,
            "SpecForgeShowRawOverlay") ==
            "显示原始曲线叠加###SpecForgeShowRawOverlay",
        "localized smoothing controls should retain stable IDs");
    Require(
        StableUiLabel(
            UiLanguage::SimplifiedChinese,
            UiTextId::RawSpectrum,
            "SpecForgeRawSpectrum") ==
            "原始光谱###SpecForgeRawSpectrum",
        "localized plot series should retain stable IDs");
}

void TestSampleWorkflowMappingsAreExact()
{
    using specforge::StableUiLabel;
    using specforge::UiLanguage;
    using specforge::UiText;
    using specforge::UiTextId;

    struct ExpectedText {
        UiTextId text_id;
        std::string_view english;
        std::string_view simplified_chinese;
    };
    constexpr std::array kExpectedTexts = {
        ExpectedText{UiTextId::UseAnnotationAsLabelingTask, "Use annotation as labeling task?", "将标注用作标注任务？"},
        ExpectedText{UiTextId::AddSampleFilterSource, "Add sample filter source", "添加样本筛选源"},
        ExpectedText{UiTextId::AddSampleSortSource, "Add sample sort source", "添加样本排序源"},
        ExpectedText{UiTextId::DeleteLabelingTaskQuestion, "Delete labeling task?", "删除标注任务？"},
        ExpectedText{UiTextId::DeleteLabelQuestion, "Delete label?", "删除标签？"},
        ExpectedText{UiTextId::ChangeUsedLabelCodeQuestion, "Change used label code?", "更改已使用的标签代码？"},
        ExpectedText{UiTextId::Ascending, "Ascending", "升序"},
        ExpectedText{UiTextId::Descending, "Descending", "降序"},
        ExpectedText{UiTextId::RemoveSampleSorting, "Remove sample sorting", "移除样本排序"},
        ExpectedText{UiTextId::InternalAutosaveDraft, "internal autosave draft", "内部自动保存草稿"},
        ExpectedText{UiTextId::AutosavedToOutput, "autosaved to output", "已自动保存到输出"},
        ExpectedText{UiTextId::PendingSave, "pending", "等待保存"},
        ExpectedText{UiTextId::SaveFailedValue, "save failed", "保存失败"},
        ExpectedText{
            UiTextId::OutputPathAlreadyUsed,
            "Output path is already used by another local labeling task.",
            "该输出路径已被另一个本地标注任务使用。"},
        ExpectedText{
            UiTextId::CouldNotSaveLabelingOutput,
            "Could not save labeling output.",
            "无法保存标注输出。"},
        ExpectedText{
            UiTextId::LabelSaveStateInternalDraft,
            "State: temporary local draft; use Save to... to create a labeling annotation.",
            "状态：临时本地草稿；使用“另存为…”创建标注结果。"},
        ExpectedText{
            UiTextId::LabelSaveStateAutosaved,
            "State: output file and metadata sidecar are saved.",
            "状态：输出文件与元数据附属文件均已保存。"},
        ExpectedText{
            UiTextId::LabelSaveStatePending,
            "State: output/metadata autosave is pending; close is disabled until it finishes.",
            "状态：正在自动保存输出/元数据；完成前无法关闭任务。"},
        ExpectedText{
            UiTextId::LabelSaveStateTemporaryFailed,
            "State: Save to... failed; choose this or another output, or pause the recoverable draft.",
            "状态：“另存为…”失败；请选择此输出或其他输出，或暂停此可恢复草稿。"},
        ExpectedText{
            UiTextId::LabelSaveStateOutputFailed,
            "State: output/metadata autosave failed; close is disabled until the save succeeds.",
            "状态：自动保存输出/元数据失败；保存成功前无法关闭任务。"},
        ExpectedText{UiTextId::LabelSaveStateUnknown, "State: unknown save state.", "状态：保存状态未知。"},
        ExpectedText{UiTextId::Pause, "Pause", "暂停"},
        ExpectedText{UiTextId::Delete, "Delete", "删除"},
        ExpectedText{UiTextId::SelectLabelingTask, "Select labeling task", "选择标注任务"},
        ExpectedText{UiTextId::NewLabelingTask, "New labeling task", "新建标注任务"},
        ExpectedText{UiTextId::TemporaryLabelingTask, "Temporary labeling task", "临时标注任务"},
        ExpectedText{UiTextId::TemporaryLabelingDraft, "Temporary labeling draft", "临时标注草稿"},
        ExpectedText{UiTextId::ResumeLabelingDraft, "Resume labeling draft", "继续标注草稿"},
        ExpectedText{
            UiTextId::OutputAutosaveCloseBlocked,
            "Output autosave must finish before this task can be closed.",
            "必须等待输出自动保存完成，才能关闭此任务。"},
        ExpectedText{
            UiTextId::OutputAutosaveDeleteBlocked,
            "Output autosave must finish before this task can be deleted.",
            "必须等待输出自动保存完成，才能删除此任务。"},
        ExpectedText{
            UiTextId::UseAnnotationEditableMessage,
            "Make \"%s\" editable in Labeling. Future autosaves will write to this annotation result and its metadata sidecar.",
            "使“%s”可在“标注任务”中编辑。此后的自动保存将写入该标注结果及其元数据附属文件。"},
        ExpectedText{
            UiTextId::EditAnnotationInPlaceWarning,
            "This edits the selected annotation result in place. Back up the file first if you need to preserve the original labels.",
            "此操作会直接修改所选标注结果。若需保留原标签，请先备份文件。"},
        ExpectedText{
            UiTextId::MetadataSidecarWillBeCreated,
            "No metadata sidecar is present; one will be created on save.",
            "不存在元数据附属文件；保存时将创建。"},
        ExpectedText{UiTextId::ExistingLabelMetadataReused, "Existing label metadata will be reused.", "将复用现有标签元数据。"},
        ExpectedText{UiTextId::UseAnnotation, "Use annotation", "使用此标注"},
        ExpectedText{UiTextId::Cancel, "Cancel", "取消"},
        ExpectedText{
            UiTextId::DeleteLocalTaskMessage,
            "Delete local task \"%s\". Output files are not deleted.",
            "删除本地任务“%s”。不会删除输出文件。"},
        ExpectedText{UiTextId::DeleteTask, "Delete task", "删除任务"},
        ExpectedText{UiTextId::LabelingProgress, "Progress: %llu labeled / %llu", "进度：已标注 %llu / %llu"},
        ExpectedText{UiTextId::CurrentLabelValue, "Current: %s", "当前：%s"},
        ExpectedText{UiTextId::RememberedRow, "Remembered row: %llu", "记忆行：%llu"},
        ExpectedText{UiTextId::Resume, "Resume", "继续"},
        ExpectedText{UiTextId::SaveStatus, "Save: %s", "保存：%s"},
        ExpectedText{UiTextId::LocalTaskRecord, "Local task record: %s", "本地任务记录：%s"},
        ExpectedText{UiTextId::AutoAdvance, "Auto-advance", "自动前进"},
        ExpectedText{UiTextId::SkipLabeled, "Skip labeled", "跳过已标注样本"},
        ExpectedText{UiTextId::SaveTo, "Save to...", "另存为…"},
        ExpectedText{UiTextId::Labels, "Labels", "标签"},
        ExpectedText{UiTextId::AddLabel, "Add label", "添加标签"},
        ExpectedText{UiTextId::DefaultLabelName, "Label %d", "标签 %d"},
        ExpectedText{UiTextId::Code, "Code", "代码"},
        ExpectedText{UiTextId::Shortcut, "Shortcut", "快捷键"},
        ExpectedText{UiTextId::PressKey, "Press key...", "按键…"},
        ExpectedText{
            UiTextId::ShortcutCaptureInstructions,
            "Press A-Z or 0-9. Backspace clears the binding; Escape cancels.",
            "按 A–Z 或 0–9。Backspace 清除绑定，Escape 取消。"},
        ExpectedText{UiTextId::ShortcutCaptureWaiting, "Waiting for an unmodified letter or digit", "正在等待不带修饰键的字母或数字"},
        ExpectedText{UiTextId::CaptureLabelShortcut, "Capture a label shortcut", "捕获标签快捷键"},
        ExpectedText{UiTextId::Clear, "Clear", "清除"},
        ExpectedText{
            UiTextId::ShortcutUnboundOnSave,
            "The shortcut will be unbound when this label is saved.",
            "保存此标签时将解除该快捷键绑定。"},
        ExpectedText{
            UiTextId::ShortcutKeysOnly,
            "Only unmodified A-Z and 0-9 keys can be assigned.",
            "只能分配不带修饰键的 A–Z 和 0–9。"},
        ExpectedText{
            UiTextId::ShortcutSelected,
            "Shortcut %s selected. Save the label to apply it.",
            "已选择快捷键 %s。保存标签后生效。"},
        ExpectedText{
            UiTextId::ShortcutWillMove,
            "Shortcut %s will move from %s when this label is saved.",
            "保存此标签时，快捷键 %s 将从“%s”移至此处。"},
        ExpectedText{
            UiTextId::ShortcutConflict,
            "%s is assigned to %s. Press %s again to move it.",
            "%s 已分配给“%s”。再次按 %s 可将其移至此处。"},
        ExpectedText{UiTextId::NameRequired, "Name is required", "名称不能为空"},
        ExpectedText{UiTextId::CodeIntegerRequired, "Code must be an integer", "代码必须是整数"},
        ExpectedText{UiTextId::UnlabeledCodeReserved, "Code -1 is reserved for unlabeled samples", "代码 -1 保留给未标注样本"},
        ExpectedText{
            UiTextId::CodeAlreadyUsed,
            "Code %d is already used by a label or sample value",
            "代码 %d 已被标签或样本值使用"},
        ExpectedText{UiTextId::ShortcutOneCharacter, "Shortcut must be one letter or digit", "快捷键必须是一个字母或数字"},
        ExpectedText{
            UiTextId::LabelCodeRewriteCount,
            "Changing this code rewrites %llu assigned sample value(s)",
            "更改此代码将重写 %llu 个已分配样本值"},
        ExpectedText{UiTextId::SavingMovesShortcut, "Saving moves this shortcut from %s", "保存后会将此快捷键从“%s”移至当前标签"},
        ExpectedText{UiTextId::SaveLabel, "Save label", "保存标签"},
        ExpectedText{UiTextId::CancelEditing, "Cancel editing", "取消编辑"},
        ExpectedText{UiTextId::EditLabel, "Edit label", "编辑标签"},
        ExpectedText{UiTextId::DeleteLabel, "Delete label", "删除标签"},
        ExpectedText{UiTextId::DeleteLabelAndClear, "Delete label and clear %llu sample(s)", "删除标签并清除 %llu 个样本"},
        ExpectedText{
            UiTextId::LabelCodeAssignedCount,
            "Label code %d is assigned to %llu sample(s).",
            "标签代码 %d 已分配给 %llu 个样本。"},
        ExpectedText{
            UiTextId::LabelCodeRewriteAll,
            "Changing it to %d will rewrite every assigned sample value.",
            "将其更改为 %d 会重写所有已分配的样本值。"},
        ExpectedText{UiTextId::ChangeCode, "Change code", "更改代码"},
        ExpectedText{
            UiTextId::LabelAssignedCount,
            "Label \"%s\" is assigned to %llu sample(s).",
            "标签“%s”已分配给 %llu 个样本。"},
        ExpectedText{
            UiTextId::DeleteLabelChangesValues,
            "Deleting it will change those values to Unlabeled (-1) and remove the label definition.",
            "删除后，这些值将改为“未标注（-1）”，并移除标签定义。"},
        ExpectedText{
            UiTextId::DeleteLabelRemovesFilter,
            "Its selected sample-filter value will also be removed, which may move the current sample.",
            "同时还会移除其选中的样本筛选值，当前样本可能因此移动。"},
        ExpectedText{UiTextId::AddAnnotationSampleFilter, "Add annotation sample filter", "添加基于标注的样本筛选"},
        ExpectedText{UiTextId::NoAvailableAnnotations, "No available annotations", "无可用标注"},
        ExpectedText{UiTextId::ResetSampleFilters, "Reset sample filters", "重置样本筛选"},
        ExpectedText{UiTextId::VisibleSamples, "Visible: %llu / %llu", "可见：%llu / %llu"},
        ExpectedText{
            UiTextId::CurrentSampleOutsideFilters,
            "Current sample is outside the active sample filters",
            "当前样本不在活动样本筛选范围内"},
        ExpectedText{
            UiTextId::FilterSourceNotLoaded,
            "Ignored a filter because its source is not loaded.",
            "由于源未加载，已忽略一个筛选条件。"},
        ExpectedText{UiTextId::FilterSourceNotFilterable, "Ignored %s because it is not filterable.", "由于“%s”不可筛选，已忽略该条件。"},
        ExpectedText{UiTextId::FilterSampleCountChanged, "Ignored %s because its sample count changed.", "由于“%s”的样本数已变化，已忽略该条件。"},
        ExpectedText{UiTextId::NoSampleFilters, "No sample filters", "无样本筛选"},
        ExpectedText{UiTextId::RemoveSampleFilter, "Remove sample filter", "移除样本筛选"},
        ExpectedText{UiTextId::AddAnnotationSampleSorting, "Add annotation sample sorting", "添加基于标注的样本排序"},
        ExpectedText{UiTextId::ResetSorting, "Reset sorting", "重置排序"},
        ExpectedText{UiTextId::SourceOrder, "Source order", "源顺序"},
        ExpectedText{UiTextId::SampleNameSortSource, "Sample name", "样本名称"},
        ExpectedText{UiTextId::NoComparableSortSources, "No comparable sort sources", "无可比较的排序源"},
        ExpectedText{UiTextId::UnlabeledValue, "Unlabeled", "未标注"},
    };

    constexpr std::size_t kFirstSampleWorkflowText =
        static_cast<std::size_t>(
            UiTextId::UseAnnotationAsLabelingTask);
    static_assert(
        kExpectedTexts.size() ==
        static_cast<std::size_t>(
            UiTextId::UnlabeledValue) +
            1 -
            kFirstSampleWorkflowText);

    for (std::size_t index = 0;
         index < kExpectedTexts.size();
         ++index) {
        const ExpectedText& expected =
            kExpectedTexts[index];
        Require(
            static_cast<std::size_t>(
                expected.text_id) ==
                kFirstSampleWorkflowText + index,
            "sample-workflow text table should cover every appended ID in order");
        Require(
            UiText(
                UiLanguage::English,
                expected.text_id) == expected.english,
            "English sample-workflow text should be exact");
        Require(
            UiText(
                UiLanguage::SimplifiedChinese,
                expected.text_id) ==
                expected.simplified_chinese,
            "Chinese sample-workflow text should be exact");
    }

    static_assert(
        kFirstSampleWorkflowText ==
        static_cast<std::size_t>(
            UiTextId::AnnotationMetadataFileIgnored) +
            1);
    static_assert(
        static_cast<std::size_t>(UiTextId::Count) ==
        static_cast<std::size_t>(
            UiTextId::UnlabeledValue) +
            1);

    Require(
        StableUiLabel(
            UiLanguage::SimplifiedChinese,
            UiTextId::UseAnnotationAsLabelingTask,
            "SpecForgeAnnotationToLabelingPopup") ==
            "将标注用作标注任务？###SpecForgeAnnotationToLabelingPopup",
        "localized sample-workflow popups should retain stable IDs");
    Require(
        StableUiLabel(
            UiLanguage::SimplifiedChinese,
            UiTextId::ResetSorting,
            "SpecForgeResetSampleSorting") ==
            "重置排序###SpecForgeResetSampleSorting",
        "localized sample-sorting controls should retain stable IDs");
}

void TestSessionSemanticsAreLocalizedAtTheUiBoundary()
{
    using specforge::SampleAnnotationWorkflowRelationship;
    using specforge::SampleLabelSaveMessageKind;
    using specforge::SourceCollectionSourceState;
    using specforge::UiLanguage;
    using specforge::UiText;

    Require(
        UiText(
            UiLanguage::English,
            SourceCollectionSourceState::LoadedWithDiagnostics) ==
            "loaded with diagnostics",
        "English source state should be exact");
    Require(
        UiText(
            UiLanguage::SimplifiedChinese,
            SourceCollectionSourceState::LoadedWithDiagnostics) ==
            "已加载（含诊断）",
        "Chinese source state should be exact");
    Require(
        UiText(
            UiLanguage::SimplifiedChinese,
            SampleAnnotationWorkflowRelationship::
                ExternalLabelResult) == "外部",
        "Chinese annotation relationship should be exact");
    Require(
        UiText(
            UiLanguage::English,
            SampleLabelSaveMessageKind::OutputPathAlreadyUsed) ==
            "Output path is already used by another local labeling task.",
        "known English labeling errors should be localized by semantic kind");
    Require(
        UiText(
            UiLanguage::SimplifiedChinese,
            SampleLabelSaveMessageKind::OutputPathAlreadyUsed) ==
            "该输出路径已被另一个本地标注任务使用。",
        "known Chinese labeling errors should be localized by semantic kind");
    Require(
        UiText(
            UiLanguage::SimplifiedChinese,
            SampleLabelSaveMessageKind::OutputSaveFailed) ==
            "无法保存标注输出。",
        "generic output save failures should be localized");
    Require(
        UiText(
            UiLanguage::SimplifiedChinese,
            SampleLabelSaveMessageKind::SystemDetail)
            .empty(),
        "system error details should remain outside the localized catalog");
}

void TestInvalidLanguageFallsBackToEnglish()
{
    constexpr std::array kInvalidLanguages = {
        specforge::UiLanguage::Count,
        static_cast<specforge::UiLanguage>(-1),
    };
    constexpr std::size_t kTextCount =
        static_cast<std::size_t>(specforge::UiTextId::Count);

    for (const specforge::UiLanguage invalid_language : kInvalidLanguages) {
        for (std::size_t index = 0; index < kTextCount; ++index) {
            const auto text_id = static_cast<specforge::UiTextId>(index);
            Require(
                specforge::UiText(invalid_language, text_id) ==
                    specforge::UiText(specforge::UiLanguage::English, text_id),
                "invalid language should fall back to English");
        }
    }
}

void TestCountSentinelIsNotDisplayable()
{
    Require(
        specforge::UiText(specforge::UiLanguage::English, specforge::UiTextId::Count).empty(),
        "count sentinel should not be displayable");
    Require(
        specforge::UiText(
            specforge::UiLanguage::SimplifiedChinese,
            specforge::UiTextId::Count)
            .empty(),
        "count sentinel should not be displayable in Chinese");
}

}  // namespace

int main()
{
    try {
        TestEveryDisplayTextIsPresent();
        TestLocalizedPrintfPlaceholderSignaturesMatch();
        TestRepresentativeMappingsAreExact();
        TestShellAndSettingsMappingsAreExact();
        TestAppearanceMappingsAreExact();
        TestSourceInspectionMappingsAreExact();
        TestSampleWorkflowMappingsAreExact();
        TestSessionSemanticsAreLocalizedAtTheUiBoundary();
        TestInvalidLanguageFallsBackToEnglish();
        TestCountSentinelIsNotDisplayable();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
