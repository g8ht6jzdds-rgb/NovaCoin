#include <gtest/gtest.h>

#include "consensus/network_params.hpp"
#include "node/bootstrap_config.hpp"

#include <filesystem>
#include <fstream>
#include <system_error>

namespace
{

class BootstrapConfigTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        path_ = std::filesystem::temp_directory_path() / "novacoin-bootstrap-test.conf";
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

    void TearDown() override
    {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

    void Write(const std::string_view content)
    {
        std::ofstream output{path_, std::ios::binary | std::ios::trunc};
        ASSERT_TRUE(output.is_open());
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
    }

    std::filesystem::path path_;
};

TEST_F(BootstrapConfigTest, AcceptsCanonicalManifestBoundToRegtestIdentity)
{
    Write("version=1\nnetwork=regtest\nmagic=dab5bffa\ngenesis="
          "c974d11a5276ca7eb69b1ec0a8062fb47b49c02f5ac726532483d090ac53eb79\nseed=seed-one.example:"
          "18444\nseed=seed-two.example:18444\n");
    const auto loaded =
        nova::node::LoadStaticBootstrapConfig(path_, nova::consensus::RegtestNetworkParams());
    ASSERT_EQ(loaded.error, nova::node::BootstrapConfigError::kNone);
    ASSERT_EQ(loaded.seeds.size(), 2U);
    EXPECT_EQ(loaded.seeds.front().host, "seed-one.example");
    EXPECT_EQ(loaded.seeds.front().port, 18'444U);
}

TEST_F(BootstrapConfigTest, AcceptsBoundedCanonicalDnsSeedsOnlyWhenExplicitlyRequestedByDaemon)
{
    Write("version=1\nnetwork=testnet\nmagic=dab5bffb\ngenesis="
          "25f944a00f3d559452b95653a20a039322ab3243a577d1cc3b8f48e4f30fd048\n"
          "dnsseed=bootstrap-one.novacoin.test\n"
          "dnsseed=bootstrap-two.novacoin.test\n");
    const auto loaded =
        nova::node::LoadStaticBootstrapConfig(path_, nova::consensus::TestnetNetworkParams());
    ASSERT_EQ(loaded.error, nova::node::BootstrapConfigError::kNone);
    EXPECT_TRUE(loaded.seeds.empty());
    ASSERT_EQ(loaded.dns_seeds.size(), 2U);
    EXPECT_EQ(loaded.dns_seeds[0], "bootstrap-one.novacoin.test");
    EXPECT_EQ(loaded.dns_seeds[1], "bootstrap-two.novacoin.test");
}

TEST_F(BootstrapConfigTest, AcceptsCanonicalHeaderOnlyTestnetConfiguration)
{
    Write("version=1\nnetwork=testnet\nmagic=dab5bffb\ngenesis="
          "25f944a00f3d559452b95653a20a039322ab3243a577d1cc3b8f48e4f30fd048\n");
    const auto loaded =
        nova::node::LoadStaticBootstrapConfig(path_, nova::consensus::TestnetNetworkParams());
    ASSERT_EQ(loaded.error, nova::node::BootstrapConfigError::kNone);
    EXPECT_TRUE(loaded.seeds.empty());
    EXPECT_TRUE(loaded.dns_seeds.empty());
}

TEST_F(BootstrapConfigTest, RejectsCanonicalHeaderOnlyConfigurationForOtherNetworks)
{
    Write("version=1\nnetwork=regtest\nmagic=dab5bffa\ngenesis="
          "c974d11a5276ca7eb69b1ec0a8062fb47b49c02f5ac726532483d090ac53eb79\n");
    EXPECT_EQ(
        nova::node::LoadStaticBootstrapConfig(path_, nova::consensus::RegtestNetworkParams()).error,
        nova::node::BootstrapConfigError::kNonCanonical);
}

TEST_F(BootstrapConfigTest, RejectsWrongNetworkGenesisAndMagic)
{
    Write("version=1\nnetwork=testnet\nmagic=dab5bffa\ngenesis="
          "c974d11a5276ca7eb69b1ec0a8062fb47b49c02f5ac726532483d090ac53eb79\nseed=seed.example:"
          "18444\n");
    EXPECT_EQ(
        nova::node::LoadStaticBootstrapConfig(path_, nova::consensus::RegtestNetworkParams()).error,
        nova::node::BootstrapConfigError::kNetworkMismatch);
}

TEST_F(BootstrapConfigTest, RejectsNoncanonicalAndHostileInputs)
{
    Write("version=1\r\nnetwork=regtest\r\nmagic=dab5bffa\r\ngenesis="
          "c974d11a5276ca7eb69b1ec0a8062fb47b49c02f5ac726532483d090ac53eb79\r\nseed=seed.example:"
          "18444\r\n");
    EXPECT_EQ(
        nova::node::LoadStaticBootstrapConfig(path_, nova::consensus::RegtestNetworkParams()).error,
        nova::node::BootstrapConfigError::kNonCanonical);
    Write(
        "version=1\nnetwork=regtest\nmagic=dab5bffa\ngenesis="
        "c974d11a5276ca7eb69b1ec0a8062fb47b49c02f5ac726532483d090ac53eb79\nseed=bad host:18444\n");
    EXPECT_EQ(
        nova::node::LoadStaticBootstrapConfig(path_, nova::consensus::RegtestNetworkParams()).error,
        nova::node::BootstrapConfigError::kInvalidEndpoint);
}

TEST_F(BootstrapConfigTest, RejectsMalformedDuplicateAndExcessDnsSeeds)
{
    Write("version=1\nnetwork=testnet\nmagic=dab5bffb\ngenesis="
          "25f944a00f3d559452b95653a20a039322ab3243a577d1cc3b8f48e4f30fd048\n"
          "dnsseed=-bad.novacoin.test\n");
    EXPECT_EQ(
        nova::node::LoadStaticBootstrapConfig(path_, nova::consensus::TestnetNetworkParams()).error,
        nova::node::BootstrapConfigError::kInvalidEndpoint);

    Write("version=1\nnetwork=testnet\nmagic=dab5bffb\ngenesis="
          "25f944a00f3d559452b95653a20a039322ab3243a577d1cc3b8f48e4f30fd048\n"
          "dnsseed=one.novacoin.test\n"
          "dnsseed=one.novacoin.test\n");
    EXPECT_EQ(
        nova::node::LoadStaticBootstrapConfig(path_, nova::consensus::TestnetNetworkParams()).error,
        nova::node::BootstrapConfigError::kNonCanonical);

    Write("version=1\nnetwork=testnet\nmagic=dab5bffb\ngenesis="
          "25f944a00f3d559452b95653a20a039322ab3243a577d1cc3b8f48e4f30fd048\n"
          "dnsseed=one.novacoin.test\n"
          "dnsseed=two.novacoin.test\n"
          "dnsseed=three.novacoin.test\n"
          "dnsseed=four.novacoin.test\n"
          "dnsseed=five.novacoin.test\n");
    EXPECT_EQ(
        nova::node::LoadStaticBootstrapConfig(path_, nova::consensus::TestnetNetworkParams()).error,
        nova::node::BootstrapConfigError::kTooManyDnsSeeds);
}

TEST(BootstrapConfig, CompiledRegistryFailsClosedUntilReviewedOperatorsAreCommitted)
{
    const auto testnet =
        nova::node::LoadCompiledBootstrapConfig(nova::consensus::TestnetNetworkParams());
    EXPECT_EQ(testnet.error, nova::node::BootstrapConfigError::kNone);
    EXPECT_TRUE(testnet.seeds.empty());
    EXPECT_TRUE(testnet.dns_seeds.empty());

    const auto regtest =
        nova::node::LoadCompiledBootstrapConfig(nova::consensus::RegtestNetworkParams());
    EXPECT_EQ(regtest.error, nova::node::BootstrapConfigError::kNone);
    EXPECT_TRUE(regtest.seeds.empty());
    EXPECT_TRUE(regtest.dns_seeds.empty());
}

} // namespace
