#include "domain/stable_sha256.h"

#include <Windows.h>
#include <bcrypt.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace spectiary {
namespace {

constexpr std::string_view kDigestPrefix = "sha256-v1:";

void RequireSuccess(NTSTATUS status, const char* operation)
{
    if (status < 0) {
        throw std::runtime_error(std::string(operation) + " failed");
    }
}

class BCryptAlgorithmHandle {
public:
    ~BCryptAlgorithmHandle()
    {
        if (value_ != nullptr) {
            BCryptCloseAlgorithmProvider(value_, 0);
        }
    }

    BCryptAlgorithmHandle(const BCryptAlgorithmHandle&) = delete;
    BCryptAlgorithmHandle& operator=(const BCryptAlgorithmHandle&) = delete;
    BCryptAlgorithmHandle() = default;

    [[nodiscard]] BCRYPT_ALG_HANDLE get() const noexcept
    {
        return value_;
    }

    [[nodiscard]] BCRYPT_ALG_HANDLE* put() noexcept
    {
        return &value_;
    }

private:
    BCRYPT_ALG_HANDLE value_ = nullptr;
};

class BCryptHashHandle {
public:
    ~BCryptHashHandle()
    {
        if (value_ != nullptr) {
            BCryptDestroyHash(value_);
        }
    }

    BCryptHashHandle(const BCryptHashHandle&) = delete;
    BCryptHashHandle& operator=(const BCryptHashHandle&) = delete;
    BCryptHashHandle() = default;

    [[nodiscard]] BCRYPT_HASH_HANDLE get() const noexcept
    {
        return value_;
    }

    [[nodiscard]] BCRYPT_HASH_HANDLE* put() noexcept
    {
        return &value_;
    }

private:
    BCRYPT_HASH_HANDLE value_ = nullptr;
};

}  // namespace

class StableSha256::Impl {
public:
    Impl()
    {
        RequireSuccess(
            BCryptOpenAlgorithmProvider(algorithm_.put(), BCRYPT_SHA256_ALGORITHM, nullptr, 0),
            "BCryptOpenAlgorithmProvider(SHA-256)");

        DWORD copied = 0;
        DWORD object_size = 0;
        RequireSuccess(
            BCryptGetProperty(
                algorithm_.get(),
                BCRYPT_OBJECT_LENGTH,
                reinterpret_cast<PUCHAR>(&object_size),
                sizeof(object_size),
                &copied,
                0),
            "BCryptGetProperty(BCRYPT_OBJECT_LENGTH)");
        object_.resize(object_size);

        DWORD hash_size = 0;
        RequireSuccess(
            BCryptGetProperty(
                algorithm_.get(),
                BCRYPT_HASH_LENGTH,
                reinterpret_cast<PUCHAR>(&hash_size),
                sizeof(hash_size),
                &copied,
                0),
            "BCryptGetProperty(BCRYPT_HASH_LENGTH)");
        if (hash_size != 32) {
            throw std::runtime_error("Windows SHA-256 provider returned an unexpected digest length");
        }

        RequireSuccess(
            BCryptCreateHash(
                algorithm_.get(),
                hash_.put(),
                object_.data(),
                static_cast<ULONG>(object_.size()),
                nullptr,
                0,
                0),
            "BCryptCreateHash(SHA-256)");
    }

    void Append(std::string_view bytes)
    {
        if (finished_) {
            throw std::logic_error("cannot append to a finished SHA-256 digest");
        }
        while (!bytes.empty()) {
            const std::size_t chunk_size =
                std::min<std::size_t>(bytes.size(), std::numeric_limits<ULONG>::max());
            RequireSuccess(
                BCryptHashData(
                    hash_.get(),
                    reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),
                    static_cast<ULONG>(chunk_size),
                    0),
                "BCryptHashData(SHA-256)");
            bytes.remove_prefix(chunk_size);
        }
    }

    std::string FinishHex()
    {
        if (finished_) {
            throw std::logic_error("SHA-256 digest was already finished");
        }
        std::array<std::uint8_t, 32> digest = {};
        RequireSuccess(
            BCryptFinishHash(hash_.get(), digest.data(), static_cast<ULONG>(digest.size()), 0),
            "BCryptFinishHash(SHA-256)");
        finished_ = true;

        constexpr char kHex[] = "0123456789abcdef";
        std::string result;
        result.resize(digest.size() * 2);
        for (std::size_t index = 0; index < digest.size(); ++index) {
            result[index * 2] = kHex[digest[index] >> 4U];
            result[index * 2 + 1] = kHex[digest[index] & 0x0fU];
        }
        return result;
    }

private:
    BCryptAlgorithmHandle algorithm_;
    // BCryptCreateHash requires the caller-owned hash object buffer to remain
    // alive until after BCryptDestroyHash. Members are destroyed in reverse
    // declaration order, so keep the handle after its backing storage.
    std::vector<UCHAR> object_;
    BCryptHashHandle hash_;
    bool finished_ = false;
};

StableSha256::StableSha256()
    : impl_(std::make_unique<Impl>())
{
}

StableSha256::~StableSha256() = default;
StableSha256::StableSha256(StableSha256&&) noexcept = default;
StableSha256& StableSha256::operator=(StableSha256&&) noexcept = default;

void StableSha256::Append(std::string_view bytes)
{
    impl_->Append(bytes);
}

std::string StableSha256::FinishHex()
{
    return impl_->FinishHex();
}

std::string VersionedSha256Digest(std::string_view bytes)
{
    StableSha256 digest;
    digest.Append(bytes);
    return FinishVersionedSha256Digest(digest);
}

std::string FinishVersionedSha256Digest(StableSha256& digest)
{
    return std::string{kDigestPrefix} + digest.FinishHex();
}

bool IsVersionedSha256Digest(std::string_view value)
{
    if (!value.starts_with(kDigestPrefix) || value.size() != kDigestPrefix.size() + 64) {
        return false;
    }
    for (const char character : value.substr(kDigestPrefix.size())) {
        if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }
    return true;
}

}  // namespace spectiary
