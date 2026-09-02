#pragma once

#include "faucet/http.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace nova::faucet
{

struct FaucetHttpServerParams final {
    std::string bind_address{"127.0.0.1"};
    std::uint16_t port{};
    std::size_t max_connections{};
    std::size_t max_header_bytes{};
    std::size_t max_body_bytes{};
    std::size_t max_response_bytes{};
    std::uint64_t idle_timeout_seconds{};
};

enum class FaucetHttpServerError : std::uint8_t {
    kNone,
    kInvalidParameters,
    kSocketFailure,
    kBindFailure,
    kListenFailure,
    kAllocationFailure,
};

struct FaucetHttpServerResult final {
    FaucetHttpServerError error{FaucetHttpServerError::kNone};
    std::size_t accepted{};
    std::size_t completed{};
    std::size_t rejected{};
};

// A bounded, one-request-per-connection listener. It intentionally binds
// only to loopback: a public frontend/reverse proxy must be separately
// reviewed before exposure and must not forward a client-controlled identity.
class FaucetLoopbackHttpServer final
{
  public:
    FaucetLoopbackHttpServer(const FaucetLoopbackHttpServer&) = delete;
    FaucetLoopbackHttpServer& operator=(const FaucetLoopbackHttpServer&) = delete;
    FaucetLoopbackHttpServer(FaucetLoopbackHttpServer&&) = delete;
    FaucetLoopbackHttpServer& operator=(FaucetLoopbackHttpServer&&) = delete;
    ~FaucetLoopbackHttpServer();

    [[nodiscard]] static std::unique_ptr<FaucetLoopbackHttpServer>
    Create(FaucetHttpServerParams parameters, FaucetHttpService& service) noexcept;
    [[nodiscard]] FaucetHttpServerResult Pump(std::uint64_t now) noexcept;
    [[nodiscard]] std::uint16_t port() const noexcept;
    void Close() noexcept;

  private:
    struct Impl;
    explicit FaucetLoopbackHttpServer(std::unique_ptr<Impl> implementation) noexcept;

    std::unique_ptr<Impl> implementation_;
};

} // namespace nova::faucet
