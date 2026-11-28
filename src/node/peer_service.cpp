#include "node/peer_service.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <openssl/rand.h>
#include <utility>

namespace nova::node
{
namespace
{

template <typename Object, typename Limits>
[[nodiscard]] bool SameSerialized(const Object& left, const Object& right,
                                  const Limits& limits) noexcept
{
    primitives::BinaryWriter left_bytes;
    primitives::BinaryWriter right_bytes;
    return left.Serialize(left_bytes, limits) && right.Serialize(right_bytes, limits) &&
           std::equal(left_bytes.bytes().begin(), left_bytes.bytes().end(),
                      right_bytes.bytes().begin(), right_bytes.bytes().end());
}

[[nodiscard]] std::uint64_t NowSeconds() noexcept
{
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
    return seconds <= 0 ? 0U : static_cast<std::uint64_t>(seconds);
}

[[nodiscard]] std::uint64_t DiscoverySeconds() noexcept
{
    const auto value = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
    return value <= 0 ? 0U : static_cast<std::uint64_t>(value);
}

[[nodiscard]] net::P2PParams P2PParams(const consensus::NetworkParams& network) noexcept
{
    return {network.network_magic,
            network.minimum_peer_protocol_version,
            network.block_limits.max_serialized_size,
            256U,
            1'000U,
            50'000U,
            101U,
            2'000U,
            network.block_limits.transaction_limits,
            network.block_limits};
}

[[nodiscard]] std::unique_ptr<net::TcpTransport>
MakeTransport(const consensus::NetworkParams& network) noexcept
{
    const auto now = NowSeconds();
    net::ConnectionParams connection{P2PParams(network),
                                     {network.protocol_version, 0U, static_cast<std::int64_t>(now),
                                      1U, "/NovaCoin:0.1/", 0, true},
                                     30U,
                                     300U,
                                     60U,
                                     1'000U,
                                     1'000U,
                                     8U * 1024U * 1024U};
    return net::TcpTransport::Create(
        {{connection, 64U, 16U}, 64U * 1024U, 8U * 1024U * 1024U, 16U});
}

} // namespace

std::unique_ptr<net::TcpTransport>
PeerService::MakeLegacyTestTransport(const consensus::NetworkParams& network) noexcept
{
    return MakeTransport(network);
}

PeerService::PeerService(
    DurableSession& session, std::unique_ptr<net::TcpTransport> transport,
    std::function<DurableSessionResult(const primitives::Block&)> accept_block,
    std::function<DurableSessionError(const primitives::Transaction&)> accept_transaction) noexcept
    : network_(&session.network_params()), durable_(&session),
      durable_block_(std::move(accept_block)), durable_transaction_(std::move(accept_transaction)),
      transport_(std::move(transport))
{
}

std::unique_ptr<PeerService>
PeerService::Create(DurableSession& session,
                    std::function<std::optional<DurablePeerWriteContext>()> write_context) noexcept
{
    if (!write_context)
        return nullptr;
    try {
        auto provider = std::make_shared<decltype(write_context)>(std::move(write_context));
        return Create(
            session,
            [&session, provider](const primitives::Block& block) {
                const auto context = (*provider)();
                if (!context)
                    return DurableSessionResult{};
                const auto result =
                    session.ApplyBlock(block, {}, context->staging_directory, context->passphrase);
                if (context->cleanup && !context->cleanup())
                    return DurableSessionResult{};
                return result;
            },
            [&session, provider](const primitives::Transaction& transaction) {
                const auto context = (*provider)();
                if (!context)
                    return DurableSessionError::kUnavailable;
                const auto result = session.AdmitTransaction(
                    transaction, context->staging_directory, context->passphrase);
                if (context->cleanup && !context->cleanup())
                    return DurableSessionError::kUnavailable;
                return result;
            });
    } catch (...) {
        return nullptr;
    }
}

std::unique_ptr<PeerService> PeerService::Create(
    DurableSession& session,
    std::function<DurableSessionResult(const primitives::Block&)> accept_block,
    std::function<DurableSessionError(const primitives::Transaction&)> accept_transaction) noexcept
{
    if (!accept_block || !accept_transaction || !session.View())
        return nullptr;
    auto transport = MakeTransport(session.network_params());
    if (!transport)
        return nullptr;
    try {
        return std::unique_ptr<PeerService>{new PeerService{
            session, std::move(transport), std::move(accept_block), std::move(accept_transaction)}};
    } catch (...) {
        return nullptr;
    }
}

net::TransportError PeerService::Listen(const net::TcpEndpoint& endpoint) noexcept
{
    if (durable_ != nullptr && (durable_failed_ || !durable_->View()))
        return net::TransportError::kInvalidParameters;
    return transport_->Listen(endpoint);
}

std::optional<std::uint64_t> PeerService::Dial(const net::TcpEndpoint& endpoint,
                                               const std::uint64_t now) noexcept
{
    if (durable_ != nullptr && (durable_failed_ || !durable_->View()))
        return std::nullopt;
    return transport_->Dial(endpoint, now);
}

net::TransportResult PeerService::Pump(const std::uint64_t now) noexcept
{
    if (durable_ != nullptr && (durable_failed_ || !durable_->View()))
        return {net::TransportError::kSocketFailure, 0U, 0U};
    recovery_now_ = clock_override_ == nullptr ? DiscoverySeconds() : *clock_override_;
    if (discovery_ != nullptr) {
        try {
            const auto peers = transport_->Peers();
            if (peers.has_value()) {
                discovery_->Update(*peers, DiscoverySeconds());
            } else {
                discovery_.reset();
            }
        } catch (...) {
            discovery_.reset(); // fail closed for discovery only
        }
    }
    const auto result = transport_->Pump(
        now, [this](const std::uint64_t peer_id, const net::FramedMessage& message) {
            Handle(peer_id, message);
        });
    if (legacy_)
        legacy_->record_transport(result);
    if (recovery_.size() != 0U) {
        const auto recovery_peers = transport_->Peers();
        if (recovery_peers.has_value())
            recovery_.Tick(recovery_now_, *recovery_peers, *this);
        else
            recovery_.SnapshotUnavailable();
    }
    if (discovery_ != nullptr) {
        try {
            const auto tick = DiscoverySeconds();
            const auto peers = transport_->Peers();
            if (!peers.has_value()) {
                discovery_.reset();
                return result;
            }
            discovery_->Update(*peers, tick);
            for (const auto peer : discovery_->Requests(tick)) {
                if (transport_->Queue(peer, {net::Command::kGetAddr, std::monostate{}}) !=
                    net::TransportError::kNone) {
                    discovery_->RequestFailed(peer);
                }
            }
            std::uint64_t random{};
            const auto entropy = RAND_bytes(reinterpret_cast<unsigned char*>(&random),
                                            static_cast<int>(sizeof(random))) == 1
                                     ? std::optional<std::uint64_t>{random}
                                     : std::nullopt;
            const auto endpoint = discovery_->Next(tick, entropy);
            if (endpoint.has_value()) {
                discovery_->Attempt(*endpoint, transport_->DialNumeric(endpoint->Numeric(), now),
                                    tick);
            }
        } catch (...) {
            discovery_.reset();
        }
    }
    return result;
}

bool PeerService::EnableDiscovery(const std::uint16_t listening_port) noexcept
{
    const auto& network = *network_;
    if (network.id != consensus::NetworkId::kRegtest || listening_port == 0U) {
        return false;
    }
    try {
        discovery_ = std::make_unique<PeerDiscovery>(listening_port);
        return true;
    } catch (...) {
        return false;
    }
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
    if (durable_ != nullptr && (durable_failed_ || !durable_->View()))
        return;
    transport_->Broadcast({net::Command::kBlock, block});
}
void PeerService::RelayTransaction(const primitives::Transaction& transaction) noexcept
{
    if (durable_ != nullptr && (durable_failed_ || !durable_->View()))
        return;
    transport_->Broadcast({net::Command::kTx, transaction});
}
void PeerService::QuarantineDurable() noexcept
{
    if (durable_ != nullptr) {
        durable_failed_ = true;
        recovery_.SnapshotUnavailable();
    }
}
void PeerService::DisconnectPeers() noexcept
{
    transport_->DisconnectAllPeers();
    recovery_.Tick(recovery_now_, {}, *this);
}
void PeerService::Close() noexcept
{
    transport_->Close();
    recovery_.Tick(recovery_now_, {}, *this);
}

const BodyRequestRecovery::Counters& PeerService::recovery_counters() const noexcept
{
    return recovery_.counters();
}

BodyRequestRecovery::BodyState PeerService::State(const crypto::Hash256& hash) const noexcept
{
    if (durable_ != nullptr) {
        if (durable_failed_)
            return BodyRequestRecovery::BodyState::kUnavailable;
        const auto state = durable_->HeaderBodyState(hash);
        if (!state)
            return BodyRequestRecovery::BodyState::kUnavailable;
        switch (*state) {
        case DurableBodyState::kValidated:
            return BodyRequestRecovery::BodyState::kValidated;
        case DurableBodyState::kPending:
            return BodyRequestRecovery::BodyState::kPending;
        case DurableBodyState::kStale:
            return BodyRequestRecovery::BodyState::kStale;
        }
        return BodyRequestRecovery::BodyState::kUnavailable;
    }
    return legacy_->body_state(hash);
}

net::TransportError PeerService::QueueBody(const std::uint64_t peer,
                                           const net::FramedMessage& message) noexcept
{
    return queue_hook_ == nullptr ? transport_->Queue(peer, message)
                                  : queue_hook_(queue_context_, peer, message);
}

void PeerService::QueueRequests(const std::uint64_t peer_id, const SyncResult& result) noexcept
{
    for (const auto& request : result.requests) {
        if (request.command == net::Command::kGetData) {
            if (const auto* inventory = std::get_if<net::InventoryMessage>(&request.message))
                recovery_.Submit(peer_id, *inventory, recovery_now_, *this);
        } else {
            static_cast<void>(transport_->Queue(peer_id, request));
        }
    }
}

void PeerService::Handle(const std::uint64_t peer_id, const net::FramedMessage& message) noexcept
{
    if (durable_ != nullptr && (durable_failed_ || !durable_->View())) {
        recovery_.SnapshotUnavailable();
        return;
    }
    if (discovery_ != nullptr &&
        (message.command == net::Command::kAddr || message.command == net::Command::kGetAddr)) {
        try {
            const auto tick = DiscoverySeconds();
            if (message.command == net::Command::kGetAddr) {
                const auto reply = discovery_->Reply(peer_id, tick);
                if (reply.has_value()) {
                    static_cast<void>(transport_->Queue(peer_id, {net::Command::kAddr, *reply}));
                }
            } else if (const auto* addresses = std::get_if<net::AddressMessage>(&message.message)) {
                discovery_->Receive(peer_id, *addresses, tick);
            }
        } catch (...) {
            discovery_.reset();
        }
        return;
    }
    if (message.command == net::Command::kVerack) {
        // Refresh once at handshake, not per addr/getaddr. Replayed address
        // traffic must hit cheap source quotas before any full topology scan.
        if (discovery_ != nullptr) {
            try {
                const auto peers = transport_->Peers();
                if (peers.has_value()) {
                    discovery_->Update(*peers, DiscoverySeconds());
                } else {
                    discovery_.reset();
                }
            } catch (...) {
                discovery_.reset();
            }
        }
        QueueRequests(peer_id,
                      durable_ != nullptr ? durable_->StartHeaderSync() : legacy_->start());
        return;
    }
    if (message.command == net::Command::kHeaders) {
        QueueRequests(peer_id, durable_ != nullptr ? durable_->HandleSyncHeaders(message)
                                                   : legacy_->headers(message));
        return;
    }
    if (message.command == net::Command::kBlock) {
        const auto* block = std::get_if<primitives::Block>(&message.message);
        if (durable_ != nullptr) {
            if (block == nullptr) {
                transport_->ClosePeer(peer_id);
                return;
            }
            try {
                const auto result = durable_block_(*block);
                if (result.error == DurableSessionError::kNone) {
                    const auto hash = primitives::ComputeBlockHash(block->header);
                    const auto accepted = hash ? durable_->FindBlock(*hash) : DurableBlockLookup{};
                    if (!accepted.available || !accepted.block || !accepted.block->block ||
                        !SameSerialized(*block, *accepted.block->block,
                                        durable_->network_params().block_limits)) {
                        durable_failed_ = true;
                        recovery_.SnapshotUnavailable();
                        return;
                    }
                    transport_->Broadcast(message, peer_id);
                } else if (result.error == DurableSessionError::kUnavailable) {
                    durable_failed_ = true;
                    recovery_.SnapshotUnavailable();
                } else if (result.error == DurableSessionError::kChain &&
                           result.chain_result.error != chain::ChainError::kDuplicateBlock)
                    transport_->ClosePeer(peer_id);
            } catch (...) {
                durable_failed_ = true;
                recovery_.SnapshotUnavailable();
            }
            return;
        }
        const auto result =
            block == nullptr ? RegtestNodeError::kBlockRejected : legacy_->block(*block);
        if (result != RegtestNodeError::kNone && result != RegtestNodeError::kDuplicateBlock) {
            transport_->ClosePeer(peer_id);
        } else if (result == RegtestNodeError::kNone) {
            transport_->Broadcast(message, peer_id);
        }
        return;
    }
    if (message.command == net::Command::kGetHeaders) {
        const auto* request = std::get_if<net::GetHeadersMessage>(&message.message);
        if (durable_ != nullptr) {
            if (request == nullptr || request->locator_hashes.size() > 101U) {
                transport_->ClosePeer(peer_id);
                return;
            }
            const auto response = durable_->HeadersForLocator(*request);
            if (response)
                static_cast<void>(transport_->Queue(peer_id, {net::Command::kHeaders, *response}));
            else
                recovery_.SnapshotUnavailable();
            return;
        }
        if (request == nullptr || request->locator_hashes.size() > 101U) {
            transport_->ClosePeer(peer_id);
            return;
        }
        const auto response = legacy_->headers_for_locator(*request);
        if (!response) {
            transport_->ClosePeer(peer_id);
            return;
        }
        static_cast<void>(transport_->Queue(peer_id, {net::Command::kHeaders, *response}));
        return;
    }
    if (message.command == net::Command::kGetData) {
        const auto* request = std::get_if<net::InventoryMessage>(&message.message);
        if (request == nullptr || request->inventory.size() > 50'000U) {
            transport_->ClosePeer(peer_id);
            return;
        }
        for (const auto& item : request->inventory) {
            if (item.type != 2U)
                continue;
            const auto lookup = durable_ != nullptr
                                    ? durable_->FindBlock(item.hash)
                                    : DurableBlockLookup{true, legacy_->find_block(item.hash)};
            if (!lookup.available) {
                recovery_.SnapshotUnavailable();
                return;
            }
            const auto& index = lookup.block;
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
        if (durable_ != nullptr) {
            if (transaction == nullptr) {
                transport_->ClosePeer(peer_id);
                return;
            }
            try {
                const auto result = durable_transaction_(*transaction);
                if (result == DurableSessionError::kNone) {
                    const auto id = transaction->TxId(
                        durable_->network_params().block_limits.transaction_limits);
                    const auto accepted =
                        id ? durable_->FindMempoolEntry(*id) : DurableMempoolLookup{};
                    if (!accepted.available || !accepted.entry ||
                        !SameSerialized(
                            *transaction, accepted.entry->transaction,
                            durable_->network_params().block_limits.transaction_limits)) {
                        durable_failed_ = true;
                        recovery_.SnapshotUnavailable();
                        return;
                    }
                    transport_->Broadcast(message, peer_id);
                } else if (result == DurableSessionError::kUnavailable) {
                    durable_failed_ = true;
                    recovery_.SnapshotUnavailable();
                } else if (result == DurableSessionError::kMempool)
                    transport_->ClosePeer(peer_id);
            } catch (...) {
                durable_failed_ = true;
                recovery_.SnapshotUnavailable();
            }
            return;
        }
        if (transaction == nullptr ||
            legacy_->transaction(*transaction) != RegtestNodeError::kNone) {
            transport_->ClosePeer(peer_id);
        } else {
            transport_->Broadcast(message, peer_id);
        }
    }
}

} // namespace nova::node
