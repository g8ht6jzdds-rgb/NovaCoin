#include "faucet/http.hpp"

#include <cctype>
#include <charconv>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace nova::faucet
{
namespace
{

[[nodiscard]] bool IsLoopback(const std::string_view address) noexcept
{
    return address == "127.0.0.1" || address == "::1";
}

[[nodiscard]] FaucetHttpResponse Error(const std::uint16_t status, const std::string_view message)
{
    return {status, "application/json", "{\"error\":\"" + std::string{message} + "\"}"};
}

[[nodiscard]] std::optional<std::string_view> StringValue(const std::string_view input,
                                                          std::size_t& position) noexcept
{
    if (position >= input.size() || input[position] != '\"') {
        return std::nullopt;
    }
    const auto begin = ++position;
    while (position < input.size() && input[position] != '\"') {
        const auto character = static_cast<unsigned char>(input[position]);
        if (character < 0x20U || input[position] == '\\') {
            return std::nullopt;
        }
        ++position;
    }
    if (position == input.size()) {
        return std::nullopt;
    }
    const auto value = input.substr(begin, position - begin);
    ++position;
    return value;
}

[[nodiscard]] std::optional<primitives::Amount> AmountValue(const std::string_view input,
                                                            std::size_t& position) noexcept
{
    const auto begin = position;
    if (position < input.size() && input[position] == '-') {
        ++position;
    }
    const auto digits_begin = position;
    while (position < input.size() && input[position] >= '0' && input[position] <= '9') {
        ++position;
    }
    if (digits_begin == position || (position - digits_begin > 1U && input[digits_begin] == '0')) {
        return std::nullopt;
    }
    primitives::Amount result{};
    const auto parsed = std::from_chars(input.data() + begin, input.data() + position, result);
    if (parsed.ec != std::errc{} || parsed.ptr != input.data() + position) {
        return std::nullopt;
    }
    return result;
}

struct PayoutJson final {
    std::string_view address;
    primitives::Amount amount{};
};

// Deliberately small JSON grammar. Canonical field order makes the transport
// deterministic and avoids accepting duplicate or ambiguous fields:
// {"address":"Base58Check","amount":<integer>}.
[[nodiscard]] std::optional<PayoutJson> ParsePayoutJson(const std::string_view input) noexcept
{
    constexpr std::string_view kAddress{"{\"address\":"};
    constexpr std::string_view kAmount{",\"amount\":"};
    if (!input.starts_with(kAddress)) {
        return std::nullopt;
    }
    std::size_t position = kAddress.size();
    const auto address = StringValue(input, position);
    if (!address.has_value() || !input.substr(position).starts_with(kAmount)) {
        return std::nullopt;
    }
    position += kAmount.size();
    const auto amount = AmountValue(input, position);
    if (!amount.has_value() || position + 1U != input.size() || input[position] != '}') {
        return std::nullopt;
    }
    return PayoutJson{*address, *amount};
}

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

[[nodiscard]] std::string ErrorName(const FaucetError error) noexcept
{
    switch (error) {
    case FaucetError::kNone:
        return "none";
    case FaucetError::kDisabled:
        return "disabled";
    case FaucetError::kWrongNetworkAddress:
        return "wrong_network_address";
    case FaucetError::kRateLimited:
        return "rate_limited";
    case FaucetError::kPayoutFailure:
        return "payout_failed";
    case FaucetError::kAuditFailure:
        return "audit_failed";
    case FaucetError::kAllocationFailure:
        return "allocation_failure";
    case FaucetError::kInvalidConfiguration:
    case FaucetError::kInvalidRequest:
        return "invalid_request";
    }
    return "invalid_request";
}

[[nodiscard]] std::uint16_t StatusFor(const FaucetError error) noexcept
{
    switch (error) {
    case FaucetError::kNone:
        return 200U;
    case FaucetError::kDisabled:
        return 503U;
    case FaucetError::kRateLimited:
        return 429U;
    case FaucetError::kPayoutFailure:
    case FaucetError::kAuditFailure:
    case FaucetError::kAllocationFailure:
        return 503U;
    case FaucetError::kInvalidConfiguration:
    case FaucetError::kInvalidRequest:
    case FaucetError::kWrongNetworkAddress:
        return 400U;
    }
    return 400U;
}

} // namespace

FaucetHttpService::FaucetHttpService(FaucetHttpConfig config, FaucetService& service) noexcept
    : config_(std::move(config)), service_(service)
{
}

std::unique_ptr<FaucetHttpService> FaucetHttpService::Create(FaucetHttpConfig config,
                                                             FaucetService& service) noexcept
{
    if (!IsLoopback(config.bind_address) || config.limits.max_body_bytes == 0U ||
        config.limits.max_source_identifier_bytes == 0U) {
        return nullptr;
    }
    try {
        return std::unique_ptr<FaucetHttpService>{
            new FaucetHttpService{std::move(config), service}};
    } catch (...) {
        return nullptr;
    }
}

FaucetHttpResponse FaucetHttpService::Handle(const FaucetHttpRequest& request) noexcept
{
    try {
        if (request.method != "POST") {
            return Error(405U, "method_not_allowed");
        }
        if (request.target != "/api/v1/request" || request.body.empty() ||
            request.body.size() > config_.limits.max_body_bytes ||
            request.source_identifier.empty() ||
            request.source_identifier.size() > config_.limits.max_source_identifier_bytes) {
            return Error(400U, "malformed_request");
        }
        const auto payout = ParsePayoutJson(request.body);
        if (!payout.has_value()) {
            return Error(400U, "malformed_request");
        }
        const auto result = service_.RequestPayout({std::string{request.source_identifier},
                                                    std::string{payout->address}, payout->amount,
                                                    request.received_at});
        if (!result.transaction_id.has_value()) {
            return Error(StatusFor(result.error), ErrorName(result.error));
        }
        return {200U, "application/json", "{\"txid\":\"" + Hex(*result.transaction_id) + "\"}"};
    } catch (const std::bad_alloc&) {
        return Error(503U, "allocation_failure");
    } catch (...) {
        return Error(503U, "internal_error");
    }
}

} // namespace nova::faucet
