#include "node/sync.hpp"

#include "consensus/network_params.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size)
{
    if (size > 512U)
        return 0;
    const auto& network = nova::consensus::RegtestNetworkParams();
    const nova::node::SyncParams params{{network.block_limits, network.pow, network.monetary, 0U,
                                         500'000'000U, std::numeric_limits<std::uint32_t>::max(),
                                         1U, 600U},
                                        network.difficulty,
                                        network.protocol_version,
                                        2U,
                                        4U,
                                        4U};
    const auto target =
        nova::consensus::TargetFromCompact(network.genesis_block.header.bits, network.pow).target;
    if (!target)
        return 0;
    const auto work = nova::consensus::CalculateWork(*target);
    if (!work)
        return 0;
    nova::chain::UTXOSet utxos;
    auto state =
        nova::chain::ChainState::Create({params.validation, network.difficulty, 11U,
                                         std::make_shared<nova::chain::FixedValidationTimeSource>(
                                             network.genesis_block.header.time + 10000U)},
                                        {network.genesis_hash,
                                         {},
                                         0U,
                                         network.genesis_block.header.time,
                                         *target,
                                         *work,
                                         nova::chain::ChainWork::FromPerBlockWork(*work)},
                                        utxos);
    if (!state)
        return 0;
    nova::node::Synchronizer sync{params, *state};
    std::vector<nova::primitives::BlockHeader> fixtures;
    for (std::uint32_t index = 0U; index < 6U; ++index) {
        auto header = network.genesis_block.header;
        header.previous_block_id = network.genesis_hash;
        header.time += 600U + index;
        const auto mined = nova::consensus::MineRegtestBlock(header, 64U);
        if (!mined)
            return 0;
        fixtures.push_back(mined->header);
    }
    for (std::size_t operation = 0U; operation < std::min(size, std::size_t{16U}); ++operation) {
        const auto byte = data[operation];
        if (byte % 5U == 0U) {
            static_cast<void>(sync.Start());
        } else {
            nova::net::HeadersMessage headers;
            const auto count = static_cast<std::size_t>(byte % 5U) + 1U;
            for (std::size_t item = 0U; item < count; ++item)
                headers.headers.push_back(fixtures.at((byte + item) % fixtures.size()));
            if ((byte & 0x80U) != 0U)
                headers.headers.back().bits = 0U;
            static_cast<void>(sync.Handle({nova::net::Command::kHeaders, headers}));
        }
        if (sync.header_count() > 4U || state->active_height() != 0U)
            std::abort();
    }
    return 0;
}
