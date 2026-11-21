#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <utility>

namespace nova::node
{
// A caller-owned, private directory under exclusive, non-reentrant use.
// Cleanup is limited to known staging files in a path issued by this instance.
// Leftovers from an earlier process are preserved and never adopted.
class StagingWorkspace final
{
  public:
    [[nodiscard]] static std::unique_ptr<StagingWorkspace>
    Open(const std::filesystem::path& existing_private_directory) noexcept;
    [[nodiscard]] std::optional<std::filesystem::path> Next() noexcept;
    [[nodiscard]] bool Cleanup(const std::filesystem::path& issued_path) noexcept;

  private:
    explicit StagingWorkspace(std::filesystem::path root) noexcept : root_(std::move(root)) {}
    std::filesystem::path root_;
    std::optional<std::filesystem::path> issued_;
    std::uint64_t next_{};
};
} // namespace nova::node
