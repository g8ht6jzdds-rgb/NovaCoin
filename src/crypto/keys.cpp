#include "crypto/crypto.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <utility>

#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <secp256k1.h>

namespace nova::crypto
{
namespace
{

struct ContextDeleter final {
    void operator()(secp256k1_context* context) const noexcept
    {
        secp256k1_context_destroy(context);
    }
};

using ContextPtr = std::unique_ptr<secp256k1_context, ContextDeleter>;

[[nodiscard]] secp256k1_context* Context() noexcept
{
    static const ContextPtr context = []() noexcept {
        ContextPtr created{
            secp256k1_context_create(SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY)};
        if (created == nullptr) {
            return ContextPtr{};
        }

        std::array<std::uint8_t, 32> seed{};
        const auto random_ok = RAND_priv_bytes(seed.data(), static_cast<int>(seed.size())) == 1;
        const auto randomized =
            random_ok && secp256k1_context_randomize(created.get(), seed.data()) == 1;
        OPENSSL_cleanse(seed.data(), seed.size());
        if (!randomized) {
            return ContextPtr{};
        }
        return created;
    }();
    return context.get();
}

constexpr std::array<std::uint8_t, 32> kCurveOrder{
    0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
    0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFEU, 0xBAU, 0xAEU, 0xDCU, 0xE6U, 0xAFU, 0x48U,
    0xA0U, 0x3BU, 0xBFU, 0xD2U, 0x5EU, 0x8CU, 0xD0U, 0x36U, 0x41U,
};

[[nodiscard]] bool IsValidScalar(const std::span<const std::uint8_t> scalar) noexcept
{
    if (scalar.size() != kCurveOrder.size() ||
        std::all_of(scalar.begin(), scalar.end(),
                    [](const std::uint8_t value) { return value == 0U; })) {
        return false;
    }
    return std::lexicographical_compare(scalar.begin(), scalar.end(), kCurveOrder.begin(),
                                        kCurveOrder.end());
}

[[nodiscard]] std::optional<secp256k1_ecdsa_signature>
ParseCanonicalSignature(const std::span<const std::uint8_t> serialized) noexcept
{
    const auto* context = Context();
    if (context == nullptr || serialized.size() < Signature::kMinimumDerSize ||
        serialized.size() > Signature::kMaximumDerSize) {
        return std::nullopt;
    }

    secp256k1_ecdsa_signature parsed{};
    if (secp256k1_ecdsa_signature_parse_der(context, &parsed, serialized.data(),
                                            serialized.size()) != 1) {
        return std::nullopt;
    }

    std::array<std::uint8_t, Signature::kMaximumDerSize> canonical{};
    std::size_t canonical_size = canonical.size();
    if (secp256k1_ecdsa_signature_serialize_der(context, canonical.data(), &canonical_size,
                                                &parsed) != 1 ||
        canonical_size != serialized.size() ||
        !std::equal(canonical.begin(),
                    canonical.begin() + static_cast<std::ptrdiff_t>(canonical_size),
                    serialized.begin())) {
        return std::nullopt;
    }

    std::array<std::uint8_t, 64> compact{};
    if (secp256k1_ecdsa_signature_serialize_compact(context, compact.data(), &parsed) != 1 ||
        !IsValidScalar(std::span<const std::uint8_t>{compact}.first(32)) ||
        !IsValidScalar(std::span<const std::uint8_t>{compact}.last(32))) {
        return std::nullopt;
    }

    secp256k1_ecdsa_signature normalized{};
    if (secp256k1_ecdsa_signature_normalize(context, &normalized, &parsed) != 0) {
        return std::nullopt;
    }
    return parsed;
}

} // namespace

PrivateKey::PrivateKey(PrivateKey&& other) noexcept : bytes_(other.bytes_)
{
    other.Clear();
}

PrivateKey& PrivateKey::operator=(PrivateKey&& other) noexcept
{
    if (this != &other) {
        Clear();
        bytes_ = other.bytes_;
        other.Clear();
    }
    return *this;
}

PrivateKey::~PrivateKey()
{
    Clear();
}

std::optional<PrivateKey> PrivateKey::Generate() noexcept
{
    Bytes candidate{};
    for (std::size_t attempt = 0; attempt < 128U; ++attempt) {
        if (RAND_priv_bytes(candidate.data(), static_cast<int>(candidate.size())) != 1) {
            OPENSSL_cleanse(candidate.data(), candidate.size());
            return std::nullopt;
        }
        auto key = FromBytes(candidate);
        OPENSSL_cleanse(candidate.data(), candidate.size());
        if (key.has_value()) {
            return key;
        }
    }
    OPENSSL_cleanse(candidate.data(), candidate.size());
    return std::nullopt;
}

std::optional<PrivateKey>
PrivateKey::FromBytes(const std::span<const std::uint8_t> serialized) noexcept
{
    const auto* context = Context();
    if (context == nullptr || serialized.size() != kSize ||
        secp256k1_ec_seckey_verify(context, serialized.data()) != 1) {
        return std::nullopt;
    }

    PrivateKey key;
    std::copy(serialized.begin(), serialized.end(), key.bytes_.begin());
    return key;
}

const PrivateKey::Bytes& PrivateKey::bytes() const noexcept
{
    return bytes_;
}

std::optional<PublicKey> PrivateKey::DerivePublicKey() const noexcept
{
    const auto* context = Context();
    if (context == nullptr) {
        return std::nullopt;
    }

    secp256k1_pubkey parsed{};
    if (secp256k1_ec_pubkey_create(context, &parsed, bytes_.data()) != 1) {
        return std::nullopt;
    }

    PublicKey::CompressedBytes serialized{};
    std::size_t serialized_size = serialized.size();
    if (secp256k1_ec_pubkey_serialize(context, serialized.data(), &serialized_size, &parsed,
                                      SECP256K1_EC_COMPRESSED) != 1 ||
        serialized_size != serialized.size()) {
        return std::nullopt;
    }
    return PublicKey{serialized};
}

std::optional<Signature> PrivateKey::Sign(const Hash256& digest) const noexcept
{
    const auto* context = Context();
    if (context == nullptr) {
        return std::nullopt;
    }

    secp256k1_ecdsa_signature signature{};
    if (secp256k1_ecdsa_sign(context, &signature, digest.bytes().data(), bytes_.data(), nullptr,
                             nullptr) != 1) {
        return std::nullopt;
    }

    std::array<std::uint8_t, Signature::kMaximumDerSize> serialized{};
    std::size_t serialized_size = serialized.size();
    if (secp256k1_ecdsa_signature_serialize_der(context, serialized.data(), &serialized_size,
                                                &signature) != 1) {
        return std::nullopt;
    }
    return Signature::FromDer(std::span<const std::uint8_t>{serialized}.first(serialized_size));
}

void PrivateKey::Clear() noexcept
{
    OPENSSL_cleanse(bytes_.data(), bytes_.size());
}

PublicKey::PublicKey(CompressedBytes bytes) noexcept : bytes_(bytes) {}

std::optional<PublicKey>
PublicKey::FromCompressed(const std::span<const std::uint8_t> serialized) noexcept
{
    const auto* context = Context();
    if (context == nullptr || serialized.size() != kCompressedSize ||
        (serialized.front() != 0x02U && serialized.front() != 0x03U)) {
        return std::nullopt;
    }

    secp256k1_pubkey parsed{};
    if (secp256k1_ec_pubkey_parse(context, &parsed, serialized.data(), serialized.size()) != 1) {
        return std::nullopt;
    }

    CompressedBytes canonical{};
    std::size_t canonical_size = canonical.size();
    if (secp256k1_ec_pubkey_serialize(context, canonical.data(), &canonical_size, &parsed,
                                      SECP256K1_EC_COMPRESSED) != 1 ||
        canonical_size != canonical.size() ||
        !std::equal(canonical.begin(), canonical.end(), serialized.begin())) {
        return std::nullopt;
    }
    return PublicKey{canonical};
}

const PublicKey::CompressedBytes& PublicKey::SerializeCompressed() const noexcept
{
    return bytes_;
}

bool PublicKey::Verify(const Hash256& digest, const Signature& signature) const noexcept
{
    const auto* context = Context();
    if (context == nullptr) {
        return false;
    }

    secp256k1_pubkey public_key{};
    const auto public_ok =
        secp256k1_ec_pubkey_parse(context, &public_key, bytes_.data(), bytes_.size());
    const auto parsed_signature = Signature::FromDer(signature.SerializeDer());
    if (public_ok != 1 || !parsed_signature.has_value()) {
        return false;
    }

    const auto native_signature = ParseCanonicalSignature(parsed_signature->SerializeDer());
    if (!native_signature.has_value()) {
        return false;
    }
    return secp256k1_ecdsa_verify(context, &*native_signature, digest.bytes().data(),
                                  &public_key) == 1;
}

Signature::Signature(DerBytes der, const std::size_t size) noexcept : der_(der), size_(size) {}

std::optional<Signature> Signature::FromDer(const std::span<const std::uint8_t> serialized) noexcept
{
    if (!ParseCanonicalSignature(serialized).has_value()) {
        return std::nullopt;
    }
    DerBytes der{};
    std::copy(serialized.begin(), serialized.end(), der.begin());
    return Signature{der, serialized.size()};
}

std::span<const std::uint8_t> Signature::SerializeDer() const noexcept
{
    return std::span<const std::uint8_t>{der_}.first(size_);
}

std::optional<KeyPair> KeyPair::Generate() noexcept
{
    auto private_key = PrivateKey::Generate();
    if (!private_key.has_value()) {
        return std::nullopt;
    }
    return FromPrivateKey(std::move(*private_key));
}

std::optional<KeyPair> KeyPair::FromPrivateKey(PrivateKey&& private_key) noexcept
{
    auto public_key = private_key.DerivePublicKey();
    if (!public_key.has_value()) {
        return std::nullopt;
    }
    return KeyPair{std::move(private_key), *public_key};
}

const PrivateKey& KeyPair::private_key() const noexcept
{
    return private_key_;
}

const PublicKey& KeyPair::public_key() const noexcept
{
    return public_key_;
}

std::optional<Signature> KeyPair::Sign(const Hash256& digest) const noexcept
{
    return private_key_.Sign(digest);
}

KeyPair::KeyPair(PrivateKey&& private_key, PublicKey public_key) noexcept
    : private_key_(std::move(private_key)), public_key_(public_key)
{
}

} // namespace nova::crypto
