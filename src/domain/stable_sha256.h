#pragma once

#include <memory>
#include <string>
#include <string_view>

namespace specforge {

class StableSha256 {
public:
    StableSha256();
    ~StableSha256();

    StableSha256(StableSha256&&) noexcept;
    StableSha256& operator=(StableSha256&&) noexcept;
    StableSha256(const StableSha256&) = delete;
    StableSha256& operator=(const StableSha256&) = delete;

    void Append(std::string_view bytes);
    [[nodiscard]] std::string FinishHex();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::string VersionedSha256Digest(std::string_view bytes);
[[nodiscard]] std::string FinishVersionedSha256Digest(StableSha256& digest);
[[nodiscard]] bool IsVersionedSha256Digest(std::string_view value);

}  // namespace specforge
