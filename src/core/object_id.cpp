#include "snc/object_id.h"

#include <xxhash.h>

namespace snc {

u64 snc_hash64(const ObjectID& id) {
  u8 bytes[16];
  std::memcpy(bytes, &id.hi, 8);
  std::memcpy(bytes + 8, &id.lo, 8);
  return XXH3_64bits(bytes, sizeof(bytes));
}

std::string ObjectID::to_string() const {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out(33, '-');
  for (int i = 0; i < 16; ++i) {
    out[i] = kHex[(hi >> (60 - 4 * i)) & 0xf];
    out[17 + i] = kHex[(lo >> (60 - 4 * i)) & 0xf];
  }
  return out;
}

std::optional<ObjectID> ObjectID::parse(std::string_view text) {
  if (text.size() != 33 || text[16] != '-') return std::nullopt;
  auto parse_half = [](std::string_view h, u64& out) {
    out = 0;
    for (char c : h) {
      u64 v;
      if (c >= '0' && c <= '9') v = c - '0';
      else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
      else return false;  // uppercase and other characters are rejected
      out = (out << 4) | v;
    }
    return true;
  };
  ObjectID id;
  if (!parse_half(text.substr(0, 16), id.hi) || !parse_half(text.substr(17, 16), id.lo)) {
    return std::nullopt;
  }
  return id;
}

const char* status_name(Status s) {
  switch (s) {
    case Status::OK: return "OK";
    case Status::E_NOT_FOUND: return "E_NOT_FOUND";
    case Status::E_TYPE_MISMATCH: return "E_TYPE_MISMATCH";
    case Status::E_UNKNOWN_OP: return "E_UNKNOWN_OP";
    case Status::E_BAD_ARGS: return "E_BAD_ARGS";
    case Status::E_VERSION_CONFLICT: return "E_VERSION_CONFLICT";
    case Status::E_APP: return "E_APP";
    case Status::E_TOO_LARGE: return "E_TOO_LARGE";
    case Status::E_NO_MEMORY: return "E_NO_MEMORY";
    case Status::E_OVERLOADED: return "E_OVERLOADED";
    case Status::E_SHUTTING_DOWN: return "E_SHUTTING_DOWN";
  }
  return "E_UNKNOWN_STATUS";
}

}  // namespace snc
