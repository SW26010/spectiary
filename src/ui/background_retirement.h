#pragma once

#include <memory>
#include <type_traits>
#include <utility>

namespace spectiary {

// An opaque ownership token whose concrete object graph is released by the
// source-load reclaimer rather than by the UI thread.
using BackgroundRetirementHandle = std::shared_ptr<const void>;

template <typename T>
[[nodiscard]] BackgroundRetirementHandle MakeBackgroundRetirementHandle(T&& value)
{
    using Value = std::decay_t<T>;
    return std::make_shared<Value>(std::forward<T>(value));
}

}  // namespace spectiary
