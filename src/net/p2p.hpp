#pragma once

#include "primitives/block.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace nova::net
{

inline constexpr std::size_t kMessageHeaderSize = 24U;
inline constexpr std::size_t kCommandSize = 12U;

struct P2PParams final {
    std::uint32_t network_magic{};
    std::int32_t minimum_protocol_version{};
    std::uint32_t max_payload{};
    std::uint32_t max_user_agent{};
    std::uint32_t max_addresses{};
    std::uint32_t max_inventory{};
    std::uint32_t max_locator_hashes{};
    std::uint32_t max_headers{};
    primitives::TransactionLimits transaction_limits;
    primitives::BlockLimits block_limits;
};

struct VersionMessage final {
    std::int32_t version{};
    std::uint64_t services{};
    std::int64_t timestamp{};
    std::uint64_t nonce{};
    std::string user_agent;
    std::int32_t start_height{};
    bool relay{};
};

struct NonceMessage final {
    std::uint64_t nonce{};
};

struct NetworkAddress final {
    std::uint64_t services{};
    std::array<std::uint8_t, 16U> address{};
    std::uint16_t port{};
};

struct AddressMessage final {
    std::vector<NetworkAddress> addresses;
};

struct InventoryVector final {
    std::uint32_t type{};
    crypto::Hash256 hash;
};

struct InventoryMessage final {
    std::vector<InventoryVector> inventory;
};

struct GetHeadersMessage final {
    std::int32_t version{};
    std::vector<crypto::Hash256> locator_hashes;
    crypto::Hash256 stop_hash;
};

struct HeadersMessage final {
    std::vector<primitives::BlockHeader> headers;
};

using Message =
    std::variant<VersionMessage, std::monostate, NonceMessage, AddressMessage, InventoryMessage,
                 primitives::Transaction, primitives::Block, GetHeadersMessage, HeadersMessage>;

enum class Command : std::uint8_t {
    kVersion,
    kVerack,
    kPing,
    kPong,
    kAddr,
    kGetAddr,
    kInv,
    kGetData,
    kTx,
    kBlock,
    kGetHeaders,
    kHeaders,
};

struct FramedMessage final {
    Command command{Command::kVersion};
    Message message;
};

enum class P2PError : std::uint8_t {
    kNone,
    kInvalidParameters,
    kUnexpectedMagic,
    kMalformedCommand,
    kUnknownCommand,
    kPayloadTooLarge,
    kChecksumMismatch,
    kMalformedPayload,
    kAllocationFailure,
    kMessageLimit,
    kQueueLimit,
    kHandshakeViolation,
    kTimeout,
    kConnectionLimit,
    kUnknownPeer,
};

struct ParseResult final {
    P2PError error{P2PError::kNone};
    std::vector<FramedMessage> messages;
};

[[nodiscard]] std::optional<std::vector<std::uint8_t>>
SerializeMessage(const FramedMessage& message, const P2PParams& parameters) noexcept;
[[nodiscard]] std::optional<FramedMessage> DeserializeMessage(Command command,
                                                              std::span<const std::uint8_t> payload,
                                                              const P2PParams& parameters) noexcept;
[[nodiscard]] std::optional<Command> CommandFromString(std::string_view command) noexcept;
[[nodiscard]] std::string_view CommandToString(Command command) noexcept;

class MessageParser final
{
  public:
    explicit MessageParser(P2PParams parameters) noexcept;

    [[nodiscard]] ParseResult PushBytes(std::span<const std::uint8_t> bytes) noexcept;
    void Reset() noexcept;

  private:
    P2PParams parameters_;
    std::vector<std::uint8_t> buffered_;
};

enum class ConnectionDirection : std::uint8_t { kInbound, kOutbound };

struct ConnectionParams final {
    P2PParams protocol;
    VersionMessage local_version;
    std::uint64_t handshake_timeout{};
    std::uint64_t idle_timeout{};
    std::uint64_t message_window{};
    std::uint32_t max_messages_per_window{};
    std::uint32_t max_queued_messages{};
    std::uint64_t max_queued_bytes{};
};

struct ConnectionResult final {
    P2PError error{P2PError::kNone};
    std::vector<FramedMessage> messages;
};

class Connection final
{
  public:
    Connection(ConnectionDirection direction, ConnectionParams parameters,
               std::uint64_t now) noexcept;

    [[nodiscard]] P2PError Start() noexcept;
    [[nodiscard]] ConnectionResult Receive(std::span<const std::uint8_t> bytes,
                                           std::uint64_t now) noexcept;
    [[nodiscard]] P2PError Queue(const FramedMessage& message) noexcept;
    [[nodiscard]] std::vector<std::vector<std::uint8_t>> TakeOutbound() noexcept;
    [[nodiscard]] P2PError Tick(std::uint64_t now) noexcept;
    [[nodiscard]] bool established() const noexcept;
    [[nodiscard]] ConnectionDirection direction() const noexcept;

  private:
    [[nodiscard]] bool CheckParameters() const noexcept;
    [[nodiscard]] P2PError ProcessHandshake(const FramedMessage& message) noexcept;

    ConnectionDirection direction_;
    ConnectionParams parameters_;
    MessageParser parser_;
    std::vector<std::vector<std::uint8_t>> outbound_;
    std::uint64_t connected_at_{};
    std::uint64_t last_received_at_{};
    std::uint64_t window_started_at_{};
    std::uint64_t queued_bytes_{};
    std::uint32_t messages_in_window_{};
    bool local_version_sent_{};
    bool remote_version_received_{};
    bool remote_verack_received_{};
};

struct Peer final {
    std::uint64_t id{};
    ConnectionDirection direction{ConnectionDirection::kInbound};
    Connection connection;
};

struct PeerManagerParams final {
    ConnectionParams connection;
    std::uint32_t max_inbound_peers{};
    std::uint32_t max_outbound_peers{};
};

struct PeerEvent final {
    std::uint64_t peer_id{};
    FramedMessage message;
};

struct PeerReceiveResult final {
    P2PError error{P2PError::kNone};
    std::vector<PeerEvent> events;
};

class PeerManager final
{
  public:
    explicit PeerManager(PeerManagerParams parameters) noexcept;

    [[nodiscard]] std::optional<std::uint64_t> AddInbound(std::uint64_t now) noexcept;
    [[nodiscard]] std::optional<std::uint64_t> AddOutbound(std::uint64_t now) noexcept;
    [[nodiscard]] PeerReceiveResult
    Receive(std::uint64_t peer_id, std::span<const std::uint8_t> bytes, std::uint64_t now) noexcept;
    [[nodiscard]] P2PError Queue(std::uint64_t peer_id, const FramedMessage& message) noexcept;
    [[nodiscard]] std::vector<std::vector<std::uint8_t>>
    TakeOutbound(std::uint64_t peer_id) noexcept;
    void Tick(std::uint64_t now) noexcept;
    // The transport owns sockets and explicitly releases protocol state when
    // one closes, so peer limits cannot be exhausted by stale entries.
    void Disconnect(std::uint64_t peer_id) noexcept;
    [[nodiscard]] std::size_t peer_count() const noexcept;

  private:
    [[nodiscard]] std::optional<std::uint64_t> Add(ConnectionDirection direction,
                                                   std::uint64_t now) noexcept;
    void Remove(std::uint64_t peer_id) noexcept;

    PeerManagerParams parameters_;
    std::map<std::uint64_t, Peer> peers_;
    std::uint64_t next_peer_id_{1U};
};

} // namespace nova::net
