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
  // Transfers (docs/TRANSFERS.md). Only kOpTransfer is for clients; the
  // rest are follow-ups that settle it and reject a client caller. Each
  // learns its peer from OpCtx::source().
  kOpTransfer = 1008,      // TransferArgs -> BalanceResult of the source
  kOpXferCredit = 1009,    // CreditArgs, on the destination
  kOpXferSettle = 1010,    // SettleArgs, on the source
  kOpXferForget = 1011,    // SeqArgs, on the destination
  kOpXferQuery = 1012,     // SeqArgs, on the source
  kOpXferBounced = 1013,   // CreditArgs, on the source: destination missing
};

enum : u16 {
  kAppInsufficientFunds = 1,
  kAppOverflow = 2,
  kAppNotEmpty = 3,
  kAppBusy = 4,  // too many transfers in flight from this account
};

struct AccountV1 {
  i64 balance_cents;  //   0
  u64 op_count;       //   8
  u64 created_ns;     //  16  from args, not the clock, so replay is exact
  u64 updated_ns;     //  24
  u8 email_len;       //  32
  u8 name_len;        //  33
  u16 flags;          //  34
  u16 n_out;          //  36  outbox entries after this struct (transfers)
  u16 n_in;           //  38  inbox entries after the outbox
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

// Payload tail: n_out OutEntry, then n_in InEntry (docs/TRANSFERS.md).
struct OutEntry {
  u64 seq;        // source version at the debit
  i64 amount;
  ObjectID to;
};
struct InEntry {
  ObjectID from;
  u64 seq;
  u8 ok;          // 1 credited, 0 refused (overflow)
  u8 pad[7];
};
static_assert(sizeof(OutEntry) == 32 && sizeof(InEntry) == 32);
inline constexpr u16 kMaxOutbox = 64;
inline constexpr u32 kMaxEntries = 1024;

struct TransferArgs {
  ObjectID to;
  i64 amount;
  u64 now_ns;
};
struct CreditArgs {
  u64 seq;
  i64 amount;
};
enum : u8 { kSettleRefused = 0, kSettleOk = 1, kSettleRetry = 2 };
struct SettleArgs {
  u64 seq;
  u64 code;  // a whole word: emit copies args in 8-byte loads (store forwarding)
};
struct SeqArgs {
  u64 seq;
};

struct BalanceResult {
  i64 balance_cents;
  u64 op_count;
};

// Registers the type and its ops in the process registry.
void register_type();

}  // namespace snc::account
