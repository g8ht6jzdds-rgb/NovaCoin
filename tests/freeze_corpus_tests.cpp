#include <gtest/gtest.h>

#include "inspection/freeze_corpus.hpp"

#include <string>

namespace
{

TEST(FreezeCorpus, EmitsCanonicalBytesRepeatedlyForCleanIdentity)
{
    const nova::inspection::FreezeCorpusIdentity identity{
        "0123456789abcdef0123456789abcdef01234567", "89abcdef0123456789abcdef0123456789abcdef",
        "clean"};
    const auto first = nova::inspection::RenderFreezeCorpus(identity);
    const auto second = nova::inspection::RenderFreezeCorpus(identity);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(*first, *second);
    EXPECT_EQ(first->substr(0U, 42U), "format=novacoin-gate1-parameter-corpus-v1\n");
    EXPECT_NE(first->find("candidate.commit=0123456789abcdef0123456789abcdef01234567\n"),
              std::string::npos);
    EXPECT_NE(first->find("network=REGTEST\n"), std::string::npos);
    EXPECT_NE(first->find("network=TESTNET\n"), std::string::npos);
    EXPECT_NE(first->find("vector.id=T10\n"), std::string::npos);
    EXPECT_EQ(first->back(), '\n');
}

TEST(FreezeCorpus, RejectsDirtyOrMalformedProvenance)
{
    EXPECT_FALSE(
        nova::inspection::RenderFreezeCorpus({"0123456789abcdef0123456789abcdef01234567",
                                              "89abcdef0123456789abcdef0123456789abcdef", "dirty"})
            .has_value());
    EXPECT_FALSE(nova::inspection::RenderFreezeCorpus(
                     {"not-a-commit", "89abcdef0123456789abcdef0123456789abcdef", "clean"})
                     .has_value());
}

TEST(FreezeCorpus, AcceptsOnlyCanonicalRelativeChecksumManifestGrammar)
{
    constexpr auto valid =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef  artifacts/a.txt\n"
        "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff  report.txt\n";
    EXPECT_TRUE(nova::inspection::IsCanonicalSha256Manifest(valid));
    EXPECT_FALSE(nova::inspection::IsCanonicalSha256Manifest(""));
    EXPECT_FALSE(nova::inspection::IsCanonicalSha256Manifest(
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef artifacts/a.txt\n"));
    EXPECT_FALSE(nova::inspection::IsCanonicalSha256Manifest(
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef  ../escape\n"));
    EXPECT_FALSE(nova::inspection::IsCanonicalSha256Manifest(
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef  /absolute\n"));
}

} // namespace
