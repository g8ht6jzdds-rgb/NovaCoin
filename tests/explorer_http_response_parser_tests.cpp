#include "explorer/http_response_parser.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace nova::explorer::detail
{
namespace
{

constexpr std::size_t kSmallLimit = 64U;

TEST(ExplorerHttpResponseParser, AcceptsBoundedResponseAndReturnsBodyView)
{
    constexpr std::string_view input{"HTTP/1.1 200 OK\r\ncOnTeNt-LeNgTh: 4\r\n\r\ndata"};
    const auto body = ParseRpcSnapshotHttpResponse(input, kSmallLimit);
    ASSERT_TRUE(body.has_value());
    EXPECT_EQ(*body, "data");
    EXPECT_EQ(body->data(), input.data() + input.find("\r\n\r\n") + 4U);
}

TEST(ExplorerHttpResponseParser, RejectsMalformedAndIncompleteFrames)
{
    const std::string_view cases[] = {
        "HTTP/1.1 200 OK\r\nContent-Length: 4\r\ndata",
        "HTTP/1.1 201 Created\r\nContent-Length: 4\r\n\r\ndata",
        "HTTP/1.1 200 OK\r\n\r\ndata",
        "HTTP/1.1 200 OK\r\nContent-Length: 4\r\nContent-Length: 4\r\n\r\ndata",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 4\r\n\r\ndata",
        "HTTP/1.1 200 OK\r\nContent-Length: 4\r\ncOnTeNt-LeNgTh: 4\r\n\r\ndata",
        "HTTP/1.1 200 OK\r\nContent-Length: +4\r\n\r\ndata",
        "HTTP/1.1 200 OK\r\nContent-Length: 184467440737095516160\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 65\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\ndat",
        "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\ndata",
    };
    for (const auto input : cases) {
        SCOPED_TRACE(input);
        EXPECT_FALSE(ParseRpcSnapshotHttpResponse(input, kSmallLimit).has_value());
    }
}

TEST(ExplorerHttpResponseParser, RejectsFrameAboveConfiguredResponseLimit)
{
    const std::string input(kSmallLimit + 1U, 'x');
    EXPECT_FALSE(ParseRpcSnapshotHttpResponse(input, kSmallLimit).has_value());
}

TEST(ExplorerHttpResponseParser, EnforcesProductionResponseBoundary)
{
    constexpr std::string_view kLengthDigitsText{"00000000"};
    std::string response{"HTTP/1.1 200 OK\r\nContent-Length: "};
    const auto length_start = response.size();
    response += kLengthDigitsText;
    response += "\r\n\r\n";
    constexpr std::size_t kLengthDigits = 8U;
    ASSERT_LT(response.size(), kExplorerRpcSnapshotMaxResponseBytes);
    const auto body_size = kExplorerRpcSnapshotMaxResponseBytes - response.size();
    const auto length = std::to_string(body_size);
    ASSERT_EQ(length.size(), kLengthDigits);
    response.replace(length_start, kLengthDigits, length);
    const auto header_size = response.size();
    response.append(body_size, 'x');
    ASSERT_EQ(response.size(), kExplorerRpcSnapshotMaxResponseBytes);
    const auto body = ParseRpcSnapshotHttpResponse(response, kExplorerRpcSnapshotMaxResponseBytes);
    ASSERT_TRUE(body.has_value());
    EXPECT_EQ(body->size(), kExplorerRpcSnapshotMaxResponseBytes - header_size);

    response.push_back('x');
    EXPECT_FALSE(
        ParseRpcSnapshotHttpResponse(response, kExplorerRpcSnapshotMaxResponseBytes).has_value());
}

} // namespace
} // namespace nova::explorer::detail
