// Account type (spec: Benchmark workloads). Schema 1, 200-byte payload.
#pragma once

#include "snc/registry.h"

namespace snc::account {

inline constexpr u32 kTypeId = 256;
inline constexpr u32 kEmailIndex = 1;  // Account.email -> ObjectID

enum : OpID {
  kOpCreate = 1001,
  kOpGet = 1002,         // returns the 200-byte payload
  kOpDeposit = 1003,
  kOpWithdraw = 1004,
  kOpSetEmail = 1005,
  kOpGetBalance = 1006,  // returns BalanceResult (16 bytes, fits inline)
  kOpDelete = 1007,
};

enum : u16 {
  kAppInsufficientFunds = 1,
  kAppOverflow = 2,
  kAppNotEmpty = 3,
};

struct AccountV1 {
  i64 balance_cents;  //   0
  u64 op_count;       //   8
  u64 created_ns;     //  16  from args, not the clock, so replay is exact
  u64 updated_ns;     //  24
  u8 email_len;       //  32
  u8 name_len;        //  33
  u16 flags;          //  34
  u32 reserved;       //  36
  char email[64];     //  40  indexed by Account.email (step 9)
  char name[96];      // 104
};
static_assert(sizeof(AccountV1) == 200);

struct CreateArgs {
  i64 balance_cents;
  u64 created_ns;
  u8 email_len;
  u8 name_len;
  u8 pad[6];
  char email[64];
  char name[96];
};

struct AmountArgs {
  i64 amount_cents;
  u64 now_ns;
};

struct SetEmailArgs {
  u64 now_ns;
  u8 email_len;
  char email[64];
};

struct BalanceResult {
  i64 balance_cents;
  u64 op_count;
};

// Registers the type and its ops in the process registry.
void register_type();

}  // namespace snc::account
