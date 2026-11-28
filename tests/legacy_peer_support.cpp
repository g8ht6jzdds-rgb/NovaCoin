// Historical node/peer fixtures only. Never linked into daemon or release targets.
#include "node/peer_service.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace nova::node
{
namespace
{
[[nodiscard]] chain::BlockValidationParams
ValidationParams(const consensus::NetworkParams& network) noexcept
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

} // namespace

PeerService::PeerService(RegtestNode& node, std::unique_ptr<net::TcpTransport> transport,
                         Synchronizer synchronizer)
    : node_(&node), network_(&node.network_params()), transport_(std::move(transport))
{
    auto sync = std::make_shared<Synchronizer>(std::move(synchronizer));
    legacy_ = std::make_unique<LegacyAdapter>();
    legacy_->record_transport = [&node](const net::TransportResult& result) {
        node.RecordTransportResult(result);
    };
    legacy_->body_state = [&node, sync](const crypto::Hash256& hash) {
        if (node.chain_state().GetHeaderMetadata(hash).has_value())
            return BodyRequestRecovery::BodyState::kValidated;
        return sync->MissingBody(hash) ? BodyRequestRecovery::BodyState::kPending
                                       : BodyRequestRecovery::BodyState::kStale;
    };
    legacy_->start = [sync] { return sync->Start(); };
    legacy_->headers = [sync](const net::FramedMessage& message) { return sync->Handle(message); };
    legacy_->block = [&node](const primitives::Block& block) { return node.ReceiveBlock(block); };
    legacy_->transaction = [&node](const primitives::Transaction& transaction) {
        return node.ReceiveTransaction(transaction);
    };
    legacy_->find_block = [&node](const crypto::Hash256& hash) {
        return node.chain_state().GetBlockIndex(hash);
    };
    legacy_->headers_for_locator =
        [&node](const net::GetHeadersMessage& request) -> std::optional<net::HeadersMessage> {
        const auto active = node.chain_state().GetActiveBlockIndexes();
        if (!active || request.locator_hashes.size() > 101U)
            return std::nullopt;
        std::size_t start{};
        for (const auto& locator : request.locator_hashes) {
            const auto found =
                std::find_if(active->begin(), active->end(),
                             [&locator](const auto& index) { return index.hash == locator; });
            if (found != active->end()) {
                start = static_cast<std::size_t>(std::distance(active->begin(), found)) + 1U;
                break;
            }
        }
        net::HeadersMessage response;
        for (std::size_t index = start; index < active->size() && response.headers.size() < 2'000U;
             ++index) {
            const auto& entry = active->at(index);
            if (!entry.block.has_value())
                continue;
            response.headers.push_back(entry.block->header);
            if (entry.hash == request.stop_hash)
                break;
        }
        return response;
    };
}

std::unique_ptr<PeerService> PeerService::Create(RegtestNode& node) noexcept
{
    const auto& network = node.network_params();
    auto transport = MakeLegacyTestTransport(network);
    if (transport == nullptr) {
        return nullptr;
    }
    try {
        return std::unique_ptr<PeerService>{
            new PeerService{node, std::move(transport),
                            Synchronizer{{ValidationParams(network), network.difficulty,
                                          network.protocol_version, 2U, 2'000U, 4'096U},
                                         node.chain_state()}}};
    } catch (...) {
        return nullptr;
    }
}

} // namespace nova::node
