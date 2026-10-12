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

bool check(const u8* payload, u32 len) {
  if (len != sizeof(AccountV1)) return false;
  const auto* a = reinterpret_cast<const AccountV1*>(payload);
  return a->email_len <= sizeof(a->email) && a->name_len <= sizeof(a->name);
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
  if (len != sizeof(AccountV1)) return 0;
  const auto* a = reinterpret_cast<const AccountV1*>(payload);
  u32 n = std::min<u32>(a->email_len, sizeof(a->email));
  std::memcpy(key_out, a->email, n);
  return n;
}

// delete: only empty accounts
Status delete_validate(OpCtx& c, const ObjectHeader& h, ByteView) {
  return acct(h)->balance_cents == 0 ? Status::OK : c.reject(kAppNotEmpty);
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
  t.max_size = sizeof(AccountV1);
  t.check = check;
  t.create_op = kOpCreate;
  t.delete_op = kOpDelete;
  r.add_type(t);

  auto op = [&](OpID id, const char* name, OpKind kind, u32 max_args, u32 max_result,
                Status (*v)(OpCtx&, const ObjectHeader&, ByteView), void (*a)(OpCtx&, ByteView),
                bool keeps_index_key = false) {
    OpDescriptor d;
    d.keeps_index_key = keeps_index_key;
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

  IndexDescriptor idx;
  idx.index_id = kEmailIndex;
  idx.type_id = kTypeId;
  idx.name = "Account.email";
  idx.extract = email_key;
  r.add_index(idx);
}

}  // namespace snc::account
