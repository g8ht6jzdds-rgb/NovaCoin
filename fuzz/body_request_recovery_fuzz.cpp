#include "node/body_request_recovery.hpp"

#include <cstdlib>

namespace
{
class Sink final : public nova::node::BodyRequestRecovery::Sink
{
  public:
    bool missing{true}, fail{true};
    nova::node::BodyRequestRecovery::BodyState
    State(const nova::crypto::Hash256&) const noexcept override
    {
        return missing ? nova::node::BodyRequestRecovery::BodyState::kPending
                       : nova::node::BodyRequestRecovery::BodyState::kValidated;
    }
    nova::net::TransportError QueueBody(std::uint64_t,
                                        const nova::net::FramedMessage& frame) noexcept override
    {
        const auto* inventory = std::get_if<nova::net::InventoryMessage>(&frame.message);
        if (inventory == nullptr || inventory->inventory.size() > 2000U)
            std::abort();
        return fail ? nova::net::TransportError::kQueueLimit : nova::net::TransportError::kNone;
    }
};
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size)
{
    if (size > 512U)
        return 0;
    nova::node::BodyRequestRecovery recovery;
    Sink sink;
    nova::net::TransportPeer peer{};
    peer.id = 7U;
    peer.status.established = true;
    std::uint64_t now{};
    for (std::size_t i = 0U; i < size; ++i) {
        nova::crypto::Hash256::Bytes bytes{};
        bytes.front() = data[i];
        switch (data[i] % 6U) {
        case 0U:
            recovery.Submit(7U, {{{2U, nova::crypto::Hash256{bytes}}}}, now, sink);
            break;
        case 1U:
            now += data[i];
            recovery.Tick(now, {&peer, 1U}, sink);
            break;
        case 2U:
            sink.fail = !sink.fail;
            break;
        case 3U:
            sink.missing = !sink.missing;
            break;
        case 4U:
            recovery.SnapshotUnavailable();
            now = 0U;
            break;
        default:
            recovery.Tick(now, {}, sink);
            break;
        }
        if (recovery.size() > nova::node::BodyRequestRecovery::kCapacity)
            std::abort();
    }
    return 0;
}
