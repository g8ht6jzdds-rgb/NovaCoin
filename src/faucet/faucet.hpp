#pragma once

#include "consensus/network_params.hpp"
#include "wallet/wallet.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>

namespace nova::faucet
{

enum class FaucetError : std::uint8_t {
    kNone,
    kInvalidConfiguration,
    kDisabled,
    kInvalidRequest,
    kWrongNetworkAddress,
    kRateLimited,
    kPayoutFailure,
    kAuditFailure,
    kAllocationFailure,
};

struct FaucetLimits final {
    primitives::Amount payout_amount{};
    primitives::Amount maximum_payout{};
    std::uint64_t source_window_seconds{};
    std::uint32_t maximum_requests_per_window{};
    std::size_t maximum_tracked_sources{};
    std::size_t maximum_source_identifier_bytes{};
    std::uint64_t address_window_seconds{};
    std::uint32_t maximum_requests_per_address_window{};
    std::size_t maximum_tracked_addresses{};
    std::uint64_t global_window_seconds{};
    primitives::Amount maximum_payout_per_global_window{};
};

struct FaucetRequest final {
    std::string source_identifier;
    std::string address;
    primitives::Amount amount{};
    std::uint64_t received_at{};
};

struct FaucetMetrics final {
    std::uint64_t requests_accepted{};
    std::uint64_t requests_rejected{};
    std::uint64_t rate_limited{};
    std::uint64_t payout_failures{};
    std::uint64_t audit_failures{};
    bool enabled{};
};

struct FaucetResult final {
    FaucetError error{FaucetError::kInvalidConfiguration};
    std::optional<crypto::Hash256> transaction_id;
};

using PayoutCallback = std::function<std::optional<crypto::Hash256>(const wallet::Recipient&)>;

// The faucet does not own consensus state. Its PayoutCallback should be the
// TESTNET-only restricted `faucetpay` RPC path to a separate node process
// whose wallet.dat is encrypted and whose credentials are not shared with the
// explorer or seed nodes.
class FaucetService final
{
  public:
    FaucetService(const FaucetService&) = delete;
    FaucetService& operator=(const FaucetService&) = delete;

    [[nodiscard]] static std::unique_ptr<FaucetService>
    Create(const consensus::NetworkParams& network, FaucetLimits limits,
           std::filesystem::path audit_log, PayoutCallback payout) noexcept;

    [[nodiscard]] FaucetResult RequestPayout(const FaucetRequest& request) noexcept;
    void SetEnabled(bool enabled) noexcept;
    [[nodiscard]] FaucetMetrics metrics() const noexcept;

  private:
    struct SourceWindow final {
        std::uint64_t starts_at{};
        std::uint32_t requests{};
    };

    struct GlobalWindow final {
        std::uint64_t starts_at{};
        primitives::Amount paid{};
    };

    FaucetService(const consensus::NetworkParams& network, FaucetLimits limits,
                  std::filesystem::path audit_log, PayoutCallback payout) noexcept;
    [[nodiscard]] bool AppendAudit(std::string_view outcome, const FaucetRequest& request,
                                   const std::optional<crypto::Hash256>& transaction_id) noexcept;
    [[nodiscard]] bool AppendQuotaReservation(std::string_view source_key,
                                              std::string_view address_key,
                                              const FaucetRequest& request) noexcept;
    [[nodiscard]] bool LoadQuotaReservations() noexcept;
    [[nodiscard]] bool LoadKillSwitchState() noexcept;
    void PruneExpired(std::uint64_t now) noexcept;
    [[nodiscard]] static bool IsValidLimits(const FaucetLimits& limits) noexcept;

    const consensus::NetworkParams* network_{};
    FaucetLimits limits_;
    std::filesystem::path audit_log_;
    std::filesystem::path quota_log_;
    std::filesystem::path kill_switch_state_;
    PayoutCallback payout_;
    std::map<std::string, SourceWindow, std::less<>> source_windows_;
    std::map<std::string, SourceWindow, std::less<>> address_windows_;
    GlobalWindow global_window_;
    FaucetMetrics metrics_;
};

} // namespace nova::faucet
