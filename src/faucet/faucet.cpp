#include "faucet/faucet.hpp"

#include "wallet/address.hpp"

#include <array>
#include <cstdio>
#include <filesystem>
#include <new>
#include <span>
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
      payout_(std::move(payout))
{
    metrics_.enabled = true;
}

bool FaucetService::IsValidLimits(const FaucetLimits& limits) noexcept
{
    return limits.payout_amount > 0 && limits.maximum_payout >= limits.payout_amount &&
           limits.source_window_seconds > 0U && limits.maximum_requests_per_window > 0U &&
           limits.maximum_tracked_sources > 0U && limits.maximum_source_identifier_bytes > 0U;
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
        return std::unique_ptr<FaucetService>{
            new FaucetService{network, limits, std::move(audit_log), std::move(payout)}};
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
    try {
        auto found = source_windows_.find(request.source_identifier);
        if (found == source_windows_.end()) {
            if (source_windows_.size() == limits_.maximum_tracked_sources) {
                ++metrics_.requests_rejected;
                return {FaucetError::kRateLimited, std::nullopt};
            }
            found = source_windows_
                        .emplace(request.source_identifier, SourceWindow{request.received_at, 0U})
                        .first;
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
        ++window.requests;
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
    metrics_.enabled = enabled;
}

FaucetMetrics FaucetService::metrics() const noexcept
{
    return metrics_;
}

} // namespace nova::faucet
