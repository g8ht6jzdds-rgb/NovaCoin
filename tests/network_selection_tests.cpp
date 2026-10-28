#include <gtest/gtest.h>

#include "node/network_selection.hpp"

namespace
{

TEST(NetworkSelection, AdmitsOnlyEnabledAndValidRegtestParameters)
{
    const auto selection = nova::node::SelectNetwork(nova::consensus::NetworkId::kRegtest);
    ASSERT_EQ(selection.error, nova::node::NetworkSelectionError::kNone);
    ASSERT_NE(selection.parameters, nullptr);
    EXPECT_EQ(selection.parameters->id, nova::consensus::NetworkId::kRegtest);
    EXPECT_TRUE(selection.parameters->enabled);
}

TEST(NetworkSelection, AdmitsSeedlessTestnetButRefusesMainnet)
{
    const auto testnet = nova::node::SelectNetwork(nova::consensus::NetworkId::kTestnet);
    ASSERT_EQ(testnet.error, nova::node::NetworkSelectionError::kNone);
    ASSERT_NE(testnet.parameters, nullptr);
    EXPECT_EQ(testnet.parameters, &nova::consensus::TestnetNetworkParams());
    EXPECT_TRUE(testnet.parameters->enabled);
    EXPECT_TRUE(testnet.parameters->deployment_final);

    const auto mainnet = nova::node::SelectNetwork(nova::consensus::NetworkId::kMainnet);
    EXPECT_EQ(mainnet.error, nova::node::NetworkSelectionError::kMainnetLocked);
    EXPECT_EQ(mainnet.parameters, nullptr);
}

TEST(NetworkSelection, MainnetLockIsIndependentOfDeploymentParameterValidation)
{
    static_assert(nova::node::MAINNET_LOCKED);
    EXPECT_FALSE(nova::consensus::MainnetNetworkParams().enabled);
    EXPECT_FALSE(nova::consensus::MainnetNetworkParams().deployment_final);
    const auto mainnet = nova::node::SelectNetwork(nova::consensus::NetworkId::kMainnet);
    EXPECT_EQ(mainnet.error, nova::node::NetworkSelectionError::kMainnetLocked);
    EXPECT_EQ(mainnet.parameters, nullptr);
}

TEST(NetworkSelection, OwnerUnknownNetworkNeverAdmitsFallback)
{
    for (const auto raw : {3U, 255U}) {
        const auto selection =
            nova::node::SelectNetwork(static_cast<nova::consensus::NetworkId>(raw));
        EXPECT_NE(selection.error, nova::node::NetworkSelectionError::kNone);
        EXPECT_EQ(selection.parameters, nullptr);
    }
    EXPECT_EQ(nova::node::SelectNetwork(nova::consensus::NetworkId::kRegtest).error,
              nova::node::NetworkSelectionError::kNone);
    EXPECT_EQ(nova::node::SelectNetwork(nova::consensus::NetworkId::kTestnet).error,
              nova::node::NetworkSelectionError::kNone);
    EXPECT_EQ(nova::node::SelectNetwork(nova::consensus::NetworkId::kMainnet).parameters, nullptr);
}

TEST(NetworkSelection, UnknownEnumDomainRejectsBeforeFallbackSelection)
{
    for (std::uint32_t raw = 3U; raw <= 255U; ++raw) {
        SCOPED_TRACE(raw);
        const auto selection =
            nova::node::SelectNetwork(static_cast<nova::consensus::NetworkId>(raw));
        ASSERT_EQ(selection.error, nova::node::NetworkSelectionError::kInvalidParameters);
        EXPECT_EQ(selection.parameters, nullptr);
    }
}

} // namespace
