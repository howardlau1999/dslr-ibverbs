#include "dslr/rdma/endpoint.h"

#include <gtest/gtest.h>

namespace dslr::rdma {
namespace {

TEST(EndpointInfo, WireRoundTrip) {
  EndpointInfo info;
  info.qp_num = 0x00ABCDEF;
  info.psn = 0x00123456;
  info.lid = 0xBEEF;
  for (size_t i = 0; i < info.gid.size(); ++i) {
    info.gid[i] = static_cast<uint8_t>(0xF0 + i);
  }
  info.table_addr = 0x7F00'1234'5678'9ABCull;
  info.table_rkey = 0xDEADBEEF;
  info.table_locks = 1u << 20;

  const auto bytes = info.to_wire();
  ASSERT_EQ(bytes.size(), EndpointInfo::kWireSize);
  const EndpointInfo back = EndpointInfo::from_wire(bytes);
  EXPECT_EQ(back.qp_num, info.qp_num);
  EXPECT_EQ(back.psn, info.psn);
  EXPECT_EQ(back.lid, info.lid);
  EXPECT_EQ(back.gid, info.gid);
  EXPECT_EQ(back.table_addr, info.table_addr);
  EXPECT_EQ(back.table_rkey, info.table_rkey);
  EXPECT_EQ(back.table_locks, info.table_locks);
}

TEST(EndpointInfo, WireFormatIsBigEndian) {
  EndpointInfo info;
  info.qp_num = 0x01020304;
  const auto bytes = info.to_wire();
  EXPECT_EQ(bytes[0], 0x01);
  EXPECT_EQ(bytes[1], 0x02);
  EXPECT_EQ(bytes[2], 0x03);
  EXPECT_EQ(bytes[3], 0x04);
}

TEST(EndpointInfo, DescribeMentionsTheTableOnlyWhenExported) {
  EndpointInfo client;
  client.qp_num = 7;
  EXPECT_EQ(client.describe().find("table="), std::string::npos);
  EndpointInfo server = client;
  server.table_locks = 16;
  EXPECT_NE(server.describe().find("table="), std::string::npos);
}

}  // namespace
}  // namespace dslr::rdma
