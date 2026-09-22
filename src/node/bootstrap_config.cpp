#include "node/bootstrap_config.hpp"

#include "node/network_selection.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace nova::node
{
namespace
{

constexpr std::size_t kMaximumBootstrapFileSize = 16U * 1024U;
constexpr std::size_t kMaximumBootstrapSeeds = 16U;
constexpr std::size_t kMaximumDnsSeeds = 4U;
constexpr std::size_t kMaximumHostLength = 253U;

[[nodiscard]] char HexDigit(const std::uint8_t value) noexcept
{
    return value < 10U ? static_cast<char>('0' + value) : static_cast<char>('a' + value - 10U);
}

[[nodiscard]] std::string Hex(const crypto::Hash256& hash)
{
    std::string result;
    result.reserve(crypto::Hash256::kSize * 2U);
    for (const auto byte : hash.bytes()) {
        result.push_back(HexDigit(static_cast<std::uint8_t>(byte >> 4U)));
        result.push_back(HexDigit(static_cast<std::uint8_t>(byte & 0x0FU)));
    }
    return result;
}

[[nodiscard]] std::string Magic(const std::uint32_t magic)
{
    std::string result(8U, '0');
    for (std::size_t index = 0U; index < result.size(); ++index) {
        const auto shift = static_cast<std::uint32_t>((result.size() - 1U - index) * 4U);
        result[index] = HexDigit(static_cast<std::uint8_t>((magic >> shift) & 0x0FU));
    }
    return result;
}

[[nodiscard]] bool IsCanonicalHost(const std::string_view host) noexcept
{
    return !host.empty() && host.size() <= kMaximumHostLength && host.front() != '.' &&
           host.back() != '.' && std::all_of(host.begin(), host.end(), [](const char character) {
               const auto value = static_cast<unsigned char>(character);
               return std::isalnum(value) != 0 || character == '.' || character == '-';
           });
}

[[nodiscard]] bool IsCanonicalDnsSeed(const std::string_view host) noexcept
{
    if (!IsCanonicalHost(host) || host.find('.') == std::string_view::npos ||
        host.find("..") != std::string_view::npos) {
        return false;
    }
    std::size_t label_begin{};
    while (label_begin < host.size()) {
        const auto label_end = host.find('.', label_begin);
        const auto length =
            (label_end == std::string_view::npos ? host.size() : label_end) - label_begin;
        if (length == 0U || length > 63U || host[label_begin] == '-' ||
            host[label_begin + length - 1U] == '-') {
            return false;
        }
        if (label_end == std::string_view::npos) {
            break;
        }
        label_begin = label_end + 1U;
    }
    return true;
}

[[nodiscard]] std::optional<net::TcpEndpoint> ParseEndpoint(const std::string_view text) noexcept
{
    const auto separator = text.rfind(':');
    if (separator == std::string_view::npos || separator == 0U || separator + 1U >= text.size()) {
        return std::nullopt;
    }
    const auto host = text.substr(0U, separator);
    if (!IsCanonicalHost(host)) {
        return std::nullopt;
    }
    std::uint16_t port{};
    const auto parsed =
        std::from_chars(text.data() + separator + 1U, text.data() + text.size(), port);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || port == 0U) {
        return std::nullopt;
    }
    return net::TcpEndpoint{std::string{host}, port};
}

[[nodiscard]] std::optional<std::string_view> Value(const std::string_view line,
                                                    const std::string_view name) noexcept
{
    if (!line.starts_with(name) || line.size() <= name.size() || line[name.size()] != '=') {
        return std::nullopt;
    }
    const auto result = line.substr(name.size() + 1U);
    return result.empty() ? std::nullopt : std::optional<std::string_view>{result};
}

} // namespace

BootstrapConfigResult LoadCompiledBootstrapConfig(const consensus::NetworkParams& network) noexcept
{
    if (consensus::CheckNetworkParams(network) != consensus::NetworkParamsError::kNone) {
        return {BootstrapConfigError::kInvalidPath, {}, {}};
    }

    // Do not add provisional addresses here.  This deliberately empty table
    // is a fail-closed, source-controlled registry boundary.  A future
    // reviewed change must add only candidate-matched signed operator records
    // and DNS ownership review before TESTNET endpoints become compiled in.
    switch (network.id) {
    case consensus::NetworkId::kRegtest:
    case consensus::NetworkId::kTestnet:
    case consensus::NetworkId::kMainnet:
        return {BootstrapConfigError::kNone, {}, {}};
    }
    return {BootstrapConfigError::kInvalidPath, {}, {}};
}

BootstrapConfigResult LoadStaticBootstrapConfig(const std::filesystem::path& path,
                                                const consensus::NetworkParams& network) noexcept
{
    if (path.empty() ||
        consensus::CheckNetworkParams(network) != consensus::NetworkParamsError::kNone) {
        return {BootstrapConfigError::kInvalidPath, {}, {}};
    }
    try {
        std::ifstream input{path, std::ios::binary};
        if (!input.is_open()) {
            return {BootstrapConfigError::kReadFailure, {}, {}};
        }
        const std::string text{std::istreambuf_iterator<char>{input},
                               std::istreambuf_iterator<char>{}};
        if (text.empty() || text.size() > kMaximumBootstrapFileSize) {
            return {text.size() > kMaximumBootstrapFileSize ? BootstrapConfigError::kOversized
                                                            : BootstrapConfigError::kNonCanonical,
                    {},
                    {}};
        }
        if (text.back() != '\n' || text.find('\r') != std::string::npos ||
            text.find("\n\n") != std::string::npos) {
            return {BootstrapConfigError::kNonCanonical, {}, {}};
        }
        bool version_seen{};
        bool network_seen{};
        bool magic_seen{};
        bool genesis_seen{};
        std::vector<net::TcpEndpoint> seeds;
        std::vector<std::string> dns_seeds;
        std::size_t cursor{};
        while (cursor < text.size()) {
            const auto newline = text.find('\n', cursor);
            if (newline == std::string::npos || newline == cursor) {
                return {BootstrapConfigError::kNonCanonical, {}, {}};
            }
            const std::string_view line{text.data() + cursor, newline - cursor};
            cursor = newline + 1U;
            if (const auto version = Value(line, "version"); version.has_value()) {
                if (version_seen || *version != "1") {
                    return {BootstrapConfigError::kNonCanonical, {}, {}};
                }
                version_seen = true;
            } else if (const auto network_name = Value(line, "network"); network_name.has_value()) {
                if (network_seen) {
                    return {BootstrapConfigError::kNonCanonical, {}, {}};
                }
                if (*network_name != NetworkName(network.id)) {
                    return {BootstrapConfigError::kNetworkMismatch, {}, {}};
                }
                network_seen = true;
            } else if (const auto magic = Value(line, "magic"); magic.has_value()) {
                if (magic_seen) {
                    return {BootstrapConfigError::kNonCanonical, {}, {}};
                }
                if (*magic != Magic(network.network_magic)) {
                    return {BootstrapConfigError::kNetworkMismatch, {}, {}};
                }
                magic_seen = true;
            } else if (const auto genesis = Value(line, "genesis"); genesis.has_value()) {
                if (genesis_seen) {
                    return {BootstrapConfigError::kNonCanonical, {}, {}};
                }
                if (*genesis != Hex(network.genesis_hash)) {
                    return {BootstrapConfigError::kNetworkMismatch, {}, {}};
                }
                genesis_seen = true;
            } else if (const auto seed = Value(line, "seed"); seed.has_value()) {
                const auto endpoint = ParseEndpoint(*seed);
                if (!endpoint.has_value()) {
                    return {BootstrapConfigError::kInvalidEndpoint, {}, {}};
                }
                if (seeds.size() == kMaximumBootstrapSeeds) {
                    return {BootstrapConfigError::kTooManySeeds, {}, {}};
                }
                if (std::any_of(
                        seeds.begin(), seeds.end(), [&endpoint](const net::TcpEndpoint& item) {
                            return item.host == endpoint->host && item.port == endpoint->port;
                        })) {
                    return {BootstrapConfigError::kNonCanonical, {}, {}};
                }
                seeds.push_back(*endpoint);
            } else if (const auto dns_seed = Value(line, "dnsseed"); dns_seed.has_value()) {
                if (!IsCanonicalDnsSeed(*dns_seed)) {
                    return {BootstrapConfigError::kInvalidEndpoint, {}, {}};
                }
                if (dns_seeds.size() == kMaximumDnsSeeds) {
                    return {BootstrapConfigError::kTooManyDnsSeeds, {}, {}};
                }
                if (std::any_of(
                        dns_seeds.begin(), dns_seeds.end(),
                        [dns_seed](const std::string& item) { return item == *dns_seed; })) {
                    return {BootstrapConfigError::kNonCanonical, {}, {}};
                }
                dns_seeds.emplace_back(*dns_seed);
            } else {
                return {BootstrapConfigError::kNonCanonical, {}, {}};
            }
        }
        if (!version_seen || !network_seen || !magic_seen || !genesis_seen) {
            return {BootstrapConfigError::kNonCanonical, {}, {}};
        }
        // A canonical testnet header is useful before independently operated
        // seed infrastructure is approved. It remains identity-bound and
        // leaves peer discovery to explicit manual peers. Other networks must
        // still declare at least one bootstrap record.
        if (seeds.empty() && dns_seeds.empty() && network.id != consensus::NetworkId::kTestnet) {
            return {BootstrapConfigError::kNonCanonical, {}, {}};
        }
        return {BootstrapConfigError::kNone, std::move(seeds), std::move(dns_seeds)};
    } catch (const std::bad_alloc&) {
        return {BootstrapConfigError::kAllocationFailure, {}, {}};
    } catch (...) {
        return {BootstrapConfigError::kReadFailure, {}, {}};
    }
}

} // namespace nova::node
