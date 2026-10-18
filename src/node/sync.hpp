#pragma once

#include "chain/chainstate.hpp"
#include "net/p2p.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace nova::node
{

struct SyncParams final {
    chain::BlockValidationParams validation;
    consensus::DifficultyParameters difficulty;
    std::int32_t protocol_version{};
    std::uint32_t block_inventory_type{};
    std::uint32_t max_headers_per_response{};
    std::uint32_t max_pending_headers{};
};

enum class SyncError : std::uint8_t {
    kNone,
    kInvalidParameters,
    kUnexpectedMessage,
    kUnknownHeaderParent,
    kDuplicateHeader,
    kInvalidTarget,
    kBadProofOfWork,
    kWorkFailure,
    kWorkOverflow,
    kBlockRejected,
    kAllocationFailure,
    kChainStateUnavailable,
    kPendingHeaderLimit,
};

struct SyncResult final {
    SyncError error{SyncError::kNone};
    chain::ChainResult chain_result;
    std::vector<net::FramedMessage> requests;
};

using HeaderIndex = chain::HeaderMetadata;

class Synchronizer final
{
  public:
    Synchronizer(SyncParams parameters, chain::ChainState& chain) noexcept;

    [[nodiscard]] SyncResult Start() noexcept;
    [[nodiscard]] SyncResult Handle(const net::FramedMessage& message) noexcept;
    [[nodiscard]] const crypto::Hash256& best_header_tip() const noexcept;
    // Pending-only metadata, including validated entries until next refresh.
    [[nodiscard]] std::size_t header_count() const noexcept;
    [[nodiscard]] bool MissingBody(const crypto::Hash256& hash) const noexcept;

  private:
    struct HashLess final {
        [[nodiscard]] bool operator()(const crypto::Hash256& left,
                                      const crypto::Hash256& right) const noexcept;
    };

    [[nodiscard]] SyncResult AcceptHeaders(const net::HeadersMessage& headers) noexcept;
    [[nodiscard]] SyncError RefreshValidatedHeaders() noexcept;
    [[nodiscard]] std::optional<HeaderIndex> FindHeader(const crypto::Hash256& hash) const noexcept;
    [[nodiscard]] SyncResult AcceptBlock(const primitives::Block& block) noexcept;
    [[nodiscard]] std::optional<std::uint32_t>
    ExpectedBits(const HeaderIndex& parent, std::uint32_t candidate_time) const noexcept;

    SyncParams parameters_;
    chain::ChainState& chain_;
    std::map<crypto::Hash256, HeaderIndex, HashLess> headers_;
    crypto::Hash256 best_header_tip_;
};

} // namespace nova::node
