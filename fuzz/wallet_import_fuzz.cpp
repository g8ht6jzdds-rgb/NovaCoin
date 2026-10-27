#include "consensus/network_params.hpp"
#include "wallet/wallet.hpp"

#include <array>
#include <cstdlib>
#include <fstream>
#include <system_error>

namespace
{
std::filesystem::path input_path;
}

extern "C" int LLVMFuzzerInitialize(int*, char***)
{
    const auto* directory = std::getenv("NOVA_FUZZ_TEMP");
    if (directory == nullptr)
        std::abort();
    input_path = std::filesystem::path{directory} / "wallet-import-input.dat";
    std::error_code error;
    const auto status = std::filesystem::symlink_status(directory, error);
    if (error || !std::filesystem::is_directory(status) ||
        !std::filesystem::is_empty(directory, error) || error)
        std::abort();
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size > 4096U)
        return 0;
    {
        std::ofstream file{input_path, std::ios::binary | std::ios::trunc};
        if (size != 0U)
            file.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
        file.close();
        if (!file)
            std::abort();
    }
    // Public disposable test password; no valid wallet or secret seed is supplied.
    constexpr std::array<std::uint8_t, 4U> password{'t', 'e', 's', 't'};
    const nova::wallet::WalletParams parameters{
        nova::consensus::RegtestNetworkParams().block_limits.transaction_limits, 1, 1U};
    static_cast<void>(nova::wallet::Wallet::LoadEncrypted(parameters, input_path, password));
    if (!std::filesystem::remove(input_path))
        std::abort();
    return 0;
}
