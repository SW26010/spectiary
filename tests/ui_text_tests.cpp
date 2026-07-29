#include "domain/sample_annotation_io.h"
#include "ui/source_collection_session_types.h"
#include "ui/ui_text.h"

#include <array>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
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
        static_cast<std::size_t>(UiTextId::Count) -
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

void TestSessionSemanticsAreLocalizedAtTheUiBoundary()
{
    using specforge::SampleAnnotationWorkflowRelationship;
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
        TestRepresentativeMappingsAreExact();
        TestShellAndSettingsMappingsAreExact();
        TestAppearanceMappingsAreExact();
        TestSourceInspectionMappingsAreExact();
        TestSessionSemanticsAreLocalizedAtTheUiBoundary();
        TestInvalidLanguageFallsBackToEnglish();
        TestCountSentinelIsNotDisplayable();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
