#include "domain/sample_filter.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace specforge {
namespace {

std::string PathToUtf8(const std::filesystem::path& path)
{
    const auto utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

void AddOption(
    std::vector<SampleFilterValueOption>& options,
    std::unordered_map<std::string, std::size_t>& option_indices,
    std::string key,
    std::string display_text)
{
    const auto existing = option_indices.find(key);
    if (existing != option_indices.end()) {
        ++options[existing->second].sample_count;
        return;
    }

    const std::size_t index = options.size();
    option_indices.emplace(key, index);
    SampleFilterValueOption option;
    option.key = std::move(key);
    option.display_text = std::move(display_text);
    option.sample_count = 1;
    options.push_back(std::move(option));
}

const SampleFilterSource* FindSource(
    const std::vector<SampleFilterSource>& sources,
    std::string_view source_id)
{
    const auto match = std::find_if(sources.begin(), sources.end(), [source_id](const auto& source) {
        return source.id == source_id;
    });
    return match == sources.end() ? nullptr : &*match;
}

}  // namespace

void SampleFilterController::Clear()
{
    conditions_.clear();
}

void SampleFilterController::ClearCondition(const std::string& source_id)
{
    conditions_.erase(
        std::remove_if(conditions_.begin(), conditions_.end(), [&source_id](const auto& condition) {
            return condition.source_id == source_id;
        }),
        conditions_.end());
}

void SampleFilterController::SetCondition(
    std::string source_id,
    std::unordered_set<std::string> allowed_value_keys)
{
    if (source_id.empty() || allowed_value_keys.empty()) {
        ClearCondition(source_id);
        return;
    }

    auto match = std::find_if(conditions_.begin(), conditions_.end(), [&source_id](const auto& condition) {
        return condition.source_id == source_id;
    });
    if (match == conditions_.end()) {
        SampleFilterCondition condition;
        condition.source_id = std::move(source_id);
        condition.allowed_value_keys = std::move(allowed_value_keys);
        conditions_.push_back(std::move(condition));
    } else {
        match->allowed_value_keys = std::move(allowed_value_keys);
    }
}

const std::vector<SampleFilterCondition>& SampleFilterController::conditions() const
{
    return conditions_;
}

const SampleFilterCondition* SampleFilterController::FindCondition(const std::string& source_id) const
{
    const auto match = std::find_if(conditions_.begin(), conditions_.end(), [&source_id](const auto& condition) {
        return condition.source_id == source_id;
    });
    return match == conditions_.end() ? nullptr : &*match;
}

SampleFilterEvaluation SampleFilterController::Evaluate(
    const std::vector<SampleFilterSource>& sources,
    std::size_t sample_count) const
{
    SampleFilterEvaluation evaluation;
    evaluation.included_samples.assign(sample_count, true);
    evaluation.included_count = sample_count;

    for (const SampleFilterCondition& condition : conditions_) {
        const SampleFilterSource* source = FindSource(sources, condition.source_id);
        if (source == nullptr) {
            evaluation.messages.push_back("Ignored a filter because its source is not loaded.");
            continue;
        }
        if (!source->filterable) {
            evaluation.messages.push_back("Ignored " + source->name + " because it is not filterable.");
            continue;
        }
        if (source->value_keys_by_sample.size() != sample_count) {
            evaluation.messages.push_back("Ignored " + source->name + " because its sample count changed.");
            continue;
        }

        evaluation.active = true;
        evaluation.included_count = 0;
        for (std::size_t index = 0; index < sample_count; ++index) {
            evaluation.included_samples[index] =
                evaluation.included_samples[index] &&
                condition.allowed_value_keys.find(source->value_keys_by_sample[index]) !=
                    condition.allowed_value_keys.end();
            if (evaluation.included_samples[index]) {
                ++evaluation.included_count;
            }
        }
    }

    if (!evaluation.active) {
        evaluation.included_count = sample_count;
        std::fill(evaluation.included_samples.begin(), evaluation.included_samples.end(), true);
    }
    return evaluation;
}

SampleFilterSource BuildAnnotationFilterSource(const SampleAnnotationResult& annotation)
{
    SampleFilterSource source;
    source.id = annotation.path.empty() ? "annotation:" + annotation.name : "annotation:" + PathToUtf8(annotation.path);
    source.name = annotation.name;
    source.kind = annotation.kind;
    source.filterable = annotation.kind != SampleAnnotationKind::ContinuousFloat;
    source.value_keys_by_sample.reserve(annotation.values.size());

    std::unordered_map<std::string, std::size_t> option_indices;
    for (const SampleAnnotationValue& value : annotation.values) {
        source.value_keys_by_sample.push_back(value.display_text);
        if (source.filterable) {
            AddOption(source.options, option_indices, value.display_text, value.display_text);
        }
    }
    return source;
}

SampleFilterSource BuildLabelingFilterSource(const SampleLabelingTask& task)
{
    SampleFilterSource source;
    source.id = "labeling:" + task.task_id;
    source.name = task.task_name;
    source.kind = SampleAnnotationKind::CategoricalInteger;
    source.filterable = true;
    source.value_keys_by_sample.reserve(task.values.size());

    std::unordered_map<std::string, std::size_t> option_indices;
    for (const int value : task.values) {
        const std::string key = std::to_string(value);
        source.value_keys_by_sample.push_back(key);
        AddOption(source.options, option_indices, key, FormatSampleLabelValue(task.label_set, value));
    }

    for (const SampleLabelDefinition& label : task.label_set.labels) {
        const std::string key = std::to_string(label.code);
        if (option_indices.find(key) == option_indices.end()) {
            SampleFilterValueOption option;
            option.key = key;
            option.display_text = FormatSampleLabelValue(task.label_set, label.code);
            option.sample_count = 0;
            option_indices.emplace(key, source.options.size());
            source.options.push_back(std::move(option));
        }
    }

    const std::string unlabeled_key = std::to_string(kUnlabeledSampleLabelCode);
    if (option_indices.find(unlabeled_key) == option_indices.end()) {
        SampleFilterValueOption option;
        option.key = unlabeled_key;
        option.display_text = FormatSampleLabelValue(task.label_set, kUnlabeledSampleLabelCode);
        option.sample_count = 0;
        source.options.push_back(std::move(option));
    }

    return source;
}

}  // namespace specforge
