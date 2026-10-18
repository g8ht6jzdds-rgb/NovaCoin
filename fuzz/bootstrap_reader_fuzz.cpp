#include "node/bootstrap_reader.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <streambuf>

namespace
{
class Buffer final : public std::streambuf
{
  public:
    Buffer(const std::uint8_t* data, const std::size_t size) : data_(data), size_(size) {}
    std::size_t consumed{};

  protected:
    std::streamsize xsgetn(char* destination, const std::streamsize count) override
    {
        const auto amount = std::min(size_ - consumed, static_cast<std::size_t>(count));
        if (amount != 0U) {
            std::memcpy(destination, data_ + consumed, amount);
        }
        consumed += amount;
        return static_cast<std::streamsize>(amount);
    }

  private:
    const std::uint8_t* data_;
    std::size_t size_;
};
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size)
{
    constexpr auto limit = nova::node::detail::kMaximumBootstrapFileSize;
    if (size > limit * 2U) {
        return 0;
    }
    Buffer buffer{data, size};
    std::istream input{&buffer};
    const auto result = nova::node::detail::ReadBootstrapText(input);
    if (buffer.consumed != std::min(size, limit + 1U) || result.size != buffer.consumed ||
        result.error != (size > limit ? nova::node::BootstrapConfigError::kOversized
                                      : nova::node::BootstrapConfigError::kNone)) {
        std::abort();
    }
    return 0;
}
