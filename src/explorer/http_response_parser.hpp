#pragma once

#include <cstddef>
#include <optional>
#include <string_view>

namespace nova::explorer::detail
{

inline constexpr std::size_t kExplorerRpcSnapshotMaxResponseBytes = 32U * 1024U * 1024U;

[[nodiscard]] std::optional<std::string_view>
ParseRpcSnapshotHttpResponse(std::string_view response, std::size_t max_response_bytes) noexcept;

[[nodiscard]] std::optional<std::string_view>
ExtractRpcSnapshotHex(std::string_view body, std::size_t max_body_bytes) noexcept;

} // namespace nova::explorer::detail
