#include "snc/types/account.h"

#include <algorithm>
#include <limits>

#include "snc/snc.h"

namespace snc::account {

namespace {

AccountV1* acct(OpCtx& c) { return reinterpret_cast<AccountV1*>(c.obj()->payload()); }
const AccountV1* acct(const ObjectHeader& h) {
  return reinterpret_cast<const AccountV1*>(h.payload());
}

template <typename T>
bool args_are(ByteView a) {
  return a.len == sizeof(T);
}

template <typename T>
T args_as(ByteView a) {
  T t;
  std::memcpy(&t, a.data, sizeof(T));
  return t;
}

u32 tail_len(u32 n_out, u32 n_in) { return (n_out + n_in) * u32{sizeof(OutEntry)}; }

bool check(const u8* payload, u32 len) {
  if (len < sizeof(AccountV1)) return false;
  const auto* a = reinterpret_cast<const AccountV1*>(payload);
  return a->email_len <= sizeof(a->email) && a->name_len <= sizeof(a->name) &&
         len == sizeof(AccountV1) + tail_len(a->n_out, a->n_in);
}

// create
Status create_validate(OpCtx& c, const ObjectHeader&, ByteView args) {
  if (!args_are<CreateArgs>(args)) return Status::E_BAD_ARGS;
  auto a = args_as<CreateArgs>(args);
  if (a.email_len > sizeof(a.email) || a.name_len > sizeof(a.name)) return Status::E_BAD_ARGS;
  if (a.balance_cents < 0) return c.reject(kAppOverflow);
  return Status::OK;
}
void create_apply(OpCtx& c, ByteView args) {
  auto a = args_as<CreateArgs>(args);
  AccountV1* p = acct(c);
  p->balance_cents = a.balance_cents;
  p->created_ns = a.created_ns;
  p->updated_ns = a.created_ns;
  p->email_len = a.email_len;
  p->name_len = a.name_len;
  std::memcpy(p->email, a.email, a.email_len);
  std::memcpy(p->name, a.name, a.name_len);
}

// get / get_balance
Status read_validate(OpCtx&, const ObjectHeader&, ByteView args) {
  return args.len == 0 ? Status::OK : Status::E_BAD_ARGS;
}
void get_apply(OpCtx& c, ByteView) { c.set_result(c.obj()->payload(), sizeof(AccountV1)); }
void get_balance_apply(OpCtx& c, ByteView) {
  BalanceResult r{acct(c)->balance_cents, acct(c)->op_count};
  c.set_result(&r, sizeof(r));
}

// deposit / withdraw
Status deposit_validate(OpCtx& c, const ObjectHeader& h, ByteView args) {
  if (!args_are<AmountArgs>(args)) return Status::E_BAD_ARGS;
  auto a = args_as<AmountArgs>(args);
  if (a.amount_cents <= 0) return Status::E_BAD_ARGS;
  if (acct(h)->balance_cents > std::numeric_limits<i64>::max() - a.amount_cents) {
    return c.reject(kAppOverflow);
  }
  return Status::OK;
}
void deposit_apply(OpCtx& c, ByteView args) {
  auto a = args_as<AmountArgs>(args);
  AccountV1* p = acct(c);
  p->balance_cents += a.amount_cents;
  p->op_count += 1;
  p->updated_ns = a.now_ns;
  BalanceResult r{p->balance_cents, p->op_count};
  c.set_result(&r, sizeof(r));
}

Status withdraw_validate(OpCtx& c, const ObjectHeader& h, ByteView args) {
  if (!args_are<AmountArgs>(args)) return Status::E_BAD_ARGS;
  auto a = args_as<AmountArgs>(args);
  if (a.amount_cents <= 0) return Status::E_BAD_ARGS;
  if (acct(h)->balance_cents < a.amount_cents) return c.reject(kAppInsufficientFunds);
  return Status::OK;
}
void withdraw_apply(OpCtx& c, ByteView args) {
  auto a = args_as<AmountArgs>(args);
  AccountV1* p = acct(c);
  p->balance_cents -= a.amount_cents;
  p->op_count += 1;
  p->updated_ns = a.now_ns;
  BalanceResult r{p->balance_cents, p->op_count};
  c.set_result(&r, sizeof(r));
}

// set_email
Status set_email_validate(OpCtx&, const ObjectHeader&, ByteView args) {
  if (!args_are<SetEmailArgs>(args)) return Status::E_BAD_ARGS;
  auto a = args_as<SetEmailArgs>(args);
  return a.email_len <= sizeof(a.email) ? Status::OK : Status::E_BAD_ARGS;
}
void set_email_apply(OpCtx& c, ByteView args) {
  auto a = args_as<SetEmailArgs>(args);
  AccountV1* p = acct(c);
  std::memset(p->email, 0, sizeof(p->email));
  std::memcpy(p->email, a.email, a.email_len);
  p->email_len = a.email_len;
  p->op_count += 1;
  p->updated_ns = a.now_ns;
}

u32 email_key(const u8* payload, u32 len, u8* key_out) {
  if (len < sizeof(AccountV1)) return 0;
  const auto* a = reinterpret_cast<const AccountV1*>(payload);
  u32 n = std::min<u32>(a->email_len, sizeof(a->email));
  std::memcpy(key_out, a->email, n);
  return n;
}

// delete: only empty accounts with no transfer in flight
Status delete_validate(OpCtx& c, const ObjectHeader& h, ByteView) {
  const AccountV1* a = acct(h);
  return a->balance_cents == 0 && a->n_out == 0 && a->n_in == 0 ? Status::OK
                                                                  : c.reject(kAppNotEmpty);
}

// ---- Transfers (docs/TRANSFERS.md) ----------------------------------------------
// The tail after AccountV1 holds n_out OutEntry then n_in InEntry. Entries
// are few and short-lived, so lists are scanned and kept dense.

OutEntry* outbox(AccountV1* a) { return reinterpret_cast<OutEntry*>(a + 1); }
const OutEntry* outbox(const AccountV1* a) { return reinterpret_cast<const OutEntry*>(a + 1); }
InEntry* inbox(AccountV1* a) { return reinterpret_cast<InEntry*>(outbox(a) + a->n_out); }
const InEntry* inbox(const AccountV1* a) {
  return reinterpret_cast<const InEntry*>(outbox(a) + a->n_out);
}

int find_out(const AccountV1* a, u64 seq) {
  for (u16 i = 0; i < a->n_out; ++i) {
    if (outbox(a)[i].seq == seq) return i;
  }
  return -1;
}
int find_in(const AccountV1* a, const ObjectID& from, u64 seq) {
  for (u16 i = 0; i < a->n_in; ++i) {
    if (inbox(a)[i].seq == seq && inbox(a)[i].from == from) return i;
  }
  return -1;
}

// Entry edits. Growing resizes first (the object may move); shrinking
// moves the bytes first, then resizes.
AccountV1* add_out(OpCtx& c, const OutEntry& e) {
  u32 len = c.obj()->length;
  u8* p = c.resize(len + sizeof(OutEntry));
  auto* a = reinterpret_cast<AccountV1*>(p);
  u8* at = reinterpret_cast<u8*>(outbox(a) + a->n_out);
  std::memmove(at + sizeof(OutEntry), at, a->n_in * sizeof(InEntry));
  std::memcpy(at, &e, sizeof(e));
  ++a->n_out;
  return a;
}
AccountV1* add_in(OpCtx& c, const InEntry& e) {
  u32 len = c.obj()->length;
  auto* a = reinterpret_cast<AccountV1*>(c.resize(len + sizeof(InEntry)));
  std::memcpy(inbox(a) + a->n_in, &e, sizeof(e));
  ++a->n_in;
  return a;
}
void remove_entry(OpCtx& c, u8* at, std::size_t size) {
  u8* end = c.obj()->payload() + c.obj()->length;
  std::memmove(at, at + size, static_cast<std::size_t>(end - at) - size);
  c.resize(c.obj()->length - static_cast<u32>(size));
}
void remove_out(OpCtx& c, int i) {
  AccountV1* a = acct(c);
  --a->n_out;
  remove_entry(c, reinterpret_cast<u8*>(outbox(a) + i), sizeof(OutEntry));
}
void remove_in(OpCtx& c, int i) {
  AccountV1* a = acct(c);
  u8* at = reinterpret_cast<u8*>(inbox(a) + i);
  --a->n_in;
  remove_entry(c, at, sizeof(InEntry));
}

void emit_credit(OpCtx& c, const OutEntry& e) {
  CreditArgs ca{e.seq, e.amount};
  c.emit(e.to, kOpXferCredit, ByteView(&ca, sizeof(ca)), kOpXferBounced);
}
void emit_settle(OpCtx& c, const ObjectID& to, u64 seq, u8 code) {
  SettleArgs s{seq, code};
  c.emit(to, kOpXferSettle, ByteView(&s, sizeof(s)));
}
void emit_seq(OpCtx& c, const ObjectID& to, OpID op, u64 seq) {
  SeqArgs a{seq};
  c.emit(to, op, ByteView(&a, sizeof(a)));
}

// Follow-ups take their peer from the emitting object; a client cannot
// call them (that would mint or drop money).
template <typename T>
Status followup_validate(OpCtx& c, const ObjectHeader&, ByteView args) {
  if (c.source().is_null()) return Status::E_BAD_ARGS;
  return args_are<T>(args) ? Status::OK : Status::E_BAD_ARGS;
}

// transfer: debit here and credit the destination. When this shard owns
// the destination too, both change in one op (one atomic WAL group);
// otherwise record an outbox entry and credit it with a follow-up.
Status transfer_validate(OpCtx& c, const ObjectHeader& h, ByteView args) {
  if (!args_are<TransferArgs>(args)) return Status::E_BAD_ARGS;
  auto t = args_as<TransferArgs>(args);
  if (t.amount <= 0 || t.to == h.id || t.to.is_null()) return Status::E_BAD_ARGS;
  const AccountV1* a = acct(h);
  if (a->balance_cents < t.amount) return c.reject(kAppInsufficientFunds);
  if (const ObjectHeader* to = c.local(t.to, kTypeId)) {
    if (acct(*to)->balance_cents > std::numeric_limits<i64>::max() - t.amount) {
      return c.reject(kAppOverflow);
    }
    return Status::OK;
  }
  if (a->n_out >= kMaxOutbox) return c.reject(kAppBusy);
  return c.reserve(h.length + sizeof(OutEntry));
}
void transfer_apply(OpCtx& c, ByteView args) {
  auto t = args_as<TransferArgs>(args);
  AccountV1* a;
  if (ObjectHeader* to = c.local(t.to, kTypeId)) {
    AccountV1* b = reinterpret_cast<AccountV1*>(to->payload());
    b->balance_cents += t.amount;
    b->op_count += 1;
    a = acct(c);
  } else {
    OutEntry e{c.obj()->version + 1, t.amount, t.to};  // the version this op produces
    a = add_out(c, e);
    emit_credit(c, e);
  }
  a->balance_cents -= t.amount;
  a->op_count += 1;
  a->updated_ns = t.now_ns;
  BalanceResult r{a->balance_cents, a->op_count};
  c.set_result(&r, sizeof(r));
}

// credit: apply once per (source, seq); answer every copy with the outcome.
Status credit_validate(OpCtx& c, const ObjectHeader& h, ByteView args) {
  Status s = followup_validate<CreditArgs>(c, h, args);
  if (s != Status::OK) return s;
  // A full or oversized account answers "retry" in apply rather than
  // rejecting, so the source keeps its entry and asks again.
  if (acct(h)->n_out + acct(h)->n_in < kMaxEntries) (void)c.reserve(h.length + sizeof(InEntry));
  return Status::OK;
}
void credit_apply(OpCtx& c, ByteView args) {
  auto ca = args_as<CreditArgs>(args);
  const ObjectID from = c.source();
  AccountV1* a = acct(c);
  int i = find_in(a, from, ca.seq);
  if (i >= 0) {
    emit_settle(c, from, ca.seq, inbox(a)[i].ok ? kSettleOk : kSettleRefused);
    return;
  }
  if (c.reserved_len() < c.obj()->length + sizeof(InEntry)) {
    emit_settle(c, from, ca.seq, kSettleRetry);
    return;
  }
  bool ok = ca.amount > 0 && a->balance_cents <= std::numeric_limits<i64>::max() - ca.amount;
  a = add_in(c, InEntry{from, ca.seq, static_cast<u8>(ok), {}});
  if (ok) {
    a->balance_cents += ca.amount;
    a->op_count += 1;
  }
  emit_settle(c, from, ca.seq, ok ? kSettleOk : kSettleRefused);
}

// settle: the destination's answer, on the source.
void settle_apply(OpCtx& c, ByteView args) {
  auto s = args_as<SettleArgs>(args);
  AccountV1* a = acct(c);
  int i = find_out(a, s.seq);
  // Only the destination recorded in the entry may settle it.
  if (i >= 0 && !(outbox(a)[i].to == c.source())) return;
  if (s.code == kSettleRetry) {
    if (i >= 0) emit_credit(c, outbox(a)[i]);
    return;
  }
  if (i >= 0) {
    if (s.code == kSettleRefused) a->balance_cents += outbox(a)[i].amount;
    remove_out(c, i);
  }
  emit_seq(c, c.source(), kOpXferForget, s.seq);
}

// bounced: the destination does not exist (or is not an account). The
// engine runs it with the missing destination as source.
void bounced_apply(OpCtx& c, ByteView args) {
  auto ca = args_as<CreditArgs>(args);
  AccountV1* a = acct(c);
  int i = find_out(a, ca.seq);
  if (i < 0 || !(outbox(a)[i].to == c.source())) return;
  a->balance_cents += outbox(a)[i].amount;
  remove_out(c, i);
}

// forget: the source has durably dropped its entry; so can we.
void forget_apply(OpCtx& c, ByteView args) {
  auto p = args_as<SeqArgs>(args);
  int i = find_in(acct(c), c.source(), p.seq);
  if (i >= 0) remove_in(c, i);
}

// query: after a restart, the destination asks whether to keep its entry.
// Still in the outbox: the settle was lost (with a node link, while only
// the destination restarted), so ask for it again with the credit.
void query_apply(OpCtx& c, ByteView args) {
  auto p = args_as<SeqArgs>(args);
  const int i = find_out(acct(c), p.seq);
  if (i < 0) {
    emit_seq(c, c.source(), kOpXferForget, p.seq);
  } else if (outbox(acct(c))[i].to == c.source()) {
    emit_credit(c, outbox(acct(c))[i]);
  }
}

// After recovery: finish whatever a crash interrupted.
void resume(OpCtx& c) {
  const AccountV1* a = acct(c);
  for (u16 i = 0; i < a->n_out; ++i) emit_credit(c, outbox(a)[i]);
  for (u16 i = 0; i < a->n_in; ++i) emit_seq(c, inbox(a)[i].from, kOpXferQuery, inbox(a)[i].seq);
}

}  // namespace

void register_type() {
  Registry& r = registry();
  if (r.type(kTypeId)) return;

  TypeDescriptor t;
  t.type_id = kTypeId;
  t.name = "Account";
  t.schema_ver = 1;
  t.initial_size = sizeof(AccountV1);
  t.max_size = sizeof(AccountV1) + kMaxEntries * sizeof(OutEntry);
  t.resume = resume;
  t.check = check;
  t.create_op = kOpCreate;
  t.delete_op = kOpDelete;
  r.add_type(t);

  auto op = [&](OpID id, const char* name, OpKind kind, u32 max_args, u32 max_result,
                Status (*v)(OpCtx&, const ObjectHeader&, ByteView), void (*a)(OpCtx&, ByteView),
                bool keeps_index_key = false, bool unlogged = false) {
    OpDescriptor d;
    d.keeps_index_key = keeps_index_key;
    d.unlogged = unlogged;
    d.op_id = id;
    d.type_id = kTypeId;
    d.name = name;
    d.kind = kind;
    d.max_args = max_args;
    d.max_result = max_result;
    d.validate = v;
    d.apply = a;
    r.add_op(d);
  };
  op(kOpCreate, "Account.create", OpKind::CREATE, sizeof(CreateArgs), 0, create_validate,
     create_apply);
  op(kOpGet, "Account.get", OpKind::READ, 0, sizeof(AccountV1), read_validate, get_apply);
  op(kOpDeposit, "Account.deposit", OpKind::MUTATE, sizeof(AmountArgs), sizeof(BalanceResult),
     deposit_validate, deposit_apply, /*keeps_index_key=*/true);
  op(kOpWithdraw, "Account.withdraw", OpKind::MUTATE, sizeof(AmountArgs), sizeof(BalanceResult),
     withdraw_validate, withdraw_apply, /*keeps_index_key=*/true);
  op(kOpSetEmail, "Account.set_email", OpKind::MUTATE, sizeof(SetEmailArgs), 0,
     set_email_validate, set_email_apply);
  op(kOpGetBalance, "Account.get_balance", OpKind::READ, 0, sizeof(BalanceResult),
     read_validate, get_balance_apply);
  op(kOpDelete, "Account.delete", OpKind::DELETE, 0, 0, delete_validate, nullptr);
  op(kOpTransfer, "Account.transfer", OpKind::MUTATE, sizeof(TransferArgs), sizeof(BalanceResult),
     transfer_validate, transfer_apply, /*keeps_index_key=*/true);
  op(kOpXferCredit, "Account.xfer_credit", OpKind::MUTATE, sizeof(CreditArgs), 0, credit_validate,
     credit_apply, /*keeps_index_key=*/true);
  op(kOpXferSettle, "Account.xfer_settle", OpKind::MUTATE, sizeof(SettleArgs), 0,
     followup_validate<SettleArgs>, settle_apply, /*keeps_index_key=*/true);
  op(kOpXferForget, "Account.xfer_forget", OpKind::MUTATE, sizeof(SeqArgs), 0,
     followup_validate<SeqArgs>, forget_apply, /*keeps_index_key=*/true,
     /*unlogged=*/true);  // a lost forget is redone: resume queries the sender
  op(kOpXferQuery, "Account.xfer_query", OpKind::MUTATE, sizeof(SeqArgs), 0,
     followup_validate<SeqArgs>, query_apply, /*keeps_index_key=*/true);
  op(kOpXferBounced, "Account.xfer_bounced", OpKind::MUTATE, sizeof(CreditArgs), 0,
     followup_validate<CreditArgs>, bounced_apply, /*keeps_index_key=*/true);

  IndexDescriptor idx;
  idx.index_id = kEmailIndex;
  idx.type_id = kTypeId;
  idx.name = "Account.email";
  idx.extract = email_key;
  r.add_index(idx);
}

}  // namespace snc::account
