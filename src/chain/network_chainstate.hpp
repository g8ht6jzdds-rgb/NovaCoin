#pragma once

#include "chain/chainstate.hpp"

namespace nova::chain
{

// Ownership order keeps the referenced UTXO set alive until the chain is destroyed.
struct InitializedNetworkChain final {
    std::unique_ptr<UTXOSet> utxos;
    std::unique_ptr<ChainState> chain;
};

[[nodiscard]] BlockValidationParams
NetworkValidationParams(const consensus::NetworkParams& network) noexcept;

// Constructs unpublished, fresh in-memory state only. This is not a network
// enablement or an operation on an existing database/UTXO set.
class NetworkChainInitializer final
{
  public:
    // One-shot, thread-local test seam; normal startup never sets it.
    enum class FailurePoint : std::uint8_t { kNone, kBeforeSeed, kAfterSeed };
    static void SetFailurePointForTesting(FailurePoint point) noexcept;

    [[nodiscard]] static std::optional<InitializedNetworkChain>
    Create(consensus::NetworkId network,
           std::shared_ptr<const ValidationTimeSource> time_source) noexcept;
};

} // namespace nova::chain
