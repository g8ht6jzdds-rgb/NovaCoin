#include "storage/generation_store.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size)
{
    if (size < 2U || size > 512U)
        return 0;
    const auto& expected = (data[0] & 1U) != 0U ? nova::consensus::TestnetNetworkParams()
                                                : nova::consensus::RegtestNetworkParams();
    const auto remaining = std::span{data, size}.subspan(2U);
    const auto length = std::min<std::size_t>(data[1], remaining.size());
    const auto root = remaining.first(length);
    const auto descriptor = remaining.subspan(length);
    const auto decoded = nova::storage::DecodeGenerationMetadata(root, descriptor, expected);
    using nova::storage::GenerationError;
    using nova::storage::GenerationState;
    if ((decoded.state == GenerationState::kUnavailable &&
         decoded.error == GenerationError::kNone) ||
        (decoded.state != GenerationState::kUnavailable &&
         decoded.error != GenerationError::kNone) ||
        (decoded.state == GenerationState::kEmpty &&
         (decoded.metadata.sequence != 0U || !descriptor.empty())) ||
        (decoded.state == GenerationState::kPresent &&
         (decoded.metadata.sequence == 0U || root.size() != 85U || descriptor.size() != 133U)))
        std::abort();
    return 0;
}
