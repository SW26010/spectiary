#pragma once

#include "domain/sample_annotation_io.h"
#include "domain/sample_labeling.h"

#include <cstddef>
#include <string>
#include <unordered_set>
#include <vector>

namespace specforge {

struct SampleFilterValueOption {
    std::string key;
    std::string display_text;
    std::size_t sample_count = 0;
};

struct SampleFilterSource {
    std::string id;
    std::string name;
    SampleAnnotationKind kind = SampleAnnotationKind::Text;
    bool filterable = false;
    std::vector<std::string> value_keys_by_sample;
    std::vector<SampleFilterValueOption> options;
};

struct SampleFilterCondition {
    std::string source_id;
    std::unordered_set<std::string> allowed_value_keys;
};

struct SampleFilterEvaluation {
    bool active = false;
    std::vector<bool> included_samples;
    std::size_t included_count = 0;
    std::vector<std::string> messages;
};

class SampleFilterController {
public:
    void Clear();
    void ClearCondition(const std::string& source_id);
    void SetCondition(std::string source_id, std::unordered_set<std::string> allowed_value_keys);

    [[nodiscard]] const std::vector<SampleFilterCondition>& conditions() const;
    [[nodiscard]] const SampleFilterCondition* FindCondition(const std::string& source_id) const;
    [[nodiscard]] SampleFilterEvaluation Evaluate(
        const std::vector<SampleFilterSource>& sources,
        std::size_t sample_count) const;

private:
    std::vector<SampleFilterCondition> conditions_;
};

[[nodiscard]] SampleFilterSource BuildAnnotationFilterSource(const SampleAnnotationResult& annotation);
[[nodiscard]] SampleFilterSource BuildLabelingFilterSource(const SampleLabelingTask& task);

}  // namespace specforge
