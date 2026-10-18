#include "chain/network_chainstate.hpp"

#include <limits>
#include <new>
#include <utility>

namespace nova::chain
{
namespace
{
thread_local auto failure_point = NetworkChainInitializer::FailurePoint::kNone;

void FailForTesting(const NetworkChainInitializer::FailurePoint point)
{
    if (failure_point == point) {
        failure_point = NetworkChainInitializer::FailurePoint::kNone;
        throw std::bad_alloc{};
    }
}
} // namespace

void NetworkChainInitializer::SetFailurePointForTesting(const FailurePoint point) noexcept
{
    failure_point = point;
}

BlockValidationParams NetworkValidationParams(const consensus::NetworkParams& network) noexcept
{
    return {network.block_limits,
            network.pow,
            network.monetary,
            0U,
            500'000'000U,
            std::numeric_limits<std::uint32_t>::max(),
            1U,
            600U};
}

std::optional<InitializedNetworkChain>
NetworkChainInitializer::Create(const consensus::NetworkId id,
                                std::shared_ptr<const ValidationTimeSource> time_source) noexcept
{
    if ((id != consensus::NetworkId::kRegtest && id != consensus::NetworkId::kTestnet) ||
        time_source == nullptr) {
        return std::nullopt;
    }
    try {
        const auto& network = consensus::GetNetworkParams(id);
        if (consensus::CheckNetworkParams(network) != consensus::NetworkParamsError::kNone) {
            return std::nullopt;
        }
        const auto target =
            consensus::TargetFromCompact(network.genesis_block.header.bits, network.pow);
        const auto work =
            target.target.has_value() ? consensus::CalculateWork(*target.target) : std::nullopt;
        if (!target.target.has_value() || !work.has_value()) {
            return std::nullopt;
        }
        const ChainAnchor anchor{network.genesis_hash,
                                 crypto::Hash256{},
                                 0U,
                                 network.genesis_block.header.time,
                                 *target.target,
                                 *work,
                                 ChainWork::FromPerBlockWork(*work)};
        auto utxos = std::make_unique<UTXOSet>();
        auto chain = ChainState::Create(
            {NetworkValidationParams(network), network.difficulty, 11U, std::move(time_source)},
            anchor, *utxos);
        if (chain == nullptr) {
            return std::nullopt;
        }
        if (id == consensus::NetworkId::kTestnet) {
            FailForTesting(FailurePoint::kBeforeSeed);
            // The immutable compiled commitment was checked above. Genesis is
            // an initialization anchor, never an ordinary child-block admission.
            // All changes remain private until the owning pair is returned.
            const auto seeded = utxos->ApplyBlock(network.genesis_block.transactions, 0U,
                                                  network.block_limits.transaction_limits);
            if (seeded.error != UTXOError::kNone) {
                return std::nullopt;
            }
            FailForTesting(FailurePoint::kAfterSeed);
            chain->indexes_.at(network.genesis_hash).block = network.genesis_block;
        }
        return InitializedNetworkChain{std::move(utxos), std::move(chain)};
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace nova::chain
