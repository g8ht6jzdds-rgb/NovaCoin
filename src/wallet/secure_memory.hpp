#pragma once

#include <openssl/crypto.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <new>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace nova::wallet::detail
{
enum class SensitiveFailureStage : std::uint8_t {
    kNone,
    kAllocation,
    kEncryptionScratch,
    kDecryptionScratch,
    kFileStaging,
};

// Thread-local in-process test seam, not a global allocator replacement and not
// reachable through configuration, files, P2P or RPC.
struct SensitiveFailureState final {
    SensitiveFailureStage stage{};
    std::size_t skip{};
    bool triggered{};
};
inline thread_local SensitiveFailureState* sensitive_failure_for_testing{};

class ScopedSensitiveFailureForTesting final
{
  public:
    explicit ScopedSensitiveFailureForTesting(const SensitiveFailureStage stage,
                                              const std::size_t skip = 0U) noexcept
        : state_{stage, skip, false}, previous_(sensitive_failure_for_testing)
    {
        sensitive_failure_for_testing = &state_;
    }
    ~ScopedSensitiveFailureForTesting()
    {
        sensitive_failure_for_testing = previous_;
    }
    ScopedSensitiveFailureForTesting(const ScopedSensitiveFailureForTesting&) = delete;
    ScopedSensitiveFailureForTesting& operator=(const ScopedSensitiveFailureForTesting&) = delete;
    bool triggered() const noexcept
    {
        return state_.triggered;
    }

  private:
    SensitiveFailureState state_;
    SensitiveFailureState* previous_;
};

inline void SensitiveAllocationCheckpoint(const SensitiveFailureStage stage)
{
    auto* failure = sensitive_failure_for_testing;
    if (failure == nullptr || failure->stage != stage || failure->triggered)
        return;
    if (failure->skip != 0U) {
        --failure->skip;
        return;
    }
    failure->triggered = true;
    throw std::bad_alloc{};
}

struct NoCleanseObserver final {
    static void AfterCleanse(std::span<const std::uint8_t>) noexcept {}
};

// Observer is a compile-time policy; production has no callback or access to
// pre-cleansed contents. Tests observe only zeros before actual deallocation.
template <typename T, typename Observer = NoCleanseObserver> class CleansingAllocator
{
  public:
    using value_type = T;
    using is_always_equal = std::true_type;
    using propagate_on_container_move_assignment = std::true_type;
    template <typename U> struct rebind {
        using other = CleansingAllocator<U, Observer>;
    };
    CleansingAllocator() noexcept = default;
    template <typename U> CleansingAllocator(const CleansingAllocator<U, Observer>&) noexcept {}
    [[nodiscard]] T* allocate(const std::size_t count)
    {
        static_assert(alignof(T) <= alignof(std::max_align_t));
        SensitiveAllocationCheckpoint(SensitiveFailureStage::kAllocation);
        if (count > std::numeric_limits<std::size_t>::max() / sizeof(T))
            throw std::bad_alloc{};
        auto* memory = OPENSSL_malloc(count * sizeof(T));
        if (memory == nullptr)
            throw std::bad_alloc{};
        return static_cast<T*>(memory);
    }
    void deallocate(T* memory, const std::size_t count) noexcept
    {
        if (memory != nullptr) {
            const auto size = count * sizeof(T); // supplied by the matching vector allocation
            OPENSSL_cleanse(memory, size);
            Observer::AfterCleanse({reinterpret_cast<const std::uint8_t*>(memory), size});
            OPENSSL_free(memory);
        }
    }
    template <typename U> bool operator==(const CleansingAllocator<U, Observer>&) const noexcept
    {
        return true;
    }
};

template <typename Observer = NoCleanseObserver> class BasicSensitiveBytes final
{
  public:
    using Buffer = std::vector<std::uint8_t, CleansingAllocator<std::uint8_t, Observer>>;
    // MSVC Debug's default vector constructor is noexcept yet allocates its
    // proxy. The size constructor permits that failure to propagate normally.
    BasicSensitiveBytes() : bytes_(0U) {}
    explicit BasicSensitiveBytes(const std::size_t size) : bytes_(size) {}
    BasicSensitiveBytes(const BasicSensitiveBytes&) = delete;
    BasicSensitiveBytes& operator=(const BasicSensitiveBytes&) = delete;
    // Empty-vector construction can allocate an MSVC debug proxy. Finish it
    // before swapping, without claiming a nonthrowing move on that platform.
    BasicSensitiveBytes(BasicSensitiveBytes&& other) : bytes_(0U)
    {
        bytes_.swap(other.bytes_);
    }
    BasicSensitiveBytes& operator=(BasicSensitiveBytes&& other) noexcept
    {
        if (this != &other)
            bytes_ = std::move(other.bytes_);
        return *this;
    }
    [[nodiscard]] std::span<const std::uint8_t> view() const noexcept
    {
        return bytes_;
    }
    [[nodiscard]] Buffer& mutable_view() noexcept
    {
        return bytes_;
    }
    [[nodiscard]] bool Append(const std::span<const std::uint8_t> input,
                              const std::size_t maximum) noexcept
    {
        if (bytes_.size() > maximum || input.size() > maximum - bytes_.size())
            return false;
        if (input.empty())
            return true;
        if (!bytes_.empty() && !input.empty()) {
            const std::less<const std::uint8_t*> less;
            if (less(input.data(), bytes_.data() + bytes_.size()) &&
                less(bytes_.data(), input.data() + input.size()))
                return false; // no invalid self-insert through a reallocation
        }
        try {
            bytes_.insert(bytes_.end(), input.begin(), input.end());
            return true;
        } catch (...) {
            return false;
        }
    }

  private:
    Buffer bytes_;
};
using SensitiveBytes = BasicSensitiveBytes<>;

// Wallet-local canonical writer: preserve the existing integer/CompactSize
// grammar without passing any plaintext fields through an ordinary byte vector.
class SensitiveWriter final
{
  public:
    explicit SensitiveWriter(const std::size_t maximum) : maximum_(maximum) {}
    bool WriteU8(const std::uint8_t value) noexcept
    {
        return WriteInteger(value, 1U);
    }
    bool WriteU32(const std::uint32_t value) noexcept
    {
        return WriteInteger(value, 4U);
    }
    bool WriteI64(const std::int64_t value) noexcept
    {
        return WriteInteger(std::bit_cast<std::uint64_t>(value), 8U);
    }
    bool WriteCompactSize(const std::uint64_t value) noexcept
    {
        if (value < 253U)
            return WriteU8(static_cast<std::uint8_t>(value));
        if (value <= std::numeric_limits<std::uint16_t>::max())
            return WriteU8(0xFDU) && WriteInteger(value, 2U);
        if (value <= std::numeric_limits<std::uint32_t>::max())
            return WriteU8(0xFEU) && WriteInteger(value, 4U);
        return WriteU8(0xFFU) && WriteInteger(value, 8U);
    }
    bool WriteFixedBytes(const std::span<const std::uint8_t> bytes) noexcept
    {
        return bytes_.Append(bytes, maximum_);
    }
    bool WriteBytes(const std::span<const std::uint8_t> bytes, const std::uint64_t maximum) noexcept
    {
        static_assert(sizeof(std::size_t) <= sizeof(std::uint64_t));
        return bytes.size() <= maximum &&
               WriteCompactSize(static_cast<std::uint64_t>(bytes.size())) && WriteFixedBytes(bytes);
    }
    std::span<const std::uint8_t> bytes() const noexcept
    {
        return bytes_.view();
    }

  private:
    bool WriteInteger(const std::uint64_t value, const std::size_t count) noexcept
    {
        std::array<std::uint8_t, 8U> encoded{};
        for (std::size_t i = 0; i < count; ++i)
            encoded[i] = static_cast<std::uint8_t>(value >> (8U * i));
        const auto result = bytes_.Append(std::span{encoded}.first(count), maximum_);
        OPENSSL_cleanse(encoded.data(), encoded.size());
        return result;
    }
    SensitiveBytes bytes_;
    std::size_t maximum_;
};
} // namespace nova::wallet::detail
