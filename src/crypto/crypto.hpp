#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace nova::crypto
{

using Hash160 = std::array<std::uint8_t, 20>;

[[nodiscard]] std::optional<Hash160> Hash160Digest(std::span<const std::uint8_t> message) noexcept;

class Hash256 final
{
  public:
    static constexpr std::size_t kSize = 32;
    using Bytes = std::array<std::uint8_t, kSize>;

    Hash256() = default;
    explicit Hash256(Bytes bytes) noexcept;

    [[nodiscard]] static std::optional<Hash256> FromHex(std::string_view hex) noexcept;
    [[nodiscard]] static std::optional<Hash256>
    Sha256(std::span<const std::uint8_t> message) noexcept;
    [[nodiscard]] static std::optional<Hash256>
    DoubleSha256(std::span<const std::uint8_t> message) noexcept;

    [[nodiscard]] const Bytes& bytes() const noexcept;

    friend bool operator==(const Hash256&, const Hash256&) = default;

  private:
    Bytes bytes_{};
};

class PublicKey;
class Signature;

class PrivateKey final
{
  public:
    static constexpr std::size_t kSize = 32;
    using Bytes = std::array<std::uint8_t, kSize>;

    PrivateKey(const PrivateKey&) = delete;
    PrivateKey& operator=(const PrivateKey&) = delete;
    PrivateKey(PrivateKey&& other) noexcept;
    PrivateKey& operator=(PrivateKey&& other) noexcept;
    ~PrivateKey();

    [[nodiscard]] static std::optional<PrivateKey> Generate() noexcept;
    [[nodiscard]] static std::optional<PrivateKey>
    FromBytes(std::span<const std::uint8_t> serialized) noexcept;

    // Export is for explicit secure-storage integrations only. Never log it.
    [[nodiscard]] const Bytes& bytes() const noexcept;
    [[nodiscard]] std::optional<PublicKey> DerivePublicKey() const noexcept;
    [[nodiscard]] std::optional<Signature> Sign(const Hash256& digest) const noexcept;

  private:
    PrivateKey() = default;

    void Clear() noexcept;

    Bytes bytes_{};
};

class PublicKey final
{
  public:
    static constexpr std::size_t kCompressedSize = 33;
    using CompressedBytes = std::array<std::uint8_t, kCompressedSize>;

    [[nodiscard]] static std::optional<PublicKey>
    FromCompressed(std::span<const std::uint8_t> serialized) noexcept;

    [[nodiscard]] const CompressedBytes& SerializeCompressed() const noexcept;
    [[nodiscard]] bool Verify(const Hash256& digest, const Signature& signature) const noexcept;

    friend bool operator==(const PublicKey&, const PublicKey&) = default;

  private:
    explicit PublicKey(CompressedBytes bytes) noexcept;

    CompressedBytes bytes_{};

    friend class PrivateKey;
};

class Signature final
{
  public:
    static constexpr std::size_t kMinimumDerSize = 8;
    static constexpr std::size_t kMaximumDerSize = 72;
    using DerBytes = std::array<std::uint8_t, kMaximumDerSize>;

    [[nodiscard]] static std::optional<Signature>
    FromDer(std::span<const std::uint8_t> serialized) noexcept;

    [[nodiscard]] std::span<const std::uint8_t> SerializeDer() const noexcept;

    friend bool operator==(const Signature&, const Signature&) = default;

  private:
    Signature(DerBytes der, std::size_t size) noexcept;

    DerBytes der_{};
    std::size_t size_{};

    friend class PrivateKey;
};

class KeyPair final
{
  public:
    KeyPair(const KeyPair&) = delete;
    KeyPair& operator=(const KeyPair&) = delete;
    KeyPair(KeyPair&&) noexcept = default;
    KeyPair& operator=(KeyPair&&) noexcept = default;

    [[nodiscard]] static std::optional<KeyPair> Generate() noexcept;
    [[nodiscard]] static std::optional<KeyPair> FromPrivateKey(PrivateKey&& private_key) noexcept;

    [[nodiscard]] const PrivateKey& private_key() const noexcept;
    [[nodiscard]] const PublicKey& public_key() const noexcept;
    [[nodiscard]] std::optional<Signature> Sign(const Hash256& digest) const noexcept;

  private:
    KeyPair(PrivateKey&& private_key, PublicKey public_key) noexcept;

    PrivateKey private_key_;
    PublicKey public_key_;
};

} // namespace nova::crypto
