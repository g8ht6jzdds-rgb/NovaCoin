#pragma once

#include "explorer/explorer.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace nova::explorer
{

struct ExplorerHttpLimits final {
    std::size_t max_target_bytes{};
    std::size_t max_authorization_bytes{};
    std::size_t max_response_items{};
};

struct ExplorerHttpConfig final {
    std::string bind_address{"127.0.0.1"};
    std::string username;
    std::string password;
    ExplorerHttpLimits limits;
    // Presentation/search is network-aware even though the index itself only
    // stores script hashes. The process supplies the immutable table selected
    // at startup; it is never inferred from a user-provided address.
    const consensus::NetworkParams* network{};
};

struct ExplorerHttpRequest final {
    std::string_view method;
    std::string_view target;
    std::string_view authorization;
    std::string_view body;
};

struct ExplorerHttpResponse final {
    std::uint16_t status{};
    std::string content_type{"application/json"};
    std::string body;
};

// A socket server supplies one bounded request at a time. This adapter owns no
// transport and only holds a const explorer index, so it cannot mutate a node.
class ExplorerHttpService final
{
  public:
    ExplorerHttpService(const ExplorerHttpService&) = delete;
    ExplorerHttpService& operator=(const ExplorerHttpService&) = delete;

    [[nodiscard]] static std::unique_ptr<ExplorerHttpService>
    Create(ExplorerHttpConfig config, const ExplorerIndex& index) noexcept;
    [[nodiscard]] ExplorerHttpResponse Handle(const ExplorerHttpRequest& request) const noexcept;
    [[nodiscard]] const std::string& bind_address() const noexcept;

  private:
    ExplorerHttpService(ExplorerHttpConfig config, const ExplorerIndex& index) noexcept;

    ExplorerHttpConfig config_;
    const ExplorerIndex& index_;
};

} // namespace nova::explorer
