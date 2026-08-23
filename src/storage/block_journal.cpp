#include "storage/block_journal.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <new>
#include <span>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace nova::storage
{
namespace
{

constexpr std::uint8_t kPrepareRecord = 1U;
constexpr std::uint8_t kCommitRecord = 2U;
std::atomic<JournalFailurePoint> g_failure_point{JournalFailurePoint::kNone};

[[nodiscard]] std::optional<std::uint32_t> ReadU32(std::ifstream& stream)
{
    std::array<unsigned char, 4U> bytes{};
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (stream.gcount() == 0 && stream.eof()) {
        return std::nullopt;
    }
    if (stream.gcount() != static_cast<std::streamsize>(bytes.size())) {
        return std::uint32_t{0};
    }
    return static_cast<std::uint32_t>(bytes[0U]) | (static_cast<std::uint32_t>(bytes[1U]) << 8U) |
           (static_cast<std::uint32_t>(bytes[2U]) << 16U) |
           (static_cast<std::uint32_t>(bytes[3U]) << 24U);
}

[[nodiscard]] std::optional<std::uint8_t> ReadU8(std::ifstream& stream)
{
    char byte{};
    stream.read(&byte, 1);
    if (stream.gcount() != 1) {
        return std::nullopt;
    }
    return static_cast<std::uint8_t>(static_cast<unsigned char>(byte));
}

[[nodiscard]] bool AppendDurably(const std::filesystem::path& path,
                                 const std::span<const std::uint8_t> record) noexcept
{
    const auto failure = g_failure_point.exchange(JournalFailurePoint::kNone);
    if (failure == JournalFailurePoint::kBeforeWrite) {
        return false;
    }
#ifdef _WIN32
    std::FILE* stream{};
    if (_wfopen_s(&stream, path.c_str(), L"ab") != 0) {
        stream = nullptr;
    }
#else
    std::FILE* stream = std::fopen(path.c_str(), "ab");
#endif
    if (stream == nullptr) {
        return false;
    }
    const auto write_size = failure == JournalFailurePoint::kDuringWrite
                                ? std::max<std::size_t>(1U, record.size() / 2U)
                                : record.size();
    const auto written = std::fwrite(record.data(), 1U, write_size, stream);
    const auto flushed = written == record.size() && std::fflush(stream) == 0;
#ifdef _WIN32
    const auto synchronized = flushed && _commit(_fileno(stream)) == 0;
#else
    const auto synchronized = flushed && fsync(fileno(stream)) == 0;
#endif
    const auto closed = std::fclose(stream) == 0;
    return closed && synchronized && failure != JournalFailurePoint::kAfterDurableWrite;
}

} // namespace

BlockJournal::BlockJournal(std::filesystem::path path, primitives::BlockLimits limits) noexcept
    : path_(std::move(path)), limits_(limits)
{
}

std::optional<BlockJournal> BlockJournal::Open(const std::filesystem::path& data_directory,
                                               primitives::BlockLimits limits) noexcept
{
    if (data_directory.empty() || limits.max_serialized_size == 0U) {
        return std::nullopt;
    }
    try {
        std::error_code error;
        std::filesystem::create_directories(data_directory, error);
        if (error) {
            return std::nullopt;
        }
        return BlockJournal{data_directory / "blocks.dat", limits};
    } catch (...) {
        return std::nullopt;
    }
}

BlockJournalError BlockJournal::Prepare(const primitives::Block& block) const noexcept
{
    try {
        primitives::BinaryWriter writer;
        if (!block.Serialize(writer, limits_) ||
            writer.bytes().size() > limits_.max_serialized_size) {
            return BlockJournalError::kMalformedRecord;
        }
        const auto size = writer.bytes().size();
        if (size > std::numeric_limits<std::uint32_t>::max()) {
            return BlockJournalError::kOversizedRecord;
        }
        std::vector<std::uint8_t> record;
        record.reserve(5U + size);
        record.push_back(kPrepareRecord);
        for (std::uint32_t shift = 0U; shift < 4U; ++shift) {
            record.push_back(static_cast<std::uint8_t>((size >> (shift * 8U)) & 0xFFU));
        }
        record.insert(record.end(), writer.bytes().begin(), writer.bytes().end());
        return AppendDurably(path_, record) ? BlockJournalError::kNone
                                            : BlockJournalError::kWriteFailure;
    } catch (const std::bad_alloc&) {
        return BlockJournalError::kAllocationFailure;
    } catch (...) {
        return BlockJournalError::kWriteFailure;
    }
}

BlockJournalError BlockJournal::Commit(const crypto::Hash256& block_hash) const noexcept
{
    try {
        std::array<std::uint8_t, 1U + crypto::Hash256::kSize> record{};
        record.front() = kCommitRecord;
        std::copy(block_hash.bytes().begin(), block_hash.bytes().end(), record.begin() + 1U);
        return AppendDurably(path_, record) ? BlockJournalError::kNone
                                            : BlockJournalError::kWriteFailure;
    } catch (...) {
        return BlockJournalError::kWriteFailure;
    }
}

BlockJournalLoadResult BlockJournal::Load() const noexcept
{
    try {
        if (!std::filesystem::exists(path_)) {
            return {};
        }
        std::ifstream stream{path_, std::ios::binary};
        if (!stream.is_open()) {
            return {BlockJournalError::kOpenFailure, {}};
        }
        std::vector<primitives::Block> blocks;
        std::vector<std::pair<crypto::Hash256, primitives::Block>> prepared;
        while (true) {
            const auto tag = ReadU8(stream);
            if (!tag.has_value()) {
                return {BlockJournalError::kNone, std::move(blocks)};
            }
            if (*tag == kPrepareRecord) {
                const auto size = ReadU32(stream);
                if (!size.has_value()) {
                    return {BlockJournalError::kNone, std::move(blocks)};
                }
                if (*size == 0U) {
                    // A torn final length field is an incomplete record, not
                    // a committed malformed record. Preserve the valid prefix.
                    if (stream.eof()) {
                        return {BlockJournalError::kNone, std::move(blocks)};
                    }
                    return {BlockJournalError::kMalformedRecord, {}};
                }
                if (*size > limits_.max_serialized_size) {
                    return {BlockJournalError::kOversizedRecord, {}};
                }
                std::vector<std::uint8_t> encoded(*size);
                stream.read(reinterpret_cast<char*>(encoded.data()),
                            static_cast<std::streamsize>(encoded.size()));
                if (stream.gcount() != static_cast<std::streamsize>(encoded.size())) {
                    return {BlockJournalError::kNone, std::move(blocks)};
                }
                const auto block = primitives::Block::Deserialize(encoded, limits_);
                const auto hash =
                    block.has_value() ? primitives::ComputeBlockHash(block->header) : std::nullopt;
                const auto duplicate =
                    hash.has_value() &&
                    std::any_of(prepared.begin(), prepared.end(),
                                [&hash](const auto& entry) { return entry.first == *hash; });
                if (!block.has_value() || !hash.has_value() || duplicate) {
                    return {BlockJournalError::kMalformedRecord, {}};
                }
                prepared.emplace_back(*hash, *block);
                continue;
            }
            if (*tag == kCommitRecord) {
                std::array<std::uint8_t, crypto::Hash256::kSize> bytes{};
                stream.read(reinterpret_cast<char*>(bytes.data()),
                            static_cast<std::streamsize>(bytes.size()));
                if (stream.gcount() != static_cast<std::streamsize>(bytes.size())) {
                    return {BlockJournalError::kNone, std::move(blocks)};
                }
                const crypto::Hash256 hash{bytes};
                const auto found =
                    std::find_if(prepared.begin(), prepared.end(),
                                 [&hash](const auto& entry) { return entry.first == hash; });
                if (found == prepared.end()) {
                    return {BlockJournalError::kMalformedRecord, {}};
                }
                blocks.push_back(std::move(found->second));
                prepared.erase(found);
                continue;
            }
            return {BlockJournalError::kMalformedRecord, {}};
        }
    } catch (const std::bad_alloc&) {
        return {BlockJournalError::kAllocationFailure, {}};
    } catch (...) {
        return {BlockJournalError::kFilesystemFailure, {}};
    }
}

const std::filesystem::path& BlockJournal::path() const noexcept
{
    return path_;
}

bool BlockJournal::IsCommitted(const crypto::Hash256& block_hash) const noexcept
{
    const auto loaded = Load();
    if (loaded.error != BlockJournalError::kNone) {
        return false;
    }
    return std::any_of(loaded.blocks.begin(), loaded.blocks.end(),
                       [&block_hash](const auto& block) {
                           const auto hash = primitives::ComputeBlockHash(block.header);
                           return hash.has_value() && *hash == block_hash;
                       });
}

void BlockJournal::SetFailurePointForTesting(const JournalFailurePoint point) noexcept
{
    g_failure_point.store(point);
}

} // namespace nova::storage
