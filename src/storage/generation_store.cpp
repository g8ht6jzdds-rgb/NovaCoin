#include "storage/generation_store.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <limits>
#include <memory>
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
constexpr std::array<std::uint8_t, 8U> kDescriptorMagicV2{'N', 'V', 'G', 'E', 'N', '0', '0', '2'};
constexpr std::size_t kIdentitySize = 1U + 4U + crypto::Hash256::kSize;
constexpr std::size_t kRootSize = 8U + kIdentitySize + 8U + crypto::Hash256::kSize;
constexpr std::size_t kDescriptorSize =
    8U + kIdentitySize + 8U + 2U * (8U + crypto::Hash256::kSize);
using RootBytes = std::array<std::uint8_t, kRootSize>;
using DescriptorBytes = std::array<std::uint8_t, kDescriptorSize>;
using DescriptorBytesV2 = std::array<std::uint8_t, kDescriptorSize + 8U + crypto::Hash256::kSize>;
constexpr const char* kRootName = "generation.current";

struct IoFault final {
    GenerationIoFault mode{GenerationIoFault::kNone};
    bool triggered{};
    bool prefix_written{};
};

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
        (void)Close();
    }
    bool Close() noexcept
    {
        if (!valid())
            return true;
        // Never retry close: on some platforms a failed close has already
        // released the descriptor. Publication callers must check this result.
        const auto closing = std::exchange(handle_, Invalid());
#ifdef _WIN32
        return CloseHandle(closing) != 0;
#else
        return close(closing) == 0;
#endif
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
    std::optional<std::size_t> WriteSome(std::span<const std::uint8_t> bytes,
                                         IoFault* fault) const noexcept
    {
        if (fault != nullptr && !fault->triggered) {
            if (fault->mode == GenerationIoFault::kZeroProgress) {
                fault->triggered = true;
                return 0U;
            }
            if (fault->mode == GenerationIoFault::kNoSpace ||
                (fault->mode == GenerationIoFault::kPrefixThenNoSpace && fault->prefix_written)) {
                fault->triggered = true;
#ifdef _WIN32
                SetLastError(ERROR_DISK_FULL);
#else
                errno = ENOSPC;
#endif
                return std::nullopt;
            }
            if (fault->mode == GenerationIoFault::kShortWrite) {
                bytes = bytes.first(std::min(bytes.size(), std::size_t{7U}));
                fault->triggered = true;
            } else if (fault->mode == GenerationIoFault::kPrefixThenNoSpace) {
                bytes = bytes.first(std::min(bytes.size(), std::size_t{1U}));
                fault->prefix_written = true;
            }
        }
#ifdef _WIN32
        const auto request = static_cast<DWORD>(std::min<std::size_t>(bytes.size(), 65536U));
        DWORD count{};
        if (!WriteFile(handle_, bytes.data(), request, &count, nullptr))
            return std::nullopt;
        return static_cast<std::size_t>(count);
#else
        ssize_t count;
        do {
            count = write(handle_, bytes.data(), bytes.size());
        } while (count < 0 && errno == EINTR);
        return count < 0 ? std::nullopt
                         : std::optional<std::size_t>{static_cast<std::size_t>(count)};
#endif
    }
    bool Write(std::span<const std::uint8_t> bytes, IoFault* fault = nullptr) const noexcept
    {
        while (!bytes.empty()) {
            const auto count = WriteSome(bytes, fault);
            if (!count || *count == 0U || *count > bytes.size())
                return false;
            bytes = bytes.subspan(*count);
        }
        return true;
    }
    bool Sync(IoFault* fault = nullptr) const noexcept
    {
        if (fault != nullptr && !fault->triggered &&
            fault->mode == GenerationIoFault::kSyncFailure) {
            fault->triggered = true;
            return false;
        }
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
    bool WriteNew(const std::string& name, const std::span<const std::uint8_t> bytes,
                  const bool simulate_close_error = false, IoFault* fault = nullptr) const
    {
        auto file = Open(name, true);
        if (!file.Size().has_value() || !file.Write(bytes, fault) || !file.Sync(fault))
            return false;
        const bool closed = file.Close();
        return closed && !simulate_close_error;
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
std::optional<Image> ProcessImage(const File& source, const File* destination = nullptr,
                                  IoFault* fault = nullptr)
{
    const auto size = source.Size();
    if (!size.has_value())
        return std::nullopt;
    using Context = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
    Context digest{EVP_MD_CTX_new(), EVP_MD_CTX_free};
    if (!digest || EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) != 1)
        return std::nullopt;
    auto buffer = std::make_unique<std::array<std::uint8_t, 65536U>>();
    std::uint64_t remaining = *size;
    while (remaining > 0U) {
        const auto wanted =
            static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer->size()));
        const auto count = source.Read(std::span{*buffer}.first(wanted));
        if (!count.has_value() || *count == 0U)
            return std::nullopt;
        const auto data = std::span{*buffer}.first(*count);
        if (EVP_DigestUpdate(digest.get(), data.data(), data.size()) != 1 ||
            (destination != nullptr && !destination->Write(data, fault)))
            return std::nullopt;
        remaining -= *count;
    }
    std::array<std::uint8_t, 1U> extra{};
    if (source.Read(extra) != 0U || source.Size() != size ||
        (destination != nullptr && (!destination->Sync(fault) || destination->Size() != size)))
        return std::nullopt;
    crypto::Hash256::Bytes hash{};
    unsigned int length{};
    if (EVP_DigestFinal_ex(digest.get(), hash.data(), &length) != 1 || length != hash.size())
        return std::nullopt;
    return Image{*size, crypto::Hash256{hash}};
}

std::optional<Image> CopyImage(const Directory& directory, const std::filesystem::path& source,
                               const std::string& name, const bool simulate_close_error = false,
                               IoFault* fault = nullptr)
{
    if (!PlainPath(source))
        return std::nullopt;
    auto input = OpenPath(source, false);
    auto output = directory.Open(name, true);
    if (!output.Size().has_value())
        return std::nullopt;
    const auto image = ProcessImage(input, &output, fault);
    const bool closed = output.Close();
    return closed && !simulate_close_error ? image : std::nullopt;
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
    const bool v2 = descriptor.size() == std::tuple_size_v<DescriptorBytesV2>;
    if (!v2 && descriptor.size() != kDescriptorSize)
        return {};
    const auto digest = crypto::Hash256::Sha256(descriptor);
    if (!digest || *digest != GetHash(root, kRootSize - crypto::Hash256::kSize))
        return {GenerationState::kUnavailable, GenerationError::kHashFailure, {}};
    const auto& magic = v2 ? kDescriptorMagicV2 : kDescriptorMagic;
    if (!std::equal(magic.begin(), magic.end(), descriptor.begin()) ||
        !std::equal(identity.begin(), identity.end(), descriptor.begin() + 8U) ||
        Get64(descriptor, 8U + kIdentitySize) != sequence)
        return {};
    constexpr std::size_t offset = 8U + kIdentitySize + 8U;
    return {GenerationState::kPresent,
            GenerationError::kNone,
            {sequence, Get64(descriptor, offset),
             Get64(descriptor, offset + 8U + crypto::Hash256::kSize),
             GetHash(descriptor, offset + 8U),
             GetHash(descriptor, offset + 16U + crypto::Hash256::kSize),
             v2 ? std::optional<std::uint64_t>{Get64(descriptor, kDescriptorSize)} : std::nullopt,
             v2 ? GetHash(descriptor, kDescriptorSize + 8U) : crypto::Hash256{}}};
}

std::size_t ReadDescriptor(const Directory& directory, const std::string& name,
                           DescriptorBytesV2& bytes)
{
    if (ReadExact(directory, name, bytes))
        return bytes.size();
    DescriptorBytes legacy{};
    if (!ReadExact(directory, name, legacy))
        return 0U;
    std::copy(legacy.begin(), legacy.end(), bytes.begin());
    return legacy.size();
}

bool CandidateMatches(const Directory& directory, const RootBytes& root,
                      const std::span<const std::uint8_t> identity)
{
    const auto sequence = Get64(root, 8U + kIdentitySize);
    DescriptorBytesV2 descriptor{};
    const auto length = ReadDescriptor(directory, Name(sequence, ".descriptor"), descriptor);
    if (length == 0U)
        return false;
    const auto decoded = Decode(root, std::span{descriptor}.first(length), identity);
    if (decoded.state != GenerationState::kPresent)
        return false;
    const auto journal = ProcessImage(directory.Open(Name(sequence, ".journal")));
    const auto wallet = ProcessImage(directory.Open(Name(sequence, ".wallet")));
    const auto mempool = decoded.metadata.mempool_size.has_value()
                             ? ProcessImage(directory.Open(Name(sequence, ".mempool")))
                             : std::optional<Image>{};
    return (!decoded.metadata.mempool_size.has_value() ||
            (mempool && mempool->size == *decoded.metadata.mempool_size &&
             mempool->hash == decoded.metadata.mempool_hash)) &&
           journal && wallet && journal->size == decoded.metadata.journal_size &&
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
    GenerationIoTarget io_target{GenerationIoTarget::kJournal};
    mutable IoFault io_fault;
    void (*failure_hook)(GenerationFailurePoint) noexcept {};

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
        if (failure_hook != nullptr)
            failure_hook(point);
        return true;
    }
    bool ValidIdentity(const std::span<const std::uint8_t> bytes) const
    {
        return std::equal(identity.begin(), identity.end(), bytes.begin() + 8U);
    }
    IoFault* FaultFor(const GenerationFailurePoint partial) const noexcept
    {
        if (io_fault.mode == GenerationIoFault::kNone)
            return nullptr;
        const bool selected = (partial == GenerationFailurePoint::kPartialJournal &&
                               io_target == GenerationIoTarget::kJournal) ||
                              (partial == GenerationFailurePoint::kPartialWallet &&
                               io_target == GenerationIoTarget::kWallet) ||
                              (partial == GenerationFailurePoint::kPartialMempool &&
                               io_target == GenerationIoTarget::kMempool) ||
                              (partial == GenerationFailurePoint::kPartialDescriptor &&
                               io_target == GenerationIoTarget::kDescriptor) ||
                              (partial == GenerationFailurePoint::kPartialRoot &&
                               io_target == GenerationIoTarget::kRoot);
        return selected ? &io_fault : nullptr;
    }
    // Test-only interruption after a real prefix write and before its flush.
    // Empty inputs exercise the zero-byte boundary. This is not an injected
    // OS error or evidence of physical power-loss durability.
    std::optional<Image> CopyCandidate(const std::filesystem::path& source, const std::string& name,
                                       const GenerationFailurePoint partial,
                                       const GenerationFailurePoint close_error) const
    {
        if (failure != partial) {
            const auto image =
                CopyImage(directory, source, name, failure == close_error, FaultFor(partial));
            (void)Fail(close_error);
            return image;
        }
        if (!PlainPath(source))
            return std::nullopt;
        auto input = OpenPath(source, false);
        auto output = directory.Open(name, true);
        std::array<std::uint8_t, 1U> prefix{};
        const auto count = input.Read(prefix);
        if (!input.Size().has_value() || !output.Size().has_value() || !count ||
            !output.Write(std::span{prefix}.first(*count)))
            return std::nullopt;
        (void)Fail(partial);
        return std::nullopt;
    }
    bool WriteCandidate(const std::string& name, const std::span<const std::uint8_t> bytes,
                        const GenerationFailurePoint partial,
                        const GenerationFailurePoint close_error) const
    {
        if (failure != partial) {
            const bool written =
                directory.WriteNew(name, bytes, failure == close_error, FaultFor(partial));
            (void)Fail(close_error);
            return written;
        }
        auto output = directory.Open(name, true);
        if (!output.Size().has_value() ||
            !output.Write(bytes.first(std::min(bytes.size(), std::size_t{1U}))))
            return false;
        (void)Fail(partial);
        return false;
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
GenerationStore::OwnerCheckpoint GenerationStore::CaptureOwnerCheckpoint() const noexcept
{
    OwnerCheckpoint result;
    result.owner_ = state_->owner;
    return result;
}

bool GenerationStore::IsOwnerCheckpointCurrent(const OwnerCheckpoint& checkpoint) const noexcept
{
    const auto owner = checkpoint.owner_.lock();
    return owner != nullptr && owner == state_->owner;
}
bool GenerationStore::MatchesNetwork(const consensus::NetworkParams& network) const noexcept
{
    if (state_->identity[0] != static_cast<std::uint8_t>(network.id))
        return false;
    for (std::size_t i = 0; i < 4U; ++i)
        if (state_->identity[1U + i] !=
            static_cast<std::uint8_t>(network.network_magic >> (8U * i)))
            return false;
    return std::equal(network.genesis_hash.bytes().begin(), network.genesis_hash.bytes().end(),
                      state_->identity.begin() + 5U);
}
GenerationStore::Prepared::Prepared(std::unique_ptr<Data> data) noexcept : data_(std::move(data)) {}
GenerationStore::Prepared::~Prepared() = default;

std::unique_ptr<GenerationStore> GenerationStore::Open(const std::filesystem::path& directory,
                                                       const consensus::NetworkParams& network,
                                                       GenerationError& error) noexcept
{
    return OpenImpl(directory, network, error, GenerationFailurePoint::kNone,
                    GenerationIoFault::kNone, nullptr);
}

std::unique_ptr<GenerationStore>
GenerationStore::OpenForTesting(const std::filesystem::path& directory,
                                const consensus::NetworkParams& network, GenerationError& error,
                                const GenerationFailurePoint point, const GenerationIoFault fault,
                                void (*hook)(GenerationFailurePoint) noexcept) noexcept
{
    return OpenImpl(directory, network, error, point, fault, hook);
}

std::unique_ptr<GenerationStore>
GenerationStore::OpenImpl(const std::filesystem::path& directory,
                          const consensus::NetworkParams& network, GenerationError& error,
                          const GenerationFailurePoint point, const GenerationIoFault fault,
                          void (*hook)(GenerationFailurePoint) noexcept) noexcept
{
    error = GenerationError::kInvalidPath;
    try {
        if (!PlainPath(directory) || !std::filesystem::is_directory(directory) ||
            consensus::CheckNetworkParams(network) != consensus::NetworkParamsError::kNone ||
            !network.enabled)
            return nullptr;
        auto state = std::make_unique<State>(directory, network);
        state->failure = point;
        state->failure_hook = hook;
        state->io_target = GenerationIoTarget::kRoot;
        state->io_fault = IoFault{fault, false, false};
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
            if (!state->WriteCandidate("generation.initial", empty,
                                       GenerationFailurePoint::kPartialRoot,
                                       GenerationFailurePoint::kRootClose) ||
                state->Fail(GenerationFailurePoint::kPrepareDirectoryBarrier) ||
                !state->directory.Sync() ||
                state->Fail(GenerationFailurePoint::kBeforeRootReplace) ||
                !state->directory.Replace("generation.initial") ||
                state->Fail(GenerationFailurePoint::kAfterRootReplace) ||
                state->Fail(GenerationFailurePoint::kDirectoryBarrier) ||
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
        // A verified selector alone does not establish completion of a prior
        // interrupted publication barrier. Refuse admission if it cannot be
        // completed; never rewrite or fall back to an older generation here.
        if (store->state_->Fail(GenerationFailurePoint::kRecoveryDirectoryBarrier) ||
            !store->state_->directory.Sync()) {
            error = GenerationError::kIoFailure;
            return nullptr;
        }
        store->state_->failure = GenerationFailurePoint::kNone;
        store->state_->failure_hook = nullptr;
        store->state_->io_fault = {};
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
        DescriptorBytesV2 descriptor{};
        const auto length =
            ReadDescriptor(state_->directory, Name(sequence, ".descriptor"), descriptor);
        if (length == 0U)
            return {GenerationState::kUnavailable, GenerationError::kMalformed, {}};
        const auto decoded = Decode(root, std::span{descriptor}.first(length), state_->identity);
        if (decoded.state != GenerationState::kPresent)
            return {decoded.state, decoded.error, {}};
        const auto journal_name = Name(sequence, ".journal");
        const auto wallet_name = Name(sequence, ".wallet");
        const auto journal = ProcessImage(state_->directory.Open(journal_name));
        const auto wallet = ProcessImage(state_->directory.Open(wallet_name));
        const auto mempool_name = Name(sequence, ".mempool");
        const auto mempool = decoded.metadata.mempool_size.has_value()
                                 ? ProcessImage(state_->directory.Open(mempool_name))
                                 : std::optional<Image>{};
        if (!journal || !wallet || journal->size != decoded.metadata.journal_size ||
            wallet->size != decoded.metadata.wallet_size ||
            journal->hash != decoded.metadata.journal_hash ||
            wallet->hash != decoded.metadata.wallet_hash ||
            (decoded.metadata.mempool_size.has_value() &&
             (!mempool || mempool->size != *decoded.metadata.mempool_size ||
              mempool->hash != decoded.metadata.mempool_hash)))
            return {GenerationState::kUnavailable, GenerationError::kHashFailure, {}};
        return {
            GenerationState::kPresent,
            GenerationError::kNone,
            {sequence, state_->directory.path / journal_name, state_->directory.path / wallet_name,
             journal->size, wallet->size, journal->hash, wallet->hash,
             mempool ? std::optional<std::filesystem::path>{state_->directory.path / mempool_name}
                     : std::nullopt,
             decoded.metadata.mempool_size, decoded.metadata.mempool_hash}};
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
    return PrepareImages(journal_image, encrypted_wallet_image, nullptr, error);
}

std::unique_ptr<GenerationStore::Prepared> GenerationStore::Prepare(
    const std::filesystem::path& journal_image, const std::filesystem::path& encrypted_wallet_image,
    const std::filesystem::path& mempool_image, GenerationError& error) noexcept
{
    return PrepareImages(journal_image, encrypted_wallet_image, &mempool_image, error);
}

std::unique_ptr<GenerationStore::Prepared> GenerationStore::PrepareImages(
    const std::filesystem::path& journal_image, const std::filesystem::path& encrypted_wallet_image,
    const std::filesystem::path* mempool_image, GenerationError& error) noexcept
{
    error = GenerationError::kIoFailure;
    try {
        const auto before = Observe();
        if (before.state == GenerationState::kUnavailable) {
            error = before.error;
            return nullptr;
        }
        if (before.snapshot.mempool.has_value() && mempool_image == nullptr) {
            error = GenerationError::kMalformed;
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
        if (state_->Fail(GenerationFailurePoint::kBeforeJournal))
            return nullptr;
        const auto journal = state_->CopyCandidate(journal_image, Name(sequence, ".journal"),
                                                   GenerationFailurePoint::kPartialJournal,
                                                   GenerationFailurePoint::kJournalClose);
        if (!journal || state_->Fail(GenerationFailurePoint::kAfterJournal))
            return nullptr;
        if (state_->Fail(GenerationFailurePoint::kBeforeWallet))
            return nullptr;
        const auto wallet = state_->CopyCandidate(encrypted_wallet_image, Name(sequence, ".wallet"),
                                                  GenerationFailurePoint::kPartialWallet,
                                                  GenerationFailurePoint::kWalletClose);
        if (!wallet || state_->Fail(GenerationFailurePoint::kAfterWallet))
            return nullptr;
        std::optional<Image> mempool;
        if (mempool_image != nullptr) {
            if (state_->Fail(GenerationFailurePoint::kBeforeMempool))
                return nullptr;
            mempool = state_->CopyCandidate(*mempool_image, Name(sequence, ".mempool"),
                                            GenerationFailurePoint::kPartialMempool,
                                            GenerationFailurePoint::kMempoolClose);
            if (!mempool || state_->Fail(GenerationFailurePoint::kAfterMempool))
                return nullptr;
        }
        DescriptorBytesV2 descriptor{};
        state_->Header(descriptor, mempool ? kDescriptorMagicV2 : kDescriptorMagic, sequence);
        constexpr std::size_t journal_offset = 8U + kIdentitySize + 8U;
        constexpr std::size_t wallet_offset = journal_offset + 8U + crypto::Hash256::kSize;
        Put64(descriptor, journal_offset, journal->size);
        PutHash(descriptor, journal_offset + 8U, journal->hash);
        Put64(descriptor, wallet_offset, wallet->size);
        PutHash(descriptor, wallet_offset + 8U, wallet->hash);
        if (mempool) {
            Put64(descriptor, kDescriptorSize, mempool->size);
            PutHash(descriptor, kDescriptorSize + 8U, mempool->hash);
        }
        const auto descriptor_bytes =
            std::span{descriptor}.first(mempool ? descriptor.size() : kDescriptorSize);
        if (state_->Fail(GenerationFailurePoint::kBeforeDescriptor))
            return nullptr;
        if (!state_->WriteCandidate(Name(sequence, ".descriptor"), descriptor_bytes,
                                    GenerationFailurePoint::kPartialDescriptor,
                                    GenerationFailurePoint::kDescriptorClose) ||
            state_->Fail(GenerationFailurePoint::kAfterDescriptor))
            return nullptr;
        const auto hash = crypto::Hash256::Sha256(descriptor_bytes);
        if (!hash) {
            error = GenerationError::kHashFailure;
            return nullptr;
        }
        state_->Header(data->after, kRootMagic, sequence);
        PutHash(data->after, kRootSize - crypto::Hash256::kSize, *hash);
        data->root_temporary = Name(sequence, ".root");
        if (!state_->WriteCandidate(data->root_temporary, data->after,
                                    GenerationFailurePoint::kPartialRoot,
                                    GenerationFailurePoint::kRootClose) ||
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

GenerationObservation GenerationStore::InspectPrepared(const Prepared& prepared) const noexcept
{
    try {
        if (prepared.data_->owner.lock() != state_->owner)
            return {GenerationState::kUnavailable, GenerationError::kStale, {}};
        const auto& root = prepared.data_->after;
        if (!CandidateMatches(state_->directory, root, state_->identity))
            return {GenerationState::kUnavailable, GenerationError::kHashFailure, {}};
        const auto sequence = Get64(root, 8U + kIdentitySize);
        DescriptorBytesV2 descriptor{};
        const auto length =
            ReadDescriptor(state_->directory, Name(sequence, ".descriptor"), descriptor);
        if (length == 0U)
            return {GenerationState::kUnavailable, GenerationError::kMalformed, {}};
        const auto decoded = Decode(root, std::span{descriptor}.first(length), state_->identity);
        if (decoded.state != GenerationState::kPresent)
            return {decoded.state, decoded.error, {}};
        return {GenerationState::kPresent,
                GenerationError::kNone,
                {sequence, state_->directory.path / Name(sequence, ".journal"),
                 state_->directory.path / Name(sequence, ".wallet"), decoded.metadata.journal_size,
                 decoded.metadata.wallet_size, decoded.metadata.journal_hash,
                 decoded.metadata.wallet_hash,
                 decoded.metadata.mempool_size.has_value()
                     ? std::optional<std::filesystem::path>{state_->directory.path /
                                                            Name(sequence, ".mempool")}
                     : std::nullopt,
                 decoded.metadata.mempool_size, decoded.metadata.mempool_hash}};
    } catch (...) {
        return {};
    }
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

void GenerationStore::SetIoFaultForTesting(const GenerationIoTarget target,
                                           const GenerationIoFault fault) noexcept
{
    state_->io_target = target;
    state_->io_fault = IoFault{fault, false, false};
}

bool GenerationStore::IoFaultTriggeredForTesting() const noexcept
{
    return state_->io_fault.triggered;
}

void GenerationStore::SetFailureHookForTesting(
    void (*hook)(GenerationFailurePoint) noexcept) noexcept
{
    state_->failure_hook = hook;
}
} // namespace nova::storage
