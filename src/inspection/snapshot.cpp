#include "inspection/snapshot.hpp"

#include "inspection/chain_report.hpp"
#include "storage/network_identity.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <dirent.h>
#include <fcntl.h>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace nova::inspection
{
namespace
{
class Descriptor final
{
  public:
    explicit Descriptor(int value = -1) noexcept : value_(value) {}
    ~Descriptor()
    {
        if (value_ >= 0)
            static_cast<void>(close(value_));
    }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    Descriptor(Descriptor&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
    Descriptor& operator=(Descriptor&& other) noexcept
    {
        if (this != &other) {
            if (value_ >= 0)
                static_cast<void>(close(value_));
            value_ = std::exchange(other.value_, -1);
        }
        return *this;
    }
    int get() const noexcept
    {
        return value_;
    }

  private:
    int value_;
};

template <class T> bool Number(const std::string_view text, T& value)
{
    if (text.empty() || (text.size() > 1U && text.front() == '0'))
        return false;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

Descriptor Directory(const std::string_view path)
{
    if (path.empty() || path.front() != '/')
        return Descriptor{};
    Descriptor current{open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    std::size_t start = 1U;
    while (start < path.size()) {
        const auto end = path.find('/', start);
        const auto component =
            path.substr(start, end == std::string_view::npos ? path.size() - start : end - start);
        if (component.empty() || component == "." || component == "..")
            return Descriptor{};
        const std::string name{component};
        current = Descriptor{
            openat(current.get(), name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
        if (current.get() < 0)
            return current;
        if (end == std::string_view::npos)
            break;
        start = end + 1U;
    }
    return current;
}

bool SameIdentity(const struct stat& a, const struct stat& b)
{
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}

bool Frozen(const int descriptor)
{
    const auto seals = fcntl(descriptor, F_GET_SEALS);
    constexpr int required = F_SEAL_SEAL | F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK;
    if (seals >= 0 && (seals & required) == required)
        return true;
    struct statvfs vfs{};
    return fstatvfs(descriptor, &vfs) == 0 && (vfs.f_flag & ST_RDONLY) != 0U;
}

std::optional<std::vector<std::uint8_t>> ReadFrozen(const int descriptor, const std::size_t maximum)
{
    struct stat before{};
    if (fstat(descriptor, &before) != 0 || !S_ISREG(before.st_mode) || before.st_size < 0 ||
        static_cast<std::uint64_t>(before.st_size) > maximum || before.st_nlink > 1U ||
        !Frozen(descriptor))
        return std::nullopt;
    std::vector<std::uint8_t> bytes;
    std::array<std::uint8_t, 4096U> buffer{};
    while (true) {
        const auto remaining = maximum - bytes.size();
        const auto request = std::min(buffer.size(), remaining + 1U);
        const auto got =
            pread(descriptor, buffer.data(), request, static_cast<off_t>(bytes.size()));
        if (got < 0) {
            if (errno == EINTR)
                continue;
            return std::nullopt;
        }
        if (got == 0)
            break;
        const auto size = static_cast<std::size_t>(got);
        if (size > remaining)
            return std::nullopt;
        bytes.insert(bytes.end(), buffer.begin(),
                     buffer.begin() + static_cast<std::ptrdiff_t>(size));
    }
    struct stat after{};
    if (fstat(descriptor, &after) != 0 || !SameIdentity(before, after) ||
        before.st_size != after.st_size ||
        bytes.size() != static_cast<std::uint64_t>(before.st_size) || !Frozen(descriptor))
        return std::nullopt;
    return bytes;
}

std::string Digest(const std::span<const std::uint8_t> bytes)
{
    const auto hash = crypto::Hash256::Sha256(bytes);
    if (!hash.has_value())
        return {};
    return Hex(hash->bytes());
}

std::optional<InspectionContext> Context(const std::vector<std::uint8_t>& metadata,
                                         const std::vector<std::uint8_t>& identity,
                                         const std::vector<std::uint8_t>& journal,
                                         const std::string& binary_hash)
{
    const std::string text(metadata.begin(), metadata.end());
    if (text.empty() || text.back() != '\n' || text.find('\r') != std::string::npos ||
        text.find('\0') != std::string::npos)
        return std::nullopt;
    std::istringstream input{text};
    std::array<std::string, 10U> lines;
    for (auto& line : lines)
        if (!std::getline(input, line))
            return std::nullopt;
    if (input.peek() != EOF || lines[0] != "nova-snapshot-v1" ||
        lines[9] != "capture=stopped-immutable-copy")
        return std::nullopt;
    constexpr std::array<std::string_view, 8U> names{"network=",
                                                     "as_of=",
                                                     "tip=",
                                                     "height=",
                                                     "work=",
                                                     "identity_sha256=",
                                                     "journal_sha256=",
                                                     "provenance_sha256="};
    std::array<std::string_view, 8U> values;
    for (std::size_t i = 0U; i < names.size(); ++i) {
        if (!lines[i + 1U].starts_with(names[i]))
            return std::nullopt;
        values[i] = std::string_view{lines[i + 1U]}.substr(names[i].size());
    }
    InspectionContext context;
    if (values[0] == "testnet")
        context.network = consensus::NetworkId::kTestnet;
    else if (values[0] != "regtest")
        return std::nullopt;
    const auto tip = crypto::Hash256::FromHex(values[2]);
    if (!Number(values[1], context.as_of) || !Number(values[3], context.expected_height) ||
        !tip.has_value() || values[4].size() != 128U)
        return std::nullopt;
    chain::ChainWork::Bytes work{};
    for (std::size_t i = 0U; i < work.size(); ++i) {
        const auto pair = values[4].substr(2U * i, 2U);
        unsigned value = 0U;
        const auto parsed = std::from_chars(pair.data(), pair.data() + 2U, value, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != pair.data() + 2U)
            return std::nullopt;
        work[i] = static_cast<std::uint8_t>(value);
    }
    context.expected_work = chain::ChainWork{work};
    context.expected_tip = *tip;
    context.source_revision = NOVA_INSPECT_SOURCE_REVISION;
    context.source_tree = NOVA_INSPECT_SOURCE_TREE;
    context.provenance_hash = values[7];
    context.identity_hash = Digest(identity);
    context.journal_hash = Digest(journal);
    context.metadata_hash = Digest(metadata);
    context.binary_hash = binary_hash;
    context.identity_bytes = identity.size();
    context.metadata_bytes = metadata.size();
    if (values[5] != context.identity_hash || values[6] != context.journal_hash ||
        storage::ValidateNetworkIdentity(identity, consensus::GetNetworkParams(context.network)) !=
            storage::NetworkIdentityError::kNone)
        return std::nullopt;
    return context;
}

bool DirectoryInventory(const int descriptor)
{
    auto* raw = fdopendir(openat(descriptor, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (raw == nullptr)
        return false;
    std::unique_ptr<DIR, decltype(&closedir)> directory{raw, &closedir};
    std::size_t count = 0U;
    errno = 0;
    while (const auto* entry = readdir(directory.get())) {
        const std::string_view name{entry->d_name};
        if (name == "." || name == "..")
            continue;
        if (name != "network.identity" && name != "blocks.dat" && name != "snapshot.meta")
            return false;
        ++count;
    }
    return errno == 0 && count == 3U;
}

bool IndependentOutput(const int output, const int source)
{
    if (source < 0)
        return true; // sealed memfds cannot alias disk directories
    struct stat source_stat{};
    if (fstat(source, &source_stat) != 0)
        return false;
    Descriptor ancestor{dup(output)};
    for (std::size_t i = 0U; i < 1024U; ++i) {
        struct stat current{}, parent_stat{};
        if (fstat(ancestor.get(), &current) != 0 || SameIdentity(source_stat, current))
            return false;
        Descriptor parent{
            openat(ancestor.get(), "..", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
        if (fstat(parent.get(), &parent_stat) != 0)
            return false;
        if (SameIdentity(current, parent_stat))
            return true;
        ancestor = std::move(parent);
    }
    return false;
}
} // namespace

int RunSnapshotInspection(const int argc, char** argv) noexcept
{
    try {
        if (argc != 4 && argc != 6) {
            std::cerr << "INCOMPLETE usage: --snapshot ABS_DIR ABS_OUTPUT_DIR | --sealed-fds ID "
                         "JOURNAL META ABS_OUTPUT_DIR\n";
            return 2;
        }
        const bool sealed = argc == 6 && std::string_view{argv[1]} == "--sealed-fds";
        if (!sealed && (argc != 4 || std::string_view{argv[1]} != "--snapshot"))
            return 2;
        // Hard process limits are operational, never consensus parameters.
        constexpr rlim_t gib = 1024ULL * 1024ULL * 1024ULL;
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
        constexpr rlim_t address_limit = 64ULL * 1024ULL * gib;
#else
        constexpr rlim_t address_limit = 2ULL * gib;
#endif
#else
        constexpr rlim_t address_limit = 2ULL * gib;
#endif
        const struct rlimit address{address_limit, address_limit}, cpu{120U, 120U};
        if (setrlimit(RLIMIT_AS, &address) != 0 || setrlimit(RLIMIT_CPU, &cpu) != 0)
            return 2;
        static_cast<void>(alarm(120U));
        Descriptor source;
        std::array<Descriptor, 3U> files;
        if (sealed) {
            for (std::size_t i = 0U; i < files.size(); ++i) {
                int value = -1;
                if (!Number(std::string_view{argv[i + 2U]}, value) || value < 3)
                    return 2;
                constexpr int required = F_SEAL_SEAL | F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK;
                const auto seals = fcntl(value, F_GET_SEALS);
                if (seals < 0 || (seals & required) != required)
                    return 2;
                files[i] = Descriptor{dup(value)};
            }
        } else {
            source = Directory(argv[2]);
            if (source.get() < 0 || !DirectoryInventory(source.get()))
                return 2;
            constexpr std::array<const char*, 3U> names{"network.identity", "blocks.dat",
                                                        "snapshot.meta"};
            for (std::size_t i = 0U; i < files.size(); ++i)
                files[i] = Descriptor{
                    openat(source.get(), names[i], O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC)};
        }
        auto output = Directory(argv[argc - 1]);
        if (output.get() < 0 || !IndependentOutput(output.get(), source.get()) ||
            (source.get() >= 0 && !IndependentOutput(source.get(), output.get())))
            return 2;
        const auto identity = ReadFrozen(files[0].get(), 45U);
        const auto journal = ReadFrozen(files[1].get(), 64U * 1024U * 1024U);
        const auto metadata = ReadFrozen(files[2].get(), 2048U);
        if (!identity.has_value() || !journal.has_value() || !metadata.has_value()) {
            std::cerr << "INCOMPLETE input_not_frozen_regular_bounded\n";
            return 2;
        }
        // Hash our executable through a read-only descriptor. It is build
        // provenance, not an input snapshot; no frozen-filesystem requirement.
        Descriptor executable{open("/proc/self/exe", O_RDONLY | O_CLOEXEC)};
        struct stat executable_stat{};
        if (fstat(executable.get(), &executable_stat) != 0 || executable_stat.st_size <= 0 ||
            executable_stat.st_size > 128 * 1024 * 1024)
            return 2;
        std::vector<std::uint8_t> binary(static_cast<std::size_t>(executable_stat.st_size));
        std::size_t position = 0U;
        while (position < binary.size()) {
            const auto got = pread(executable.get(), binary.data() + position,
                                   binary.size() - position, static_cast<off_t>(position));
            if (got <= 0)
                return 2;
            position += static_cast<std::size_t>(got);
        }
        const auto context = Context(*metadata, *identity, *journal, Digest(binary));
        if (!context.has_value()) {
            std::cerr << "INCOMPLETE metadata_identity_or_build_context\n";
            return 2;
        }
        std::istringstream stream{std::string(journal->begin(), journal->end())};
        const auto result = InspectJournal(stream, *context);
        if (!result.error.empty()) {
            std::cerr << "INCOMPLETE " << result.error << '\n';
            return 2;
        }
        if (ReadFrozen(files[0].get(), 45U) != identity ||
            ReadFrozen(files[1].get(), 64U * 1024U * 1024U) != journal ||
            ReadFrozen(files[2].get(), 2048U) != metadata ||
            (source.get() >= 0 && !DirectoryInventory(source.get())))
            return 2;
        // A final report is created once, atomically, without overwriting any
        // existing output. Only this process's exclusive temporary name is removed.
        const auto temporary = ".nova-inspection-" + std::to_string(getpid());
        Descriptor report{openat(output.get(), temporary.c_str(),
                                 O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600)};
        if (report.get() < 0)
            return 2;
        bool good = true;
        position = 0U;
        while (position < result.report.size()) {
            const auto got = write(report.get(), result.report.data() + position,
                                   result.report.size() - position);
            if (got <= 0) {
                good = false;
                break;
            }
            position += static_cast<std::size_t>(got);
        }
        good = good && fsync(report.get()) == 0;
        if (good)
            good = linkat(output.get(), temporary.c_str(), output.get(), "report.json", 0) == 0;
        static_cast<void>(unlinkat(output.get(), temporary.c_str(), 0));
        if (!good) {
            std::cerr << "INCOMPLETE output\n";
            return 2;
        }
        static_cast<void>(fsync(output.get()));
        return 0;
    } catch (...) {
        std::cerr << "INCOMPLETE allocation_or_io\n";
        return 2;
    }
}
} // namespace nova::inspection
