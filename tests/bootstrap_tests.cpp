#include <gtest/gtest.h>

#include "consensus/network_params.hpp"
#include "node/bootstrap_config.hpp"
#include "node/bootstrap_reader.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <new>
#include <streambuf>
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
          "7825772a2dd18d4619622b198052a9c82ca17b0f847754f6cb24960df7a7914c\n"
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
          "7825772a2dd18d4619622b198052a9c82ca17b0f847754f6cb24960df7a7914c\n");
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

TEST_F(BootstrapConfigTest, RejectsWrongNetwork)
{
    Write("version=1\nnetwork=testnet\nmagic=dab5bffa\ngenesis="
          "c974d11a5276ca7eb69b1ec0a8062fb47b49c02f5ac726532483d090ac53eb79\nseed=seed.example:"
          "18444\n");
    EXPECT_EQ(
        nova::node::LoadStaticBootstrapConfig(path_, nova::consensus::RegtestNetworkParams()).error,
        nova::node::BootstrapConfigError::kNetworkMismatch);
}

TEST_F(BootstrapConfigTest, RejectsWrongGenesisWithCorrectNetworkAndMagic)
{
    Write("version=1\nnetwork=regtest\nmagic=dab5bffa\ngenesis="
          "d974d11a5276ca7eb69b1ec0a8062fb47b49c02f5ac726532483d090ac53eb79\n"
          "seed=127.0.0.1:18444\n");
    EXPECT_EQ(
        nova::node::LoadStaticBootstrapConfig(path_, nova::consensus::RegtestNetworkParams()).error,
        nova::node::BootstrapConfigError::kNetworkMismatch);
}

TEST_F(BootstrapConfigTest, RejectsWrongMagicWithCorrectNetworkAndGenesis)
{
    Write("version=1\nnetwork=regtest\nmagic=dab5bff0\ngenesis="
          "c974d11a5276ca7eb69b1ec0a8062fb47b49c02f5ac726532483d090ac53eb79\n"
          "seed=127.0.0.1:18444\n");
    EXPECT_EQ(
        nova::node::LoadStaticBootstrapConfig(path_, nova::consensus::RegtestNetworkParams()).error,
        nova::node::BootstrapConfigError::kNetworkMismatch);
}

TEST_F(BootstrapConfigTest, RejectsLateWrongGenesisAfterValidSeed)
{
    Write("version=1\nnetwork=regtest\nmagic=dab5bffa\nseed=127.0.0.1:18444\ngenesis="
          "d974d11a5276ca7eb69b1ec0a8062fb47b49c02f5ac726532483d090ac53eb79\n");
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
          "7825772a2dd18d4619622b198052a9c82ca17b0f847754f6cb24960df7a7914c\n"
          "dnsseed=-bad.novacoin.test\n");
    EXPECT_EQ(
        nova::node::LoadStaticBootstrapConfig(path_, nova::consensus::TestnetNetworkParams()).error,
        nova::node::BootstrapConfigError::kInvalidEndpoint);

    Write("version=1\nnetwork=testnet\nmagic=dab5bffb\ngenesis="
          "7825772a2dd18d4619622b198052a9c82ca17b0f847754f6cb24960df7a7914c\n"
          "dnsseed=one.novacoin.test\n"
          "dnsseed=one.novacoin.test\n");
    EXPECT_EQ(
        nova::node::LoadStaticBootstrapConfig(path_, nova::consensus::TestnetNetworkParams()).error,
        nova::node::BootstrapConfigError::kNonCanonical);

    Write("version=1\nnetwork=testnet\nmagic=dab5bffb\ngenesis="
          "7825772a2dd18d4619622b198052a9c82ca17b0f847754f6cb24960df7a7914c\n"
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

class CountedBootstrapBuffer final : public std::streambuf
{
  public:
    explicit CountedBootstrapBuffer(const std::size_t available, const bool fail = false)
        : available_(available), fail_(fail)
    {
    }
    std::streamsize requested{};
    std::size_t consumed{};
    std::size_t calls{};

  protected:
    std::streamsize xsgetn(char* destination, const std::streamsize count) override
    {
        ++calls;
        requested += count;
        if (fail_) {
            throw std::ios_base::failure{"injected read failure"};
        }
        const auto size = std::min(available_, static_cast<std::size_t>(count));
        std::fill_n(destination, size, 'x');
        available_ -= size;
        consumed += size;
        return static_cast<std::streamsize>(size);
    }

  private:
    std::size_t available_;
    bool fail_;
};

TEST(BootstrapReader, BoundsRequestsAndConsumptionAtLimitPlusOne)
{
    constexpr auto limit = nova::node::detail::kMaximumBootstrapFileSize;
    for (const auto size :
         {std::size_t{0U}, std::size_t{1U}, limit - 1U, limit, limit + 1U, limit * 64U}) {
        CountedBootstrapBuffer buffer{size};
        std::istream input{&buffer};
        const auto read = nova::node::detail::ReadBootstrapText(input);
        EXPECT_EQ(buffer.calls, 1U);
        EXPECT_EQ(buffer.requested, static_cast<std::streamsize>(limit + 1U));
        EXPECT_EQ(buffer.consumed, std::min(size, limit + 1U));
        EXPECT_EQ(read.size, buffer.consumed);
        EXPECT_EQ(read.error, size > limit ? nova::node::BootstrapConfigError::kOversized
                                           : nova::node::BootstrapConfigError::kNone);
    }
}

TEST(BootstrapReader, PreservesShortEofWithExceptionMaskAndFailsClosedOnReadErrors)
{
    CountedBootstrapBuffer short_buffer{17U};
    std::istream short_input{&short_buffer};
    short_input.exceptions(std::ios::failbit | std::ios::badbit);
    const auto short_read = nova::node::detail::ReadBootstrapText(short_input);
    EXPECT_EQ(short_read.error, nova::node::BootstrapConfigError::kNone);
    EXPECT_EQ(short_read.size, 17U);
    for (const bool exceptions : {false, true}) {
        CountedBootstrapBuffer failed_buffer{100U, true};
        std::istream input{&failed_buffer};
        if (exceptions) {
            input.exceptions(std::ios::badbit);
        }
        const auto failed = nova::node::detail::ReadBootstrapText(input);
        EXPECT_EQ(failed.error, nova::node::BootstrapConfigError::kReadFailure);
        EXPECT_EQ(failed.size, 0U);
        EXPECT_EQ(failed_buffer.consumed, 0U);
    }
    CountedBootstrapBuffer unread{100U};
    std::istream failed{&unread};
    failed.setstate(std::ios::failbit);
    EXPECT_EQ(nova::node::detail::ReadBootstrapText(failed).error,
              nova::node::BootstrapConfigError::kReadFailure);
    EXPECT_EQ(unread.calls, 0U);
}

TEST_F(BootstrapConfigTest, RejectsOversizeBeforeParsingAndReturnsNoEndpoints)
{
    constexpr auto limit = nova::node::detail::kMaximumBootstrapFileSize;
    for (const auto size : {limit - 1U, limit, limit + 1U, limit * 64U}) {
        Write(std::string(size, 'x'));
        const auto loaded =
            nova::node::LoadStaticBootstrapConfig(path_, nova::consensus::RegtestNetworkParams());
        EXPECT_EQ(loaded.error, size > limit ? nova::node::BootstrapConfigError::kOversized
                                             : nova::node::BootstrapConfigError::kNonCanonical);
        EXPECT_TRUE(loaded.seeds.empty());
        EXPECT_TRUE(loaded.dns_seeds.empty());
    }
    Write("");
    EXPECT_EQ(
        nova::node::LoadStaticBootstrapConfig(path_, nova::consensus::RegtestNetworkParams()).error,
        nova::node::BootstrapConfigError::kNonCanonical);
    Write("version=1\nnetwork=regtest\nmagic=dab5bffa\ngenesis="
          "c974d11a5276ca7eb69b1ec0a8062fb47b49c02f5ac726532483d090ac53eb79\n"
          "seed=127.0.0.1:18444");
    EXPECT_EQ(
        nova::node::LoadStaticBootstrapConfig(path_, nova::consensus::RegtestNetworkParams()).error,
        nova::node::BootstrapConfigError::kNonCanonical);
    std::filesystem::remove(path_);
    EXPECT_EQ(
        nova::node::LoadStaticBootstrapConfig(path_, nova::consensus::RegtestNetworkParams()).error,
        nova::node::BootstrapConfigError::kReadFailure);
}

} // namespace
