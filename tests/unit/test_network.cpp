/**
 * @file tests/unit/test_network.cpp
 * @brief Test src/network.*
 */
#include "../tests_common.h"

#include <boost/asio.hpp>
#include <src/network.h>

struct MdnsInstanceNameTest: testing::TestWithParam<std::tuple<std::string, std::string>> {};

TEST_P(MdnsInstanceNameTest, Run) {
  auto [input, expected] = GetParam();
  ASSERT_EQ(net::mdns_instance_name(input), expected);
}

INSTANTIATE_TEST_SUITE_P(
  MdnsInstanceNameTests,
  MdnsInstanceNameTest,
  testing::Values(
    std::make_tuple("shortname-123", "shortname-123"),
    std::make_tuple("space 123", "space-123"),
    std::make_tuple("hostname.domain.test", "hostname"),
    std::make_tuple("&", "Hermes"),
    std::make_tuple("", "Hermes"),
    std::make_tuple("😁", "Hermes"),
    std::make_tuple(std::string(128, 'a'), std::string(63, 'a'))
  )
);

TEST(NetworkTest, HostCreateReturnsEmptyWhenPortIsAlreadyBound) {
  boost::asio::io_context io_context;
  boost::asio::ip::udp::socket blocker {
    io_context,
    {
      boost::asio::ip::udp::v4(),
      0
    }
  };
  const auto occupied_port =
    blocker.local_endpoint().port();
  ENetAddress address {};

  const auto host =
    net::host_create(net::IPV4, address, occupied_port);

  EXPECT_FALSE(host);
}
