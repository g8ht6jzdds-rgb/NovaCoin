#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace nova::inspection
{

// This identity is supplied by build/version_data.hpp at the command-line
// boundary.  Rendering never invokes Git or examines the checkout at runtime.
struct FreezeCorpusIdentity final {
    std::string_view commit;
    std::string_view tree;
    std::string_view state;
};

// Returns the canonical, LF-terminated REGTEST/TESTNET parameter-and-vector
// corpus. Only clean or archive-declared source identities are eligible.
[[nodiscard]] std::optional<std::string>
RenderFreezeCorpus(const FreezeCorpusIdentity& identity) noexcept;

// A release-facing checksum manifest must use exactly
// "<64 lowercase hex><two spaces><relative slash path>\\n" per entry.
// This parser is intentionally grammar-only; it neither opens files nor
// interprets a manifest as an instruction to access a path.
[[nodiscard]] bool IsCanonicalSha256Manifest(std::string_view manifest) noexcept;

} // namespace nova::inspection
