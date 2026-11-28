#include "explorer/http_response_parser.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace nova::explorer::detail
{
namespace
{

constexpr std::size_t kSmallLimit = 128U;
constexpr std::string_view kPrefix{"{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{"};
constexpr std::string_view kFields{"\"encoding\":\"nova-snapshot-v1\",\"data\":\"0102\"}}"};

TEST(ExplorerSnapshotJsonParser, ExtractsBoundedHexView)
{
    const std::string body = std::string{kPrefix} + std::string{kFields};
    const auto hex = ExtractRpcSnapshotHex(body, kSmallLimit);
    ASSERT_TRUE(hex.has_value());
    EXPECT_EQ(*hex, "0102");
    EXPECT_EQ(hex->data(), body.data() + body.find("0102"));
}

TEST(ExplorerSnapshotJsonParser, RejectsMalformedOrUnsupportedEnvelope)
{
    const std::string body = std::string{kPrefix} + std::string{kFields};
    const std::string cases[] = {
        body.substr(1U),
        "{\"jsonrpc\":\"1.0\",\"id\":1,\"result\":{" + std::string{kFields},
        std::string{kPrefix} + "\"encoding\":\"other\",\"data\":\"0102\"}}",
        std::string{kPrefix} + "not-json,\"encoding\":\"nova-snapshot-v1\",\"data\":\"0102\"}}",
        std::string{kPrefix} +
            "\"unexpected\":true,\"encoding\":\"nova-snapshot-v1\",\"data\":\"0102\"}}",
        std::string{kPrefix} + "\"encoding\":\"nova-snapshot-v1\"}}",
        std::string{kPrefix} + "\"encoding\":\"nova-snapshot-v1\",\"data\":0102}}",
        std::string{kPrefix} + "\"encoding\":\"nova-snapshot-v1\",\"data\":\"0102\"} extra",
        std::string{kPrefix} + "\"encoding\":\"nova-snapshot-v1\",\"data\":\"0102\"}",
    };
    for (const auto& input : cases) {
        SCOPED_TRACE(input);
        EXPECT_FALSE(ExtractRpcSnapshotHex(input, kSmallLimit).has_value());
    }
}

TEST(ExplorerSnapshotJsonParser, EnforcesProductionBodyBoundary)
{
    constexpr std::string_view kDataPrefix{
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"encoding\":\"nova-snapshot-v1\",\"data\":\""};
    constexpr std::string_view kSuffix{"\"}}"};
    ASSERT_LT(kDataPrefix.size() + kSuffix.size(), kExplorerRpcSnapshotMaxResponseBytes);
    const auto hex_size =
        kExplorerRpcSnapshotMaxResponseBytes - kDataPrefix.size() - kSuffix.size();
    std::string body{kDataPrefix};
    body.append(hex_size, '0');
    body.append(kSuffix);
    ASSERT_EQ(body.size(), kExplorerRpcSnapshotMaxResponseBytes);
    const auto hex = ExtractRpcSnapshotHex(body, kExplorerRpcSnapshotMaxResponseBytes);
    ASSERT_TRUE(hex.has_value());
    EXPECT_EQ(hex->size(), hex_size);

    body.push_back(' ');
    EXPECT_FALSE(ExtractRpcSnapshotHex(body, kExplorerRpcSnapshotMaxResponseBytes).has_value());
}

} // namespace
} // namespace nova::explorer::detail
