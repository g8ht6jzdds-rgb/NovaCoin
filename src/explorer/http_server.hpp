#pragma once

#include "explorer/http.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace nova::explorer
{

struct ExplorerHttpServerParams final {
    std::string bind_address{"127.0.0.1"};
    std::uint16_t port{};
    std::size_t max_connections{};
    std::size_t max_header_bytes{};
    std::size_t max_target_bytes{};
    std::size_t max_response_bytes{};
    std::uint64_t idle_timeout_seconds{};
};

enum class ExplorerHttpServerError : std::uint8_t {
    kNone,
    kInvalidParameters,
    kSocketFailure,
    kBindFailure,
    kListenFailure,
    kAllocationFailure,
};

struct ExplorerHttpServerResult final {
    ExplorerHttpServerError error{ExplorerHttpServerError::kNone};
    std::size_t accepted{};
    std::size_t completed{};
    std::size_t rejected{};
};

// Bounded, one-request-per-connection listener for the explorer's read-only
// HTTP adapter.  It can bind only to loopback and has no node-state reference.
class ExplorerLoopbackHttpServer final
{
  public:
    ExplorerLoopbackHttpServer(const ExplorerLoopbackHttpServer&) = delete;
    ExplorerLoopbackHttpServer& operator=(const ExplorerLoopbackHttpServer&) = delete;
    ExplorerLoopbackHttpServer(ExplorerLoopbackHttpServer&&) = delete;
    ExplorerLoopbackHttpServer& operator=(ExplorerLoopbackHttpServer&&) = delete;
    ~ExplorerLoopbackHttpServer();

    [[nodiscard]] static std::unique_ptr<ExplorerLoopbackHttpServer>
    Create(ExplorerHttpServerParams parameters, const ExplorerHttpService& service) noexcept;
    [[nodiscard]] ExplorerHttpServerResult Pump(std::uint64_t now) noexcept;
    [[nodiscard]] std::uint16_t port() const noexcept;
    void Close() noexcept;

  private:
    struct Impl;
    explicit ExplorerLoopbackHttpServer(std::unique_ptr<Impl> implementation) noexcept;

    std::unique_ptr<Impl> implementation_;
};

} // namespace nova::explorer
