#include "build/version.hpp"

#include <gtest/gtest.h>

#include <sstream>

TEST(BuildVersion, OnlyStandaloneOption)
{
    const char* standalone[]{"test", "--version"};
    const char* other[]{"test", "--version-extra"};
    const char* mixed[]{"test", "--version", "--version"};
    EXPECT_TRUE(nova::build::IsVersionRequest(2, standalone));
    EXPECT_FALSE(nova::build::IsVersionRequest(1, standalone));
    EXPECT_FALSE(nova::build::IsVersionRequest(2, other));
    EXPECT_FALSE(nova::build::IsVersionRequest(3, mixed));
    EXPECT_FALSE(nova::build::IsVersionRequest(0, nullptr));
}

TEST(BuildVersion, ExactDeterministicFormat)
{
    std::ostringstream output;
    nova::build::PrintVersion(output, "test");
    EXPECT_EQ(output.str(), "test " + std::string{nova::build::kApplicationVersion} +
                                " revision=" + std::string{nova::build::kSourceRevision} +
                                " tree=" + std::string{nova::build::kSourceTree} +
                                " state=" + std::string{nova::build::kSourceState} + "\n");
}
