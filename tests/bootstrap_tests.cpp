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

} // namespace
