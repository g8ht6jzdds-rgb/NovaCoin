#pragma once

#include "node/peer_service.hpp"
#include "node/staging_workspace.hpp"

#include <array>
#include <chrono>

namespace nova::node
{
// Single-threaded owner. The borrowed passphrase must outlive the runtime and
// be kept in protected caller-owned memory. Construction opens no listeners.
// All writer paths share this node owner; RPC is only an adapter over it.
class DurableNode final
{
  public:
    [[nodiscard]] static std::unique_ptr<DurableNode>
    Create(std::unique_ptr<node::DurableSession> session,
           std::unique_ptr<node::StagingWorkspace> workspace,
           const std::span<const std::uint8_t> passphrase,
           std::unique_ptr<storage::GenerationStore> directory_lease = nullptr) noexcept
    {
        if (!session || !workspace || passphrase.empty() || !session->View())
            return nullptr;
        try {
            auto runtime = std::unique_ptr<DurableNode>{new DurableNode{
                std::move(directory_lease), std::move(session), std::move(workspace), passphrase}};
            auto* self = runtime.get();
            runtime->peer_ = node::PeerService::Create(
                *runtime->session_,
                [self](const primitives::Block& block) { return self->ReceiveBlock(block); },
                [self](const primitives::Transaction& tx) { return self->ReceiveTransaction(tx); });
            if (!runtime->peer_)
                return nullptr;
            return runtime;
        } catch (...) {
            return nullptr;
        }
    }
    [[nodiscard]] bool Ready() const noexcept
    {
        return !shutdown_result_.has_value() && !failed_ && (!peer_ || peer_->DurableAvailable()) &&
               session_->View().has_value();
    }
    // Synchronous, single-owner shutdown: all accepted mutations were already
    // committed. Do not introduce a separate shutdown wallet/mempool writer.
    // A false result requires recovery; it is not evidence of rejection.
    [[nodiscard]] bool Shutdown() noexcept
    {
        if (shutdown_result_)
            return *shutdown_result_;
        const bool verified = Ready();
        shutdown_result_ = verified;
        peer_->QuarantineDurable();
        peer_->Close();
        return verified;
    }
    [[nodiscard]] std::optional<node::DurableSessionView> View() const noexcept
    {
        return Ready() ? session_->View() : std::nullopt;
    }
    [[nodiscard]] node::PeerService& Peer() noexcept
    {
        return *peer_;
    }
    [[nodiscard]] const DurableSession& Session() const noexcept
    {
        return *session_;
    }
    [[nodiscard]] observability::MetricsSnapshot Metrics() const noexcept
    {
        auto result = metrics_;
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                                 std::chrono::steady_clock::now() - started_)
                                 .count();
        result.uptime_seconds = seconds > 0 ? static_cast<std::uint64_t>(seconds) : 0U;
        return result;
    }
    [[nodiscard]] net::TransportResult Pump(std::uint64_t now) noexcept
    {
        const auto result = peer_->Pump(now);
        observability::SaturatingAdd(metrics_.p2p_messages_dispatched, result.messages_dispatched);
        observability::SaturatingAdd(metrics_.p2p_disconnects, result.disconnected);
        if (result.error != net::TransportError::kNone)
            observability::SaturatingAdd(metrics_.p2p_transport_errors, 1U);
        return result;
    }
    [[nodiscard]] std::optional<crypto::Hash256> UncertainPayment() const noexcept
    {
        return uncertain_payment_;
    }

  private:
    DurableNode(std::unique_ptr<storage::GenerationStore> directory_lease,
                std::unique_ptr<node::DurableSession> session,
                std::unique_ptr<node::StagingWorkspace> workspace,
                const std::span<const std::uint8_t> passphrase) noexcept
        : directory_lease_(std::move(directory_lease)), session_(std::move(session)),
          workspace_(std::move(workspace)), passphrase_(passphrase)
    {
    }
    [[nodiscard]] std::optional<std::filesystem::path> Next() noexcept
    {
        if (!Ready())
            return std::nullopt;
        auto path = workspace_->Next();
        if (!path)
            Quarantine();
        return path;
    }
    void Quarantine() noexcept
    {
        failed_ = true;
        if (peer_)
            peer_->QuarantineDurable();
    }
    void RecordBlock(const DurableSessionResult& result) noexcept
    {
        if (result.error == DurableSessionError::kNone) {
            observability::SaturatingAdd(metrics_.blocks_accepted, 1U);
            const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
            metrics_.last_block_arrival_time_seconds =
                now > 0 ? static_cast<std::uint64_t>(now) : 0U;
        } else if (result.error == DurableSessionError::kChain &&
                   result.chain_result.error != chain::ChainError::kDuplicateBlock) {
            observability::SaturatingAdd(metrics_.blocks_rejected, 1U);
        }
    }
    void RecordTransaction(DurableSessionError result) noexcept
    {
        if (result == DurableSessionError::kNone)
            observability::SaturatingAdd(metrics_.transactions_accepted, 1U);
        else if (result == DurableSessionError::kMempool)
            observability::SaturatingAdd(metrics_.transactions_rejected, 1U);
    }
    [[nodiscard]] bool Finish(const std::filesystem::path& path) noexcept
    {
        if (!workspace_->Cleanup(path)) {
            Quarantine();
            return false;
        }
        return true;
    }

  public:
    // Direct application ingress shares the same session and staging owner as
    // peer ingress. Receipt does not relay; the caller decides relay policy.
    [[nodiscard]] DurableSessionResult ReceiveBlock(const primitives::Block& block) noexcept
    {
        const auto path = Next();
        if (!path)
            return {};
        const auto result = session_->ApplyBlock(block, {}, *path, passphrase_);
        if (result.error == DurableSessionError::kUnavailable)
            Quarantine();
        if (!Finish(*path))
            return {DurableSessionError::kUnavailable, result.chain_result};
        RecordBlock(result);
        return result;
    }
    [[nodiscard]] std::optional<wallet::WalletAddress> GenerateAddress() noexcept
    {
        const auto path = Next();
        if (!path)
            return std::nullopt;
        const auto result = session_->GenerateAddress(*path, passphrase_);
        if (result.error == node::DurableSessionError::kUnavailable)
            Quarantine();
        if (!Finish(*path) || result.error != node::DurableSessionError::kNone)
            return std::nullopt;
        return result.address;
    }
    [[nodiscard]] DurableSessionError ReceiveTransaction(const primitives::Transaction& tx) noexcept
    {
        const auto path = Next();
        if (!path)
            return DurableSessionError::kUnavailable;
        const auto result = session_->AdmitTransaction(tx, *path, passphrase_);
        if (result == node::DurableSessionError::kUnavailable)
            Quarantine();
        if (!Finish(*path))
            return DurableSessionError::kUnavailable;
        RecordTransaction(result);
        return result;
    }
    [[nodiscard]] bool Submit(const primitives::Transaction& tx) noexcept
    {
        if (ReceiveTransaction(tx) != DurableSessionError::kNone)
            return false;
        peer_->RelayTransaction(tx);
        return true;
    }
    [[nodiscard]] std::optional<crypto::Hash256> Mine() noexcept
    {
        const auto path = Next();
        if (!path)
            return std::nullopt;
        const auto result = session_->MineBlock({}, *path, passphrase_);
        if (result.error == node::DurableSessionError::kUnavailable)
            Quarantine();
        if (!Finish(*path))
            return std::nullopt;
        RecordBlock(result);
        if (result.error != node::DurableSessionError::kNone || !result.chain_result.block_hash)
            return std::nullopt;
        const auto block = session_->FindBlock(*result.chain_result.block_hash);
        if (!block.available || !block.block || !block.block->block) {
            Quarantine();
            return std::nullopt;
        }
        peer_->RelayBlock(*block.block->block);
        return result.chain_result.block_hash;
    }
    [[nodiscard]] std::optional<crypto::Hash256> Send(const wallet::Recipient& recipient) noexcept
    {
        uncertain_payment_.reset();
        const auto path = Next();
        if (!path)
            return std::nullopt;
        const std::array<wallet::Recipient, 1U> recipients{recipient};
        const auto result = session_->BuildAndAdmit(recipients, *path, passphrase_);
        const auto candidate = result.transaction
                                   ? result.transaction->TxId(
                                         session_->network_params().block_limits.transaction_limits)
                                   : std::nullopt;
        if (result.error == node::DurableSessionError::kUnavailable)
            Quarantine();
        const bool cleaned = Finish(*path);
        if (!cleaned || result.error == node::DurableSessionError::kUnavailable)
            uncertain_payment_ = candidate;
        if (!cleaned || result.error != node::DurableSessionError::kNone || !result.transaction)
            return std::nullopt;
        RecordTransaction(result.error);
        const auto id = candidate;
        if (!id) {
            Quarantine();
            return std::nullopt;
        }
        peer_->RelayTransaction(*result.transaction);
        return id;
    }

  private:
    std::unique_ptr<storage::GenerationStore> directory_lease_;
    std::unique_ptr<node::DurableSession> session_;
    std::unique_ptr<node::StagingWorkspace> workspace_;
    std::span<const std::uint8_t> passphrase_;
    std::optional<crypto::Hash256> uncertain_payment_;
    bool failed_{};
    std::optional<bool> shutdown_result_;
    const std::chrono::steady_clock::time_point started_{std::chrono::steady_clock::now()};
    observability::MetricsSnapshot metrics_;
    std::unique_ptr<node::PeerService> peer_;
};
} // namespace nova::node
