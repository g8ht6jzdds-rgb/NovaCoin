#include "net/p2p.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size)
{
    constexpr std::size_t kMaximumInput = 1U << 20U;
    if (size > kMaximumInput) {
        return 0;
    }
    constexpr nova::primitives::TransactionLimits kTransactionLimits{21'000'000LL * 100'000'000LL,
                                                                     100'000U, 8U, 8U, 128U};
    constexpr nova::primitives::BlockLimits kBlockLimits{kTransactionLimits, 100'000U, 8U, 64U};
    const nova::net::P2PParams parameters{
        0xDAB5'BFFAU, 1, 100'000U, 256U, 16U, 32U, 16U, 16U, kTransactionLimits, kBlockLimits};
    constexpr std::uint8_t kEmptyInput = 0U;
    const auto* input = size == 0U ? &kEmptyInput : data;
    nova::net::MessageParser parser{parameters};
    static_cast<void>(parser.PushBytes(std::span<const std::uint8_t>{input, size}));
    // Exercise payload parsers without requiring mutations to preserve the
    // envelope checksum. All supported commands use the same bounded payload.
    for (const auto name : {"version", "verack", "ping", "pong", "addr", "getaddr", "inv",
                            "getdata", "getheaders", "headers", "tx", "block"}) {
        const auto command = nova::net::CommandFromString(name);
        if (!command)
            std::abort();
        const auto message = nova::net::DeserializeMessage(*command, {input, size}, parameters);
        if (message) {
            const auto encoded = nova::net::SerializeMessage(*message, parameters);
            if (!encoded)
                std::abort();
            nova::net::MessageParser replay{parameters};
            const auto result = replay.PushBytes(*encoded);
            if (result.error != nova::net::P2PError::kNone || result.messages.size() != 1U)
                std::abort();
        }
    }
    return 0;
}
