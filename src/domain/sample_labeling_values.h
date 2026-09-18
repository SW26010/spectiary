#pragma once

#include <cstddef>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>
#include <utility>
#include <stdexcept>

namespace specforge {

// A cache overlay carries only accepted pending rows; absent rows are unknown.
struct SampleLabelingSparseValues {
    std::size_t sample_count = 0;
    std::unordered_map<std::size_t, int> pending_values;
    bool operator==(const SampleLabelingSparseValues&) const = default;
};

// Accessing complete values requires selecting the complete alternative.
// Sparse cache records never expose a placeholder vector as labeling data.
class SampleLabelingValues {
public:
    [[nodiscard]] bool IsComplete() const noexcept
    {
        return std::holds_alternative<std::vector<int>>(storage_);
    }
    [[nodiscard]] const std::vector<int>* CompleteIfAvailable() const noexcept
    {
        return std::get_if<std::vector<int>>(&storage_);
    }
    [[nodiscard]] const std::vector<int>& Complete() const
    {
        return std::get<std::vector<int>>(storage_);
    }
    [[nodiscard]] std::vector<int>& Complete()
    {
        return std::get<std::vector<int>>(storage_);
    }
    [[nodiscard]] std::size_t SampleCount() const noexcept
    {
        if (const auto* complete = CompleteIfAvailable()) return complete->size();
        return std::get<SampleLabelingSparseValues>(storage_).sample_count;
    }
    [[nodiscard]] const SampleLabelingSparseValues* Sparse() const noexcept
    {
        return std::get_if<SampleLabelingSparseValues>(&storage_);
    }
    [[nodiscard]] int PendingValue(std::size_t index) const
    {
        if (const auto* complete = CompleteIfAvailable()) return complete->at(index);
        return std::get<SampleLabelingSparseValues>(storage_).pending_values.at(index);
    }
    void SetPendingValue(std::size_t index, int value)
    {
        auto& sparse = std::get<SampleLabelingSparseValues>(storage_);
        if (index >= sparse.sample_count) throw std::out_of_range("pending labeling row");
        sparse.pending_values[index] = value;
    }
    void ResetSparse(std::size_t sample_count)
    {
        storage_ = SampleLabelingSparseValues{.sample_count = sample_count};
    }
    bool MakeSparse(const std::unordered_set<std::size_t>& pending_indices)
    {
        SampleLabelingSparseValues sparse{.sample_count = SampleCount()};
        for (const auto index : pending_indices) {
            if (index < sparse.sample_count) sparse.pending_values.emplace(index, PendingValue(index));
        }
        const bool changed = !Sparse() || *Sparse() != sparse;
        storage_ = std::move(sparse);
        return changed;
    }
    bool operator==(const SampleLabelingValues&) const = default;
private:
    std::variant<std::vector<int>, SampleLabelingSparseValues> storage_;
};

} // namespace specforge
