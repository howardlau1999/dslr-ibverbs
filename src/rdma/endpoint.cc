#include "dslr/rdma/endpoint.h"

#include <cstdio>

namespace dslr::rdma {
namespace {

template <typename T>
void put_be(uint8_t*& out, T value) {
  for (int shift = static_cast<int>(sizeof(T)) * 8 - 8; shift >= 0; shift -= 8) {
    *out++ = static_cast<uint8_t>(value >> shift);
  }
}

template <typename T>
T get_be(const uint8_t*& in) {
  T value = 0;
  for (size_t i = 0; i < sizeof(T); ++i) {
    value = static_cast<T>((value << 8) | *in++);
  }
  return value;
}

}  // namespace

std::array<uint8_t, EndpointInfo::kWireSize> EndpointInfo::to_wire() const {
  std::array<uint8_t, kWireSize> bytes{};
  uint8_t* out = bytes.data();
  put_be(out, qp_num);
  put_be(out, psn);
  put_be(out, lid);
  for (uint8_t b : gid) {
    *out++ = b;
  }
  put_be(out, table_addr);
  put_be(out, table_rkey);
  put_be(out, table_locks);
  *out++ = table_big_endian ? 1 : 0;
  return bytes;
}

EndpointInfo EndpointInfo::from_wire(const std::array<uint8_t, kWireSize>& bytes) {
  EndpointInfo info;
  const uint8_t* in = bytes.data();
  info.qp_num = get_be<uint32_t>(in);
  info.psn = get_be<uint32_t>(in);
  info.lid = get_be<uint16_t>(in);
  for (uint8_t& b : info.gid) {
    b = *in++;
  }
  info.table_addr = get_be<uint64_t>(in);
  info.table_rkey = get_be<uint32_t>(in);
  info.table_locks = get_be<uint32_t>(in);
  info.table_big_endian = (*in++ & 1) != 0;
  return info;
}

std::string EndpointInfo::describe() const {
  char gid_text[48];
  std::snprintf(gid_text, sizeof(gid_text),
                "%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x", gid[0],
                gid[1], gid[2], gid[3], gid[4], gid[5], gid[6], gid[7], gid[8], gid[9], gid[10],
                gid[11], gid[12], gid[13], gid[14], gid[15]);
  std::string text = "qpn=" + std::to_string(qp_num) + " psn=" + std::to_string(psn) +
                     " lid=" + std::to_string(lid) + " gid=" + gid_text;
  if (table_locks != 0) {
    char addr[32];
    std::snprintf(addr, sizeof(addr), "0x%llx", static_cast<unsigned long long>(table_addr));
    text += " table=" + std::string(addr) + " rkey=" + std::to_string(table_rkey) +
            " locks=" + std::to_string(table_locks) + (table_big_endian ? " (big-endian)" : "");
  }
  return text;
}

}  // namespace dslr::rdma
