#include "rpc/http_request_parser.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <string>
#include <string_view>

namespace nova::rpc::detail
{
namespace
{

constexpr std::size_t kHeaderLimit = 128U;
constexpr std::size_t kBodyLimit = 16U;

[[nodiscard]] std::string Request(const std::string_view headers, const std::string_view body)
{
    return "POST /rpc HTTP/1.1\r\n" + std::string{headers} + "\r\n\r\n" + std::string{body};
}

TEST(RpcHttpParser, AcceptsExactBodyBoundaryAndBindsParsedViews)
{
    const std::string input =
        Request("Authorization: Basic dXNlcjpwYXNz\r\nContent-Length: 16", "0123456789abcdef");
    ParsedHttpRequest parsed{};

    EXPECT_EQ(ParseHttpRequest(input, kHeaderLimit, kBodyLimit, parsed), HttpParseState::kReady);
    EXPECT_EQ(parsed.method, "POST");
    EXPECT_EQ(parsed.authorization, "Basic dXNlcjpwYXNz");
    EXPECT_EQ(parsed.body, "0123456789abcdef");
    EXPECT_EQ(parsed.body.data(), input.data() + input.find("\r\n\r\n") + 4U);
}

TEST(RpcHttpParser, DistinguishesIncompleteHeaderAndBodyAtTheirLimits)
{
    const std::string incomplete_header = "POST /rpc HTTP/1.1\r\nHost: local\r\n";
    ParsedHttpRequest parsed{};
    EXPECT_EQ(ParseHttpRequest(incomplete_header, incomplete_header.size(), kBodyLimit, parsed),
              HttpParseState::kIncomplete);
    EXPECT_EQ(
        ParseHttpRequest(incomplete_header + "X", incomplete_header.size(), kBodyLimit, parsed),
        HttpParseState::kInvalid);

    const auto complete_header = Request("Content-Length: 4", "");
    EXPECT_EQ(ParseHttpRequest(complete_header, complete_header.size(), kBodyLimit, parsed),
              HttpParseState::kIncomplete);
}

TEST(RpcHttpParser, KeepsUnterminatedLfInputIncompleteUntilHeaderLimit)
{
    const std::string input = "POST /rpc HTTP/1.1\nContent-Length: 0\n\n";
    ParsedHttpRequest parsed{"sentinel-method", "sentinel-auth", "sentinel-body"};

    EXPECT_EQ(ParseHttpRequest(input, input.size(), kBodyLimit, parsed),
              HttpParseState::kIncomplete);
    EXPECT_EQ(parsed.method, "sentinel-method");
    EXPECT_EQ(ParseHttpRequest(input, input.size() - 1U, kBodyLimit, parsed),
              HttpParseState::kInvalid);
    EXPECT_EQ(parsed.method, "sentinel-method");
}

TEST(RpcHttpParser, RejectsMalformedFramingAndOverflowWithoutPublishingPartialResult)
{
    const ParsedHttpRequest sentinel{"sentinel-method", "sentinel-auth", "sentinel-body"};
    const std::string too_large_length = Request(
        "Content-Length: " + std::to_string(std::numeric_limits<std::size_t>::max()) + "0", "");
    const std::string cases[] = {
        Request("", "body"),
        Request("Content-Length: 1\r\nContent-Length: 1", "x"),
        Request("Authorization: Basic a\r\nAuthorization: Basic b\r\nContent-Length: 1", "x"),
        Request("Transfer-Encoding: chunked", "0\r\n\r\n"),
        Request("Content-Length: +1", "x"),
        Request("Content-Length: 17", ""),
        too_large_length,
        Request("Content-Length: 1", "xy"),
        "GET /rpc HTTP/1.1\r\nContent-Length: 0\r\n\r\n",
    };

    for (const auto& input : cases) {
        SCOPED_TRACE(input);
        ParsedHttpRequest parsed = sentinel;
        EXPECT_EQ(ParseHttpRequest(input, kHeaderLimit, kBodyLimit, parsed),
                  HttpParseState::kInvalid);
        EXPECT_EQ(parsed.method, sentinel.method);
        EXPECT_EQ(parsed.authorization, sentinel.authorization);
        EXPECT_EQ(parsed.body, sentinel.body);
    }
}

TEST(RpcHttpParser, RejectsHeaderOneByteOverLimitAndKeepsIncompleteBodyUnpublished)
{
    ParsedHttpRequest parsed{"sentinel-method", "sentinel-auth", "sentinel-body"};
    const auto valid = Request("Content-Length: 1", "x");
    const auto header_end = valid.find("\r\n\r\n") + 4U;
    ASSERT_GT(header_end, 0U);
    EXPECT_EQ(ParseHttpRequest(valid, header_end - 1U, kBodyLimit, parsed),
              HttpParseState::kInvalid);
    EXPECT_EQ(parsed.method, "sentinel-method");

    const auto incomplete_body = Request("Content-Length: 2", "x");
    EXPECT_EQ(ParseHttpRequest(incomplete_body, kHeaderLimit, kBodyLimit, parsed),
              HttpParseState::kIncomplete);
    EXPECT_EQ(parsed.method, "sentinel-method");
}

TEST(RpcHttpParser, AcceptsHeaderAtExactConfiguredBoundary)
{
    const auto input = Request("Content-Length: 0", "");
    ParsedHttpRequest parsed{};
    const auto header_bytes = input.size();
    EXPECT_EQ(ParseHttpRequest(input, header_bytes, kBodyLimit, parsed), HttpParseState::kReady);
    EXPECT_TRUE(parsed.body.empty());
}

} // namespace
} // namespace nova::rpc::detail
