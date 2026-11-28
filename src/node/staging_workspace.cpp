#include "node/staging_workspace.hpp"

#include <array>
#include <cstdio>
#include <limits>
#include <string>
#include <system_error>

namespace nova::node
{
std::unique_ptr<StagingWorkspace>
StagingWorkspace::Open(const std::filesystem::path& existing_private_directory) noexcept
{
    try {
        std::error_code error;
        const auto status = std::filesystem::symlink_status(existing_private_directory, error);
        if (error || !std::filesystem::is_directory(status) || std::filesystem::is_symlink(status))
            return nullptr;
        const auto root = std::filesystem::canonical(existing_private_directory, error);
        if (error)
            return nullptr;
        return std::unique_ptr<StagingWorkspace>{new StagingWorkspace{root}};
    } catch (...) {
        return nullptr;
    }
}

std::optional<std::filesystem::path> StagingWorkspace::Next() noexcept
{
    if (issued_)
        return std::nullopt;
    try {
        std::error_code root_error;
        const auto root_status = std::filesystem::symlink_status(root_, root_error);
        if (root_error || !std::filesystem::is_directory(root_status) ||
            std::filesystem::is_symlink(root_status) ||
            std::filesystem::canonical(root_, root_error) != root_ || root_error)
            return std::nullopt;
        for (std::size_t attempts = 0U; attempts < 4096U; ++attempts) {
            if (next_ == std::numeric_limits<std::uint64_t>::max())
                return std::nullopt;
            auto candidate = root_ / ("stage-" + std::to_string(next_++));
            std::error_code error;
            const auto status = std::filesystem::symlink_status(candidate, error);
            if (error && error != std::errc::no_such_file_or_directory)
                return std::nullopt;
            if (!std::filesystem::exists(status)) {
                issued_ = candidate;
                return candidate;
            }
        }
    } catch (...) {
    }
    return std::nullopt;
}

bool StagingWorkspace::Cleanup(const std::filesystem::path& issued_path) noexcept
{
    const auto failure = [](unsigned stage, const std::error_code& error = {}) noexcept {
        // No paths, wallet material or peer-supplied strings enter diagnostics.
        std::fprintf(stderr, "durable staging cleanup failed: stage=%u system_error=%d\n", stage,
                     error.value());
        return false;
    };
    if (!issued_ || issued_path != *issued_ || issued_path.parent_path() != root_)
        return failure(1U);
    try {
        std::error_code error;
        const auto root_status = std::filesystem::symlink_status(root_, error);
        if (error || !std::filesystem::is_directory(root_status) ||
            std::filesystem::is_symlink(root_status) ||
            std::filesystem::canonical(root_, error) != root_ || error)
            return failure(2U, error);
        const auto status = std::filesystem::symlink_status(issued_path, error);
        if (error == std::errc::no_such_file_or_directory ||
            (!error && !std::filesystem::exists(status))) {
            issued_.reset();
            return true;
        }
        if (error || !std::filesystem::is_directory(status) ||
            std::filesystem::is_symlink(status) ||
            std::filesystem::canonical(issued_path, error).parent_path() != root_ || error)
            return failure(3U, error);
        constexpr std::array allowed{"blocks.dat", "wallet.dat", "wallet.dat.tmp", "mempool.dat"};
        for (std::filesystem::directory_iterator it{issued_path, error}, end; !error && it != end;
             it.increment(error)) {
            const auto name = it->path().filename().string();
            bool known = false;
            for (const auto* permitted : allowed)
                known = known || name == permitted;
            const auto child = it->symlink_status(error);
            if (error || !known || !std::filesystem::is_regular_file(child) ||
                std::filesystem::is_symlink(child))
                return failure(4U, error);
        }
        if (error)
            return failure(5U, error);
        for (const auto* name : allowed) {
            const auto child = issued_path / name;
            if (!std::filesystem::remove(child, error) && error &&
                error != std::errc::no_such_file_or_directory)
                return failure(6U, error);
            error.clear();
        }
        if (!std::filesystem::remove(issued_path, error) || error)
            return failure(7U, error);
        issued_.reset();
        return true;
    } catch (...) {
        return failure(8U);
    }
}
} // namespace nova::node
