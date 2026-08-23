#include <gtest/gtest.h>

#include "consensus/network_params.hpp"
#include "wallet/address.hpp"

#include <string>

namespace
{

TEST(Address, RoundTripsOnlyOnItsSelectedNetwork)
{
    nova::crypto::Hash160 hash{};
    hash.fill(0x42U);
    for (const auto* network :
         {&nova::consensus::RegtestNetworkParams(), &nova::consensus::TestnetNetworkParams(),
          &nova::consensus::MainnetNetworkParams()}) {
        const auto encoded = nova::wallet::EncodeP2pkhAddress(hash, *network);
        ASSERT_TRUE(encoded.has_value());
        EXPECT_EQ(nova::wallet::DecodeP2pkhAddress(*encoded, *network), hash);
    }
}

TEST(Address, RejectsWrongNetworkChecksumAndMalformedPresentation)
{
    nova::crypto::Hash160 hash{};
    hash.fill(0x42U);
    const auto testnet =
        nova::wallet::EncodeP2pkhAddress(hash, nova::consensus::TestnetNetworkParams());
    ASSERT_TRUE(testnet.has_value());
    EXPECT_FALSE(
        nova::wallet::DecodeP2pkhAddress(*testnet, nova::consensus::RegtestNetworkParams()));
    EXPECT_FALSE(
        nova::wallet::DecodeP2pkhAddress(*testnet, nova::consensus::MainnetNetworkParams()));
    auto altered = *testnet;
    altered.back() = altered.back() == '1' ? '2' : '1';
    EXPECT_FALSE(
        nova::wallet::DecodeP2pkhAddress(altered, nova::consensus::TestnetNetworkParams()));
    EXPECT_FALSE(nova::wallet::DecodeP2pkhAddress("0OIl", nova::consensus::TestnetNetworkParams()));
    EXPECT_FALSE(nova::wallet::DecodeP2pkhAddress("1", nova::consensus::TestnetNetworkParams()));
    EXPECT_FALSE(nova::wallet::DecodeP2pkhAddress(std::string(65U, '1'),
                                                  nova::consensus::TestnetNetworkParams()));
}

} // namespace
