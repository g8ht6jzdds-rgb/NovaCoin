#include <gtest/gtest.h>

#include "consensus/network_params.hpp"
#include "storage/network_identity.hpp"

#include <filesystem>
#include <fstream>
#include <system_error>

namespace
{

class NetworkIdentityTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        root_ = std::filesystem::temp_directory_path() / "novacoin-network-identity-test";
        std::error_code error;
        std::filesystem::remove_all(root_, error);
        ASSERT_FALSE(error);
    }

    void TearDown() override
    {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    std::filesystem::path root_;
};

TEST_F(NetworkIdentityTest, CreatesAndReopensMatchingRegtestIdentity)
{
    const auto& network = nova::consensus::RegtestNetworkParams();
    ASSERT_EQ(nova::storage::EnsureNetworkIdentity(root_, network),
              nova::storage::NetworkIdentityError::kNone);
    EXPECT_TRUE(std::filesystem::is_regular_file(nova::storage::NetworkIdentityMarkerPath(root_)));
    EXPECT_EQ(nova::storage::EnsureNetworkIdentity(root_, network),
              nova::storage::NetworkIdentityError::kNone);
}

TEST_F(NetworkIdentityTest, RejectsAnotherNetworkMarkerBeforeJournalCanOpen)
{
    ASSERT_EQ(nova::storage::EnsureNetworkIdentity(root_, nova::consensus::RegtestNetworkParams()),
              nova::storage::NetworkIdentityError::kNone);
    EXPECT_EQ(nova::storage::EnsureNetworkIdentity(root_, nova::consensus::TestnetNetworkParams()),
              nova::storage::NetworkIdentityError::kNetworkMismatch);
}

TEST_F(NetworkIdentityTest, RejectsLegacyNonemptyDirectoryWithoutMarker)
{
    std::filesystem::create_directories(root_);
    std::ofstream output{root_ / "blocks.dat", std::ios::binary};
    ASSERT_TRUE(output.is_open());
    output.put('x');
    output.close();
    EXPECT_EQ(nova::storage::EnsureNetworkIdentity(root_, nova::consensus::RegtestNetworkParams()),
              nova::storage::NetworkIdentityError::kLegacyDirectoryWithoutMarker);
}

TEST_F(NetworkIdentityTest, RejectsTruncatedOrCorruptMarker)
{
    std::filesystem::create_directories(root_);
    std::ofstream output{nova::storage::NetworkIdentityMarkerPath(root_), std::ios::binary};
    ASSERT_TRUE(output.is_open());
    output.put('N');
    output.close();
    EXPECT_EQ(nova::storage::EnsureNetworkIdentity(root_, nova::consensus::RegtestNetworkParams()),
              nova::storage::NetworkIdentityError::kMalformedMarker);
}

} // namespace
