#include "node/peer_service.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <utility>

namespace nova::node
{
namespace
{

[[nodiscard]] std::uint64_t NowSeconds() noexcept
{
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
    return seconds <= 0 ? 0U : static_cast<std::uint64_t>(seconds);
}

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

[[nodiscard]] net::P2PParams P2PParams(const consensus::NetworkParams& network) noexcept
{
    return {network.network_magic,
            1,
            network.block_limits.max_serialized_size,
            256U,
            1'000U,
            50'000U,
            101U,
            2'000U,
            network.block_limits.transaction_limits,
            network.block_limits};
}

} // namespace

PeerService::PeerService(RegtestNode& node, std::unique_ptr<net::TcpTransport> transport,
                         Synchronizer synchronizer) noexcept
    : node_(node), transport_(std::move(transport)), synchronizer_(std::move(synchronizer))
{
}

std::unique_ptr<PeerService> PeerService::Create(RegtestNode& node) noexcept
{
    const auto& network = node.network_params();
    const auto now = NowSeconds();
    net::ConnectionParams connection{
        P2PParams(network),
        {1, 0U, static_cast<std::int64_t>(now), 1U, "/NovaCoin:0.1/", 0, true},
        30U,
        300U,
        60U,
        1'000U,
        1'000U,
        8U * 1024U * 1024U};
    auto transport =
        net::TcpTransport::Create({{connection, 64U, 16U}, 64U * 1024U, 8U * 1024U * 1024U, 16U});
    if (transport == nullptr) {
        return nullptr;
    }
    try {
        return std::unique_ptr<PeerService>{new PeerService{
            node, std::move(transport),
            Synchronizer{{ValidationParams(network), network.difficulty, 1, 2U, 2'000U},
                         node.chain_state()}}};
    } catch (...) {
        return nullptr;
    }
}

net::TransportError PeerService::Listen(const net::TcpEndpoint& endpoint) noexcept
{
    return transport_->Listen(endpoint);
}

std::optional<std::uint64_t> PeerService::Dial(const net::TcpEndpoint& endpoint,
                                               const std::uint64_t now) noexcept
{
    return transport_->Dial(endpoint, now);
}

net::TransportResult PeerService::Pump(const std::uint64_t now) noexcept
{
    const auto result = transport_->Pump(
        now, [this](const std::uint64_t peer_id, const net::FramedMessage& message) {
            Handle(peer_id, message);
        });
    node_.RecordTransportResult(result);
    return result;
}

std::size_t PeerService::peer_count() const noexcept
{
    return transport_->peer_count();
}
net::PeerManager& PeerService::peer_manager() noexcept
{
    return transport_->peer_manager();
}
void PeerService::RelayBlock(const primitives::Block& block) noexcept
{
    transport_->Broadcast({net::Command::kBlock, block});
}
void PeerService::RelayTransaction(const primitives::Transaction& transaction) noexcept
{
    transport_->Broadcast({net::Command::kTx, transaction});
}
void PeerService::DisconnectPeers() noexcept
{
    transport_->DisconnectAllPeers();
}
void PeerService::Close() noexcept
{
    transport_->Close();
}

void PeerService::QueueRequests(const std::uint64_t peer_id, const SyncResult& result) noexcept
{
    for (const auto& request : result.requests) {
        static_cast<void>(transport_->Queue(peer_id, request));
    }
}

void PeerService::Handle(const std::uint64_t peer_id, const net::FramedMessage& message) noexcept
{
    if (message.command == net::Command::kVerack) {
        QueueRequests(peer_id, synchronizer_.Start());
        return;
    }
    if (message.command == net::Command::kHeaders) {
        QueueRequests(peer_id, synchronizer_.Handle(message));
        return;
    }
    if (message.command == net::Command::kBlock) {
        const auto* block = std::get_if<primitives::Block>(&message.message);
        const auto result =
            block == nullptr ? RegtestNodeError::kBlockRejected : node_.ReceiveBlock(*block);
        if (result != RegtestNodeError::kNone && result != RegtestNodeError::kDuplicateBlock) {
            transport_->ClosePeer(peer_id);
        } else if (result == RegtestNodeError::kNone) {
            transport_->Broadcast(message, peer_id);
        }
        return;
    }
    if (message.command == net::Command::kGetHeaders) {
        const auto* request = std::get_if<net::GetHeadersMessage>(&message.message);
        const auto active = node_.chain_state().GetActiveBlockIndexes();
        if (request == nullptr || !active.has_value() || request->locator_hashes.size() > 101U) {
            transport_->ClosePeer(peer_id);
            return;
        }
        std::size_t start{};
        for (const auto& locator : request->locator_hashes) {
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
            if (entry.hash == request->stop_hash)
                break;
        }
        static_cast<void>(
            transport_->Queue(peer_id, {net::Command::kHeaders, std::move(response)}));
        return;
    }
    if (message.command == net::Command::kGetData) {
        const auto* request = std::get_if<net::InventoryMessage>(&message.message);
        if (request == nullptr || request->inventory.size() > 50'000U) {
            transport_->ClosePeer(peer_id);
            return;
        }
        for (const auto& item : request->inventory) {
            const auto index = node_.chain_state().GetBlockIndex(item.hash);
            if (item.type == 2U && index.has_value() && index->block.has_value()) {
                static_cast<void>(
                    transport_->Queue(peer_id, {net::Command::kBlock, *index->block}));
            }
        }
        return;
    }
    if (message.command == net::Command::kInv) {
        const auto* inventory = std::get_if<net::InventoryMessage>(&message.message);
        if (inventory == nullptr || inventory->inventory.size() > 50'000U) {
            transport_->ClosePeer(peer_id);
            return;
        }
        static_cast<void>(transport_->Queue(peer_id, {net::Command::kGetData, *inventory}));
        return;
    }
    if (message.command == net::Command::kTx) {
        const auto* transaction = std::get_if<primitives::Transaction>(&message.message);
        if (transaction == nullptr ||
            node_.ReceiveTransaction(*transaction) != RegtestNodeError::kNone) {
            transport_->ClosePeer(peer_id);
        } else {
            transport_->Broadcast(message, peer_id);
        }
    }
}

} // namespace nova::node
