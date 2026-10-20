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
    EXPECT_EQ(mainnet.error, nova::node::NetworkSelectionError::kDisabled);
    EXPECT_EQ(mainnet.parameters, nullptr);
}

} // namespace
