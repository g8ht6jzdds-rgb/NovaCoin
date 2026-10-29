#include "storage/generation_store.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace nova::storage
{
namespace
{
constexpr std::array<std::uint8_t, 8U> kRootMagic{'N', 'V', 'R', 'O', 'O', 'T', '0', '1'};
constexpr std::array<std::uint8_t, 8U> kDescriptorMagic{'N', 'V', 'G', 'E', 'N', '0', '0', '1'};
constexpr std::size_t kIdentitySize = 1U + 4U + crypto::Hash256::kSize;
constexpr std::size_t kRootSize = 8U + kIdentitySize + 8U + crypto::Hash256::kSize;
constexpr std::size_t kDescriptorSize =
    8U + kIdentitySize + 8U + 2U * (8U + crypto::Hash256::kSize);
using RootBytes = std::array<std::uint8_t, kRootSize>;
using DescriptorBytes = std::array<std::uint8_t, kDescriptorSize>;
constexpr const char* kRootName = "generation.current";

class File final
{
  public:
#ifdef _WIN32
    using Handle = HANDLE;
    static Handle Invalid() noexcept
    {
        return INVALID_HANDLE_VALUE;
    }
#else
    using Handle = int;
    static constexpr Handle Invalid() noexcept
    {
        return -1;
    }
#endif
    explicit File(const Handle handle = Invalid()) noexcept : handle_(handle) {}
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    File(File&& other) noexcept : handle_(std::exchange(other.handle_, Invalid())) {}
    File& operator=(File&&) = delete;
    ~File()
    {
        if (valid()) {
#ifdef _WIN32
            (void)CloseHandle(handle_);
#else
            (void)close(handle_);
#endif
        }
    }
    bool valid() const noexcept
    {
        return handle_ != Invalid();
    }
    Handle handle() const noexcept
    {
        return handle_;
    }
    std::optional<std::uint64_t> Size() const noexcept
    {
#ifdef _WIN32
        BY_HANDLE_FILE_INFORMATION info{};
        if (!valid() || GetFileType(handle_) != FILE_TYPE_DISK ||
            !GetFileInformationByHandle(handle_, &info) ||
            (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) !=
                0U ||
            info.nNumberOfLinks != 1U)
            return std::nullopt;
        return (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32U) | info.nFileSizeLow;
#else
        struct stat info {
        };
        if (!valid() || fstat(handle_, &info) != 0 || !S_ISREG(info.st_mode) ||
            info.st_nlink != 1 || info.st_size < 0)
            return std::nullopt;
        return static_cast<std::uint64_t>(info.st_size);
#endif
    }
    std::optional<std::size_t> Read(const std::span<std::uint8_t> bytes) const noexcept
    {
#ifdef _WIN32
        if (bytes.size() > std::numeric_limits<DWORD>::max())
            return std::nullopt;
        DWORD count{};
        if (!ReadFile(handle_, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr))
            return std::nullopt;
        return static_cast<std::size_t>(count);
#else
        ssize_t count;
        do {
            count = read(handle_, bytes.data(), bytes.size());
        } while (count < 0 && errno == EINTR);
        if (count < 0)
            return std::nullopt;
        return static_cast<std::size_t>(count);
#endif
    }
    bool Write(std::span<const std::uint8_t> bytes) const noexcept
    {
        while (!bytes.empty()) {
#ifdef _WIN32
            const auto request = static_cast<DWORD>(std::min<std::size_t>(bytes.size(), 65536U));
            DWORD count{};
            if (!WriteFile(handle_, bytes.data(), request, &count, nullptr) || count == 0U)
                return false;
#else
            ssize_t count;
            do {
                count = write(handle_, bytes.data(), bytes.size());
            } while (count < 0 && errno == EINTR);
            if (count <= 0)
                return false;
#endif
            bytes = bytes.subspan(static_cast<std::size_t>(count));
        }
        return true;
    }
    bool Sync() const noexcept
    {
#ifdef _WIN32
        return FlushFileBuffers(handle_) != 0;
#else
        return fsync(handle_) == 0;
#endif
    }

  private:
    Handle handle_;
};

// Reject links in the supplied path before opening; all store members are then
// addressed by generated basenames. Custodians must prevent hostile local edits
// of ancestor directories; this is not a sandbox against the host administrator.
bool PlainPath(const std::filesystem::path& path)
{
    if (!path.is_absolute())
        return false;
    std::filesystem::path part;
    for (const auto& component : path) {
        part /= component;
        std::error_code error;
        const auto status = std::filesystem::symlink_status(part, error);
        if (error || std::filesystem::is_symlink(status))
            return false;
#ifdef _WIN32
        const auto attrs = GetFileAttributesW(part.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)
            return false;
#endif
    }
    return true;
}

File OpenPath(const std::filesystem::path& path, const bool create)
{
#ifdef _WIN32
    return File{CreateFileW(path.c_str(), create ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ,
                            FILE_SHARE_READ, nullptr, create ? CREATE_NEW : OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
#else
    return File{
        open(path.c_str(),
             (create ? O_RDWR | O_CREAT | O_EXCL : O_RDONLY) | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK,
             0600)};
#endif
}

struct Directory final {
    std::filesystem::path path;
    File directory;
    File lock;

    explicit Directory(std::filesystem::path root)
        : path(std::move(root)),
#ifdef _WIN32
          directory(CreateFileW(
              path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
              OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)),
          lock(CreateFileW((path / "generation.lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                           nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr))
#else
          directory(open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)),
          lock(directory.valid()
                   ? openat(directory.handle(), "generation.lock",
                            O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600)
                   : -1)
#endif
    {
    }
    bool Locked() const noexcept
    {
        if (!directory.valid() || !lock.Size().has_value())
            return false;
#ifndef _WIN32
        return flock(lock.handle(), LOCK_EX | LOCK_NB) == 0;
#else
        return true;
#endif
    }
    File Open(const std::string& name, const bool create = false) const
    {
#ifdef _WIN32
        return OpenPath(path / name, create);
#else
        return File{openat(directory.handle(), name.c_str(),
                           (create ? O_RDWR | O_CREAT | O_EXCL : O_RDONLY) | O_CLOEXEC |
                               O_NOFOLLOW | O_NONBLOCK,
                           0600)};
#endif
    }
    bool WriteNew(const std::string& name, const std::span<const std::uint8_t> bytes) const
    {
        auto file = Open(name, true);
        return file.Size().has_value() && file.Write(bytes) && file.Sync();
    }
    bool Replace(const std::string& temporary) const
    {
#ifdef _WIN32
        return MoveFileExW((path / temporary).c_str(), (path / kRootName).c_str(),
                           MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
        return renameat(directory.handle(), temporary.c_str(), directory.handle(), kRootName) == 0;
#endif
    }
    bool Sync() const noexcept
    {
#ifdef _WIN32
        // Windows uses flushed member files and WRITE_THROUGH root replacement.
        // No directory-fsync/physical-power-loss equivalence is asserted.
        return true;
#else
        return directory.Sync();
#endif
    }
};

template <std::size_t N>
bool ReadExact(const Directory& directory, const std::string& name,
               std::array<std::uint8_t, N>& bytes)
{
    const auto file = directory.Open(name);
    const auto size = file.Size();
    if (!size.has_value() || *size != N)
        return false;
    std::size_t offset = 0U;
    while (offset < N) {
        const auto count = file.Read(std::span{bytes}.subspan(offset));
        if (!count.has_value() || *count == 0U)
            return false;
        offset += *count;
    }
    std::array<std::uint8_t, 1U> extra{};
    return file.Read(extra) == 0U && file.Size() == size;
}

void Put64(const std::span<std::uint8_t> bytes, const std::size_t offset, const std::uint64_t value)
{
    for (std::size_t i = 0; i < 8U; ++i)
        bytes[offset + i] = static_cast<std::uint8_t>(value >> (8U * i));
}
std::uint64_t Get64(const std::span<const std::uint8_t> bytes, const std::size_t offset)
{
    std::uint64_t result{};
    for (std::size_t i = 0; i < 8U; ++i)
        result |= static_cast<std::uint64_t>(bytes[offset + i]) << (8U * i);
    return result;
}
std::string Name(const std::uint64_t sequence, const char* extension)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string result = "generation-";
    for (std::size_t i = 0; i < 16U; ++i)
        result += digits[(sequence >> (4U * (15U - i))) & 15U];
    return result + extension;
}

struct Image final {
    std::uint64_t size{};
    crypto::Hash256 hash;
};
std::optional<Image> ProcessImage(const File& source, const File* destination = nullptr)
{
    const auto size = source.Size();
    if (!size.has_value())
        return std::nullopt;
    using Context = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
    Context digest{EVP_MD_CTX_new(), EVP_MD_CTX_free};
    if (!digest || EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) != 1)
        return std::nullopt;
    std::array<std::uint8_t, 65536U> buffer{};
    std::uint64_t remaining = *size;
    while (remaining > 0U) {
        const auto wanted =
            static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
        const auto count = source.Read(std::span{buffer}.first(wanted));
        if (!count.has_value() || *count == 0U)
            return std::nullopt;
        const auto data = std::span{buffer}.first(*count);
        if (EVP_DigestUpdate(digest.get(), data.data(), data.size()) != 1 ||
            (destination != nullptr && !destination->Write(data)))
            return std::nullopt;
        remaining -= *count;
    }
    std::array<std::uint8_t, 1U> extra{};
    if (source.Read(extra) != 0U || source.Size() != size ||
        (destination != nullptr && (!destination->Sync() || destination->Size() != size)))
        return std::nullopt;
    crypto::Hash256::Bytes hash{};
    unsigned int length{};
    if (EVP_DigestFinal_ex(digest.get(), hash.data(), &length) != 1 || length != hash.size())
        return std::nullopt;
    return Image{*size, crypto::Hash256{hash}};
}

std::optional<Image> CopyImage(const Directory& directory, const std::filesystem::path& source,
                               const std::string& name)
{
    if (!PlainPath(source))
        return std::nullopt;
    auto input = OpenPath(source, false);
    auto output = directory.Open(name, true);
    if (!output.Size().has_value())
        return std::nullopt;
    return ProcessImage(input, &output);
}
crypto::Hash256 GetHash(const std::span<const std::uint8_t> bytes, const std::size_t offset)
{
    crypto::Hash256::Bytes hash{};
    std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), hash.size(), hash.begin());
    return crypto::Hash256{hash};
}
void PutHash(const std::span<std::uint8_t> bytes, const std::size_t offset,
             const crypto::Hash256& hash)
{
    std::copy(hash.bytes().begin(), hash.bytes().end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(offset));
}
GenerationMetadataResult Decode(const std::span<const std::uint8_t> root,
                                const std::span<const std::uint8_t> descriptor,
                                const std::span<const std::uint8_t> identity) noexcept
{
    if (root.size() != kRootSize || !std::equal(kRootMagic.begin(), kRootMagic.end(), root.begin()))
        return {};
    if (!std::equal(identity.begin(), identity.end(), root.begin() + 8U))
        return {GenerationState::kUnavailable, GenerationError::kIdentityMismatch, {}};
    const auto sequence = Get64(root, 8U + kIdentitySize);
    if (sequence == 0U) {
        if (!descriptor.empty() ||
            GetHash(root, kRootSize - crypto::Hash256::kSize) != crypto::Hash256{})
            return {};
        return {GenerationState::kEmpty, GenerationError::kNone, {}};
    }
    if (descriptor.size() != kDescriptorSize)
        return {};
    const auto digest = crypto::Hash256::Sha256(descriptor);
    if (!digest || *digest != GetHash(root, kRootSize - crypto::Hash256::kSize))
        return {GenerationState::kUnavailable, GenerationError::kHashFailure, {}};
    if (!std::equal(kDescriptorMagic.begin(), kDescriptorMagic.end(), descriptor.begin()) ||
        !std::equal(identity.begin(), identity.end(), descriptor.begin() + 8U) ||
        Get64(descriptor, 8U + kIdentitySize) != sequence)
        return {};
    constexpr std::size_t offset = 8U + kIdentitySize + 8U;
    return {GenerationState::kPresent,
            GenerationError::kNone,
            {sequence, Get64(descriptor, offset),
             Get64(descriptor, offset + 8U + crypto::Hash256::kSize),
             GetHash(descriptor, offset + 8U),
             GetHash(descriptor, offset + 16U + crypto::Hash256::kSize)}};
}

bool CandidateMatches(const Directory& directory, const RootBytes& root,
                      const std::span<const std::uint8_t> identity)
{
    const auto sequence = Get64(root, 8U + kIdentitySize);
    DescriptorBytes descriptor{};
    if (!ReadExact(directory, Name(sequence, ".descriptor"), descriptor))
        return false;
    const auto decoded = Decode(root, descriptor, identity);
    if (decoded.state != GenerationState::kPresent)
        return false;
    const auto journal = ProcessImage(directory.Open(Name(sequence, ".journal")));
    const auto wallet = ProcessImage(directory.Open(Name(sequence, ".wallet")));
    return journal && wallet && journal->size == decoded.metadata.journal_size &&
           journal->hash == decoded.metadata.journal_hash &&
           wallet->size == decoded.metadata.wallet_size &&
           wallet->hash == decoded.metadata.wallet_hash;
}
} // namespace

GenerationMetadataResult DecodeGenerationMetadata(const std::span<const std::uint8_t> root,
                                                  const std::span<const std::uint8_t> descriptor,
                                                  const consensus::NetworkParams& expected) noexcept
{
    std::array<std::uint8_t, kIdentitySize> identity{};
    identity[0] = static_cast<std::uint8_t>(expected.id);
    for (std::size_t i = 0; i < 4U; ++i)
        identity[1U + i] = static_cast<std::uint8_t>(expected.network_magic >> (8U * i));
    std::copy(expected.genesis_hash.bytes().begin(), expected.genesis_hash.bytes().end(),
              identity.begin() + 5U);
    return Decode(root, descriptor, identity);
}

struct GenerationStore::State final {
    Directory directory;
    std::array<std::uint8_t, kIdentitySize> identity{};
    std::shared_ptr<const std::uint8_t> owner{std::make_shared<const std::uint8_t>(0U)};
    mutable GenerationFailurePoint failure{GenerationFailurePoint::kNone};

    State(std::filesystem::path path, const consensus::NetworkParams& network)
        : directory(std::move(path))
    {
        identity[0] = static_cast<std::uint8_t>(network.id);
        for (std::size_t i = 0; i < 4U; ++i)
            identity[1U + i] = static_cast<std::uint8_t>(network.network_magic >> (8U * i));
        std::copy(network.genesis_hash.bytes().begin(), network.genesis_hash.bytes().end(),
                  identity.begin() + 5U);
    }
    bool Fail(const GenerationFailurePoint point) const noexcept
    {
        if (failure != point)
            return false;
        failure = GenerationFailurePoint::kNone;
        return true;
    }
    bool ValidIdentity(const std::span<const std::uint8_t> bytes) const
    {
        return std::equal(identity.begin(), identity.end(), bytes.begin() + 8U);
    }
    template <std::size_t N>
    void Header(std::array<std::uint8_t, N>& bytes, const std::array<std::uint8_t, 8U>& magic,
                const std::uint64_t sequence) const
    {
        std::copy(magic.begin(), magic.end(), bytes.begin());
        std::copy(identity.begin(), identity.end(), bytes.begin() + 8U);
        Put64(bytes, 8U + kIdentitySize, sequence);
    }
};

struct GenerationStore::Prepared::Data final {
    std::weak_ptr<const std::uint8_t> owner;
    RootBytes before{};
    RootBytes after{};
    std::string root_temporary;
    bool attempted{};
};

GenerationStore::GenerationStore(std::unique_ptr<State> state) noexcept : state_(std::move(state))
{
}
GenerationStore::~GenerationStore() = default;
GenerationStore::Prepared::Prepared(std::unique_ptr<Data> data) noexcept : data_(std::move(data)) {}
GenerationStore::Prepared::~Prepared() = default;

std::unique_ptr<GenerationStore> GenerationStore::Open(const std::filesystem::path& directory,
                                                       const consensus::NetworkParams& network,
                                                       GenerationError& error) noexcept
{
    error = GenerationError::kInvalidPath;
    try {
        if (!PlainPath(directory) || !std::filesystem::is_directory(directory) ||
            consensus::CheckNetworkParams(network) != consensus::NetworkParamsError::kNone ||
            !network.enabled)
            return nullptr;
        auto state = std::make_unique<State>(directory, network);
        if (!state->directory.Locked()) {
            error = GenerationError::kLocked;
            return nullptr;
        }
        std::error_code filesystem_error;
        const auto root_status =
            std::filesystem::symlink_status(directory / kRootName, filesystem_error);
        if (filesystem_error && filesystem_error != std::errc::no_such_file_or_directory) {
            error = GenerationError::kIoFailure;
            return nullptr;
        }
        if (root_status.type() == std::filesystem::file_type::not_found) {
            // Never turn a lost committed selector into an empty database.
            for (const auto& member : std::filesystem::directory_iterator(directory)) {
                if (member.path().filename() != "generation.lock") {
                    error = GenerationError::kMalformed;
                    return nullptr;
                }
            }
            RootBytes empty{};
            state->Header(empty, kRootMagic, 0U);
            if (!state->directory.WriteNew("generation.initial", empty) ||
                !state->directory.Sync() || !state->directory.Replace("generation.initial") ||
                !state->directory.Sync()) {
                error = GenerationError::kIoFailure;
                return nullptr;
            }
        }
        auto store = std::unique_ptr<GenerationStore>{new GenerationStore{std::move(state)}};
        const auto observation = store->Observe();
        if (observation.state == GenerationState::kUnavailable) {
            error = observation.error;
            return nullptr;
        }
        error = GenerationError::kNone;
        return store;
    } catch (const std::bad_alloc&) {
        error = GenerationError::kAllocationFailure;
    } catch (...) {
        error = GenerationError::kIoFailure;
    }
    return nullptr;
}

GenerationObservation GenerationStore::Observe() const noexcept
{
    try {
        if (state_->Fail(GenerationFailurePoint::kObservation))
            return {};
        RootBytes root{};
        if (!ReadExact(state_->directory, kRootName, root))
            return {GenerationState::kUnavailable, GenerationError::kMalformed, {}};
        if (!std::equal(kRootMagic.begin(), kRootMagic.end(), root.begin()))
            return {GenerationState::kUnavailable, GenerationError::kMalformed, {}};
        if (!state_->ValidIdentity(root))
            return {GenerationState::kUnavailable, GenerationError::kIdentityMismatch, {}};
        const auto sequence = Get64(root, 8U + kIdentitySize);
        if (sequence == 0U) {
            const auto decoded = Decode(root, {}, state_->identity);
            return {decoded.state, decoded.error, {}};
        }
        DescriptorBytes descriptor{};
        if (!ReadExact(state_->directory, Name(sequence, ".descriptor"), descriptor))
            return {GenerationState::kUnavailable, GenerationError::kMalformed, {}};
        const auto decoded = Decode(root, descriptor, state_->identity);
        if (decoded.state != GenerationState::kPresent)
            return {decoded.state, decoded.error, {}};
        const auto journal_name = Name(sequence, ".journal");
        const auto wallet_name = Name(sequence, ".wallet");
        const auto journal = ProcessImage(state_->directory.Open(journal_name));
        const auto wallet = ProcessImage(state_->directory.Open(wallet_name));
        if (!journal || !wallet || journal->size != decoded.metadata.journal_size ||
            wallet->size != decoded.metadata.wallet_size ||
            journal->hash != decoded.metadata.journal_hash ||
            wallet->hash != decoded.metadata.wallet_hash)
            return {GenerationState::kUnavailable, GenerationError::kHashFailure, {}};
        return {GenerationState::kPresent,
                GenerationError::kNone,
                {sequence, state_->directory.path / journal_name,
                 state_->directory.path / wallet_name, journal->size, wallet->size, journal->hash,
                 wallet->hash}};
    } catch (const std::bad_alloc&) {
        return {GenerationState::kUnavailable, GenerationError::kAllocationFailure, {}};
    } catch (...) {
        return {};
    }
}

std::unique_ptr<GenerationStore::Prepared>
GenerationStore::Prepare(const std::filesystem::path& journal_image,
                         const std::filesystem::path& encrypted_wallet_image,
                         GenerationError& error) noexcept
{
    error = GenerationError::kIoFailure;
    try {
        const auto before = Observe();
        if (before.state == GenerationState::kUnavailable) {
            error = before.error;
            return nullptr;
        }
        auto data = std::make_unique<Prepared::Data>();
        data->owner = state_->owner;
        if (!ReadExact(state_->directory, kRootName, data->before))
            return nullptr;
        std::uint64_t sequence = before.snapshot.sequence;
        bool reserved = false;
        for (std::size_t attempt = 0;
             attempt < 1024U && sequence < std::numeric_limits<std::uint64_t>::max(); ++attempt) {
            ++sequence;
            auto reservation = state_->directory.Open(Name(sequence, ".reserve"), true);
            if (reservation.Size().has_value()) {
                reserved = true;
                break;
            }
        }
        if (!reserved) {
            error = GenerationError::kExhausted;
            return nullptr;
        }
        const auto journal =
            CopyImage(state_->directory, journal_image, Name(sequence, ".journal"));
        if (!journal || state_->Fail(GenerationFailurePoint::kAfterJournal))
            return nullptr;
        const auto wallet =
            CopyImage(state_->directory, encrypted_wallet_image, Name(sequence, ".wallet"));
        if (!wallet || state_->Fail(GenerationFailurePoint::kAfterWallet))
            return nullptr;
        DescriptorBytes descriptor{};
        state_->Header(descriptor, kDescriptorMagic, sequence);
        constexpr std::size_t journal_offset = 8U + kIdentitySize + 8U;
        constexpr std::size_t wallet_offset = journal_offset + 8U + crypto::Hash256::kSize;
        Put64(descriptor, journal_offset, journal->size);
        PutHash(descriptor, journal_offset + 8U, journal->hash);
        Put64(descriptor, wallet_offset, wallet->size);
        PutHash(descriptor, wallet_offset + 8U, wallet->hash);
        if (!state_->directory.WriteNew(Name(sequence, ".descriptor"), descriptor) ||
            state_->Fail(GenerationFailurePoint::kAfterDescriptor))
            return nullptr;
        const auto hash = crypto::Hash256::Sha256(descriptor);
        if (!hash) {
            error = GenerationError::kHashFailure;
            return nullptr;
        }
        state_->Header(data->after, kRootMagic, sequence);
        PutHash(data->after, kRootSize - crypto::Hash256::kSize, *hash);
        data->root_temporary = Name(sequence, ".root");
        if (!state_->directory.WriteNew(data->root_temporary, data->after) ||
            !state_->directory.Sync())
            return nullptr;
        error = GenerationError::kNone;
        return std::unique_ptr<Prepared>{new Prepared{std::move(data)}};
    } catch (const std::bad_alloc&) {
        error = GenerationError::kAllocationFailure;
    } catch (...) {
        error = GenerationError::kIoFailure;
    }
    return nullptr;
}

GenerationCommitResult GenerationStore::Resolve(const Prepared& prepared) const noexcept
{
    try {
        if (prepared.data_->owner.lock() != state_->owner)
            return {GenerationCommitState::kUnavailable, GenerationError::kStale};
        const auto observation = Observe();
        RootBytes current{};
        if (observation.state == GenerationState::kUnavailable ||
            !ReadExact(state_->directory, kRootName, current) || !state_->directory.Sync())
            return {};
        if (current == prepared.data_->after)
            return {GenerationCommitState::kCommitted, GenerationError::kNone};
        if (current == prepared.data_->before)
            return {GenerationCommitState::kNotCommitted, GenerationError::kNone};
        return {GenerationCommitState::kUnavailable, GenerationError::kStale};
    } catch (...) {
        return {};
    }
}

GenerationCommitResult GenerationStore::Commit(Prepared& prepared) noexcept
{
    try {
        if (prepared.data_->owner.lock() != state_->owner || prepared.data_->attempted)
            return {GenerationCommitState::kUnavailable, GenerationError::kStale};
        // Resolve verifies all currently authoritative images, not just selector
        // equality. An unreadable old state must not be overwritten as repair.
        const auto before = Resolve(prepared);
        if (before.state != GenerationCommitState::kNotCommitted)
            return before;
        prepared.data_->attempted = true;
        RootBytes temporary{};
        if (!ReadExact(state_->directory, prepared.data_->root_temporary, temporary) ||
            temporary != prepared.data_->after ||
            !CandidateMatches(state_->directory, temporary, state_->identity))
            return {GenerationCommitState::kNotCommitted, GenerationError::kHashFailure};
        if (state_->Fail(GenerationFailurePoint::kBeforeRootReplace))
            return {GenerationCommitState::kNotCommitted, GenerationError::kIoFailure};
        if (!state_->directory.Replace(prepared.data_->root_temporary))
            return Resolve(prepared);
        if (state_->Fail(GenerationFailurePoint::kAfterRootReplace) ||
            state_->Fail(GenerationFailurePoint::kDirectoryBarrier) || !state_->directory.Sync())
            return {};
        return {GenerationCommitState::kCommitted, GenerationError::kNone};
    } catch (...) {
        // Never translate an exception after replacement into final rejection.
        return {};
    }
}

void GenerationStore::FailNextForTesting(const GenerationFailurePoint point) noexcept
{
    state_->failure = point;
}
} // namespace nova::storage
