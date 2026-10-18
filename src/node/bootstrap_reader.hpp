#pragma once

#include "node/bootstrap_config.hpp"

#include <array>
#include <cstddef>
#include <istream>

namespace nova::node::detail
{

// Internal ingestion seam, not a daemon/RPC interface. One extra byte detects
// oversize without reading or allocating in proportion to the input length.
inline constexpr std::size_t kMaximumBootstrapFileSize = 16U * 1024U;
struct BootstrapText final {
    std::array<char, kMaximumBootstrapFileSize + 1U> bytes{};
    std::size_t size{};
    BootstrapConfigError error{BootstrapConfigError::kReadFailure};
};

[[nodiscard]] BootstrapText ReadBootstrapText(std::istream& input) noexcept;

} // namespace nova::node::detail
