#include "faucet/faucet.hpp"

#include "wallet/address.hpp"

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <new>
#include <span>
#include <sstream>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace nova::faucet
{
namespace
{

[[nodiscard]] std::string Hex(const crypto::Hash256& hash)
{
    constexpr std::string_view digits{"0123456789abcdef"};
    std::string result;
    result.reserve(crypto::Hash256::kSize * 2U);
    for (const auto byte : hash.bytes()) {
        result.push_back(digits[byte >> 4U]);
        result.push_back(digits[byte & 0x0FU]);
    }
    return result;
}

[[nodiscard]] bool AppendDurably(const std::filesystem::path& path,
                                 const std::string_view text) noexcept
{
    if (path.empty() || text.empty()) {
        return false;
    }
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        return false;
    }
#ifdef _WIN32
    std::FILE* stream{};
    if (_wfopen_s(&stream, path.c_str(), L"ab") != 0) {
        stream = nullptr;
    }
#else
    std::FILE* stream = std::fopen(path.c_str(), "ab");
#endif
    if (stream == nullptr) {
        return false;
    }
    const auto written = std::fwrite(text.data(), 1U, text.size(), stream);
    const auto flushed = written == text.size() && std::fflush(stream) == 0;
#ifdef _WIN32
    const auto synchronized = flushed && _commit(_fileno(stream)) == 0;
#else
    const auto synchronized = flushed && fsync(fileno(stream)) == 0;
#endif
    return std::fclose(stream) == 0 && synchronized;
}

} // namespace

FaucetService::FaucetService(const consensus::NetworkParams& network, FaucetLimits limits,
                             std::filesystem::path audit_log, PayoutCallback payout) noexcept
    : network_(&network), limits_(limits), audit_log_(std::move(audit_log)),
      quota_log_(audit_log_.string() + ".quota"),
      kill_switch_state_(audit_log_.string() + ".kill-switch"), payout_(std::move(payout))
{
    metrics_.enabled = true;
}

bool FaucetService::IsValidLimits(const FaucetLimits& limits) noexcept
{
    return limits.payout_amount > 0 && limits.maximum_payout >= limits.payout_amount &&
           limits.source_window_seconds > 0U && limits.maximum_requests_per_window > 0U &&
           limits.maximum_tracked_sources > 0U && limits.maximum_source_identifier_bytes > 0U &&
           limits.address_window_seconds > 0U && limits.maximum_requests_per_address_window > 0U &&
           limits.maximum_tracked_addresses > 0U && limits.global_window_seconds > 0U &&
           limits.maximum_payout_per_global_window >= limits.payout_amount;
}

std::unique_ptr<FaucetService> FaucetService::Create(const consensus::NetworkParams& network,
                                                     FaucetLimits limits,
                                                     std::filesystem::path audit_log,
                                                     PayoutCallback payout) noexcept
{
    if (network.id != consensus::NetworkId::kTestnet || !IsValidLimits(limits) ||
        audit_log.empty() || !payout ||
        consensus::CheckNetworkParams(network) != consensus::NetworkParamsError::kNone) {
        return nullptr;
    }
    try {
        auto service = std::unique_ptr<FaucetService>{
            new FaucetService{network, limits, std::move(audit_log), std::move(payout)}};
        if (!service->LoadKillSwitchState() || !service->LoadQuotaReservations()) {
            return nullptr;
        }
        return service;
    } catch (...) {
        return nullptr;
    }
}

bool FaucetService::AppendAudit(const std::string_view outcome, const FaucetRequest& request,
                                const std::optional<crypto::Hash256>& transaction_id) noexcept
{
    const auto source_hash = crypto::Hash256::Sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(request.source_identifier.data()),
        request.source_identifier.size()});
    if (!source_hash.has_value()) {
        return false;
    }
    // Source identifiers may contain personal data. Persist only a SHA-256
    // correlation digest, never the raw source identity or any private key.
    const auto transaction = transaction_id.has_value() ? Hex(*transaction_id) : "-";
    const std::string line =
        std::to_string(request.received_at) + " outcome=" + std::string{outcome} +
        " source_sha256=" + Hex(*source_hash) + " address=" + request.address +
        " amount=" + std::to_string(request.amount) + " txid=" + transaction + "\n";
    return AppendDurably(audit_log_, line);
}

bool FaucetService::AppendQuotaReservation(const std::string_view source_key,
                                           const std::string_view address_key,
                                           const FaucetRequest& request) noexcept
{
    // Reserve quota before handing a request to the wallet RPC.  A crash after
    // the RPC call can therefore over-limit availability, but cannot cause a
    // rate-limit bypass or duplicate spending window after restart.
    const std::string line = std::to_string(request.received_at) + " " + std::string{source_key} +
                             " " + std::string{address_key} + " " + std::to_string(request.amount) +
                             "\n";
    return AppendDurably(quota_log_, line);
}

void FaucetService::PruneExpired(const std::uint64_t now) noexcept
{
    const auto erase_expired = [now](auto& windows, const std::uint64_t duration) {
        for (auto item = windows.begin(); item != windows.end();) {
            const auto& window = item->second;
            if (now >= window.starts_at && now - window.starts_at >= duration) {
                item = windows.erase(item);
            } else {
                ++item;
            }
        }
    };
    erase_expired(source_windows_, limits_.source_window_seconds);
    erase_expired(address_windows_, limits_.address_window_seconds);
    if (now >= global_window_.starts_at &&
        now - global_window_.starts_at >= limits_.global_window_seconds) {
        global_window_ = {};
    }
}

bool FaucetService::LoadKillSwitchState() noexcept
{
    std::error_code error;
    if (!std::filesystem::exists(kill_switch_state_, error)) {
        return !error;
    }
    if (error) {
        return false;
    }
    std::ifstream stream{kill_switch_state_, std::ios::binary};
    std::string state;
    if (!stream || !std::getline(stream, state) || state != "disabled" ||
        std::getline(stream, state)) {
        return false;
    }
    metrics_.enabled = false;
    return true;
}

bool FaucetService::LoadQuotaReservations() noexcept
{
    std::error_code error;
    if (!std::filesystem::exists(quota_log_, error)) {
        return !error;
    }
    const auto size = std::filesystem::file_size(quota_log_, error);
    constexpr std::uintmax_t kMaximumQuotaLogBytes = 1U * 1024U * 1024U;
    if (error || size > kMaximumQuotaLogBytes) {
        return false;
    }
    std::ifstream stream{quota_log_, std::ios::binary};
    if (!stream) {
        return false;
    }
    std::string line;
    while (std::getline(stream, line)) {
        std::istringstream fields{line};
        std::uint64_t received_at{};
        std::string source_key;
        std::string address_key;
        primitives::Amount amount{};
        std::string extra;
        if (!(fields >> received_at >> source_key >> address_key >> amount) || fields >> extra ||
            source_key.size() != crypto::Hash256::kSize * 2U ||
            address_key.size() != crypto::Hash256::kSize * 2U || amount <= 0 ||
            amount > limits_.maximum_payout) {
            return false;
        }
        if (source_windows_.size() == limits_.maximum_tracked_sources &&
            !source_windows_.contains(source_key)) {
            return false;
        }
        if (address_windows_.size() == limits_.maximum_tracked_addresses &&
            !address_windows_.contains(address_key)) {
            return false;
        }
        auto& source = source_windows_[source_key];
        auto& address = address_windows_[address_key];
        if (received_at < source.starts_at ||
            received_at - source.starts_at >= limits_.source_window_seconds) {
            source = {received_at, 0U};
        }
        if (received_at < address.starts_at ||
            received_at - address.starts_at >= limits_.address_window_seconds) {
            address = {received_at, 0U};
        }
        if (received_at < global_window_.starts_at ||
            received_at - global_window_.starts_at >= limits_.global_window_seconds) {
            global_window_ = {received_at, 0};
        }
        if (source.requests == (std::numeric_limits<std::uint32_t>::max)() ||
            address.requests == (std::numeric_limits<std::uint32_t>::max)() ||
            global_window_.paid > (std::numeric_limits<primitives::Amount>::max)() - amount) {
            return false;
        }
        source.requests = static_cast<std::uint32_t>(source.requests + 1U);
        address.requests = static_cast<std::uint32_t>(address.requests + 1U);
        global_window_.paid += amount;
    }
    return stream.eof();
}

FaucetResult FaucetService::RequestPayout(const FaucetRequest& request) noexcept
{
    if (!metrics_.enabled) {
        ++metrics_.requests_rejected;
        return {FaucetError::kDisabled, std::nullopt};
    }
    if (request.source_identifier.empty() ||
        request.source_identifier.size() > limits_.maximum_source_identifier_bytes ||
        request.amount <= 0 || request.amount > limits_.maximum_payout ||
        request.amount != limits_.payout_amount) {
        ++metrics_.requests_rejected;
        return {FaucetError::kInvalidRequest, std::nullopt};
    }
    const auto key_hash = wallet::DecodeP2pkhAddress(request.address, *network_);
    if (!key_hash.has_value()) {
        ++metrics_.requests_rejected;
        static_cast<void>(AppendAudit("wrong-network-or-invalid-address", request, std::nullopt));
        return {FaucetError::kWrongNetworkAddress, std::nullopt};
    }
    const auto source_digest = crypto::Hash256::Sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(request.source_identifier.data()),
        request.source_identifier.size()});
    const auto address_digest = crypto::Hash256::Sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(request.address.data()), request.address.size()});
    if (!source_digest.has_value() || !address_digest.has_value()) {
        ++metrics_.requests_rejected;
        return {FaucetError::kAllocationFailure, std::nullopt};
    }
    const auto source_key = Hex(*source_digest);
    const auto address_key = Hex(*address_digest);
    try {
        PruneExpired(request.received_at);
        auto found = source_windows_.find(source_key);
        if (found == source_windows_.end()) {
            if (source_windows_.size() == limits_.maximum_tracked_sources) {
                ++metrics_.requests_rejected;
                return {FaucetError::kRateLimited, std::nullopt};
            }
            found =
                source_windows_.emplace(source_key, SourceWindow{request.received_at, 0U}).first;
        }
        auto& window = found->second;
        if (request.received_at < window.starts_at ||
            request.received_at - window.starts_at >= limits_.source_window_seconds) {
            window = {request.received_at, 0U};
        }
        if (window.requests >= limits_.maximum_requests_per_window) {
            ++metrics_.requests_rejected;
            ++metrics_.rate_limited;
            static_cast<void>(AppendAudit("rate-limited", request, std::nullopt));
            return {FaucetError::kRateLimited, std::nullopt};
        }
        auto address = address_windows_.find(address_key);
        if (address == address_windows_.end()) {
            if (address_windows_.size() == limits_.maximum_tracked_addresses) {
                ++metrics_.requests_rejected;
                ++metrics_.rate_limited;
                return {FaucetError::kRateLimited, std::nullopt};
            }
            address =
                address_windows_.emplace(address_key, SourceWindow{request.received_at, 0U}).first;
        }
        auto& address_window = address->second;
        if (request.received_at < address_window.starts_at ||
            request.received_at - address_window.starts_at >= limits_.address_window_seconds) {
            address_window = {request.received_at, 0U};
        }
        if (address_window.requests >= limits_.maximum_requests_per_address_window) {
            ++metrics_.requests_rejected;
            ++metrics_.rate_limited;
            static_cast<void>(AppendAudit("address-rate-limited", request, std::nullopt));
            return {FaucetError::kRateLimited, std::nullopt};
        }
        if (request.received_at < global_window_.starts_at ||
            request.received_at - global_window_.starts_at >= limits_.global_window_seconds) {
            global_window_ = {request.received_at, 0};
        }
        if (global_window_.paid > limits_.maximum_payout_per_global_window - request.amount) {
            ++metrics_.requests_rejected;
            ++metrics_.rate_limited;
            static_cast<void>(AppendAudit("global-rate-limited", request, std::nullopt));
            return {FaucetError::kRateLimited, std::nullopt};
        }
        const auto previous_source = window;
        const auto previous_address = address_window;
        const auto previous_global = global_window_;
        ++window.requests;
        ++address_window.requests;
        global_window_.paid += request.amount;
        if (!AppendQuotaReservation(source_key, address_key, request)) {
            window = previous_source;
            address_window = previous_address;
            global_window_ = previous_global;
            ++metrics_.requests_rejected;
            ++metrics_.audit_failures;
            return {FaucetError::kAuditFailure, std::nullopt};
        }
        const auto transaction_id = payout_({*key_hash, request.amount});
        if (!transaction_id.has_value()) {
            ++metrics_.requests_rejected;
            ++metrics_.payout_failures;
            static_cast<void>(AppendAudit("payout-failed", request, std::nullopt));
            return {FaucetError::kPayoutFailure, std::nullopt};
        }
        if (!AppendAudit("paid", request, transaction_id)) {
            ++metrics_.requests_rejected;
            ++metrics_.audit_failures;
            return {FaucetError::kAuditFailure, std::nullopt};
        }
        ++metrics_.requests_accepted;
        return {FaucetError::kNone, transaction_id};
    } catch (const std::bad_alloc&) {
        ++metrics_.requests_rejected;
        return {FaucetError::kAllocationFailure, std::nullopt};
    } catch (...) {
        ++metrics_.requests_rejected;
        return {FaucetError::kPayoutFailure, std::nullopt};
    }
}

void FaucetService::SetEnabled(const bool enabled) noexcept
{
    if (!enabled) {
        // Disabling is durable and fail-closed. Re-enabling requires the
        // external dual-custodian workflow to remove this control record and
        // start a fresh service instance; this process never self-reenables.
        static_cast<void>(AppendDurably(kill_switch_state_, "disabled\n"));
        metrics_.enabled = false;
    }
}

FaucetMetrics FaucetService::metrics() const noexcept
{
    return metrics_;
}

} // namespace nova::faucet
