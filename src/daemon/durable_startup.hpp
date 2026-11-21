#pragma once

#include "daemon/durable_runtime.hpp"
#include "node/durable_startup.hpp"

namespace nova::daemon
{
// The directory must be private to this process. Existing legacy data is not
// migrated here; it must never be mistaken for a new generation store.
[[nodiscard]] inline std::unique_ptr<DurableRuntime>
OpenDurableNode(const std::filesystem::path& directory,
                const std::shared_ptr<const chain::ValidationTimeSource>& time_source,
                const std::uint32_t start_time, const std::span<const std::uint8_t> passphrase,
                rpc::RpcConfig config) noexcept
{
    auto opened =
        node::OpenDurableNodeState(directory, config.network, time_source, start_time, passphrase);
    if (!opened)
        return nullptr;
    return DurableRuntime::Create(std::move(opened->session), std::move(opened->workspace),
                                  passphrase, std::move(config),
                                  std::move(opened->directory_lease));
}

[[nodiscard]] inline std::unique_ptr<DurableRuntime>
OpenDurableRegtest(const std::filesystem::path& directory,
                   const std::shared_ptr<const chain::ValidationTimeSource>& time_source,
                   const std::uint32_t start_time, const std::span<const std::uint8_t> passphrase,
                   rpc::RpcConfig config) noexcept
{
    if (config.network != consensus::NetworkId::kRegtest)
        return nullptr;
    return OpenDurableNode(directory, time_source, start_time, passphrase, std::move(config));
}
} // namespace nova::daemon
