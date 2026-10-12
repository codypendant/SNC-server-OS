#include "snc/wal_format.h"

#include <array>

namespace snc {

namespace {
// Table-driven CRC-32C (Castagnoli, reflected 0x82F63B78). Hardware CRC
// (SSE4.2 / ARMv8) is a later optimization behind the same function.
constexpr std::array<u32, 256> make_table() {
  std::array<u32, 256> t{};
  for (u32 i = 0; i < 256; ++i) {
    u32 c = i;
    for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : c >> 1;
    t[i] = c;
  }
  return t;
}
constexpr auto kTable = make_table();
}  // namespace

u32 crc32c(const void* data, std::size_t len, u32 seed) {
  const u8* p = static_cast<const u8*>(data);
  u32 c = ~seed;
  for (std::size_t i = 0; i < len; ++i) c = kTable[(c ^ p[i]) & 0xff] ^ (c >> 8);
  return ~c;
}

u32 encode_record(u8* dst, RecType type, u64 lsn, u32 type_id, const ObjectID& id, u64 version,
                  u16 schema_ver, const u8* payload, u32 payload_len) {
  u32 total = record_size(payload_len);
  WalRecord h{};
  h.total_len = total;
  h.lsn = lsn;
  h.rec_type = static_cast<u16>(type);
  h.type_id = type_id;
  h.id = id;
  h.version = version;
  h.schema_ver = schema_ver;
  h.payload_len = payload_len;
  std::memcpy(dst, &h, sizeof(h));
  if (payload_len) std::memcpy(dst + sizeof(h), payload, payload_len);
  std::memset(dst + sizeof(h) + payload_len, 0, total - sizeof(h) - payload_len);
  u32 crc = crc32c(dst + 4, total - 4);
  std::memcpy(dst, &crc, 4);
  return total;
}

u32 check_record(const u8* p, std::size_t avail) {
  if (avail < sizeof(WalRecord)) return 0;
  WalRecord h;
  std::memcpy(&h, p, sizeof(h));
  if (h.total_len < sizeof(WalRecord) || h.total_len > avail || (h.total_len & 7)) return 0;
  if (record_size(h.payload_len) != h.total_len) return 0;
  if (h.rec_type != 1 && h.rec_type != 2 && h.rec_type != 3 && h.rec_type != 5) return 0;
  if (crc32c(p + 4, h.total_len - 4) != h.crc32c) return 0;
  return h.total_len;
}

}  // namespace snc
