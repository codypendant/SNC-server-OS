// Garnet with the B1 account commands (docs/DIRECTION_COMPARISON_PLAN.md 4.2).
// An account is a 200-byte string value whose first 8 bytes are the balance
// (little-endian cents). Reads are plain GET, creates plain SET.
//   ACCT.DEP key cents        add cents in place; replies the new balance
//   ACCT.XFER src dst cents   transaction on both keys; -ERR NOFUNDS if short
using System.Buffers.Binary;
using Garnet;
using Garnet.common;
using Garnet.server;
using Tsavorite.core;

using var server = new GarnetServer(args);
server.Register.NewCommand("ACCT.DEP", CommandType.ReadModifyWrite, new Deposit(),
    new RespCommandsInfo { Name = "ACCT.DEP", Arity = 3, FirstKey = 1, LastKey = 1, Step = 1,
                           Flags = RespCommandFlags.Write, AclCategories = RespAclCategories.Write });
server.Register.NewTransactionProc("ACCT.XFER", () => new Transfer(),
    new RespCommandsInfo { Name = "ACCT.XFER", Arity = 4, FirstKey = 1, LastKey = 2, Step = 1,
                           Flags = RespCommandFlags.Write, AclCategories = RespAclCategories.Write });
server.Start();
Thread.Sleep(Timeout.Infinite);

sealed class Deposit : CustomRawStringFunctions
{
    static long Cents(ref StringInput input) => long.Parse(System.Text.Encoding.ASCII.GetString(GetFirstArg(ref input)));

    public override bool NeedInitialUpdate(scoped ReadOnlySpan<byte> key, ref StringInput input, ref RespMemoryWriter writer) => false;
    public override int GetInitialLength(ref StringInput input) => throw new InvalidOperationException();
    public override bool InitialUpdater(ReadOnlySpan<byte> key, ref StringInput input, Span<byte> value, ref RespMemoryWriter writer, ref RMWInfo rmwInfo)
        => throw new InvalidOperationException();
    public override int GetLength(ReadOnlySpan<byte> value, ref StringInput input) => value.Length;

    public override bool InPlaceUpdater(ReadOnlySpan<byte> key, ref StringInput input, Span<byte> value, ref int valueLength, ref RespMemoryWriter writer, ref RMWInfo rmwInfo)
    {
        long b = BinaryPrimitives.ReadInt64LittleEndian(value) + Cents(ref input);
        BinaryPrimitives.WriteInt64LittleEndian(value, b);
        writer.WriteInt64(b);
        return true;
    }

    public override bool CopyUpdater(ReadOnlySpan<byte> key, ref StringInput input, ReadOnlySpan<byte> oldValue, Span<byte> newValue, ref RespMemoryWriter writer, ref RMWInfo rmwInfo)
    {
        oldValue.CopyTo(newValue);
        long b = BinaryPrimitives.ReadInt64LittleEndian(oldValue) + Cents(ref input);
        BinaryPrimitives.WriteInt64LittleEndian(newValue, b);
        writer.WriteInt64(b);
        return true;
    }

    public override bool Reader(ReadOnlySpan<byte> key, ref StringInput input, ReadOnlySpan<byte> value, ref RespMemoryWriter writer, ref ReadInfo readInfo)
        => throw new InvalidOperationException();
}

sealed class Transfer : CustomTransactionProcedure
{
    public override bool Prepare<TGarnetReadApi>(TGarnetReadApi api, ref CustomProcedureInput procInput)
    {
        int offset = 0;
        AddKey(GetNextArg(ref procInput, ref offset), LockType.Exclusive, StoreType.Main);
        AddKey(GetNextArg(ref procInput, ref offset), LockType.Exclusive, StoreType.Main);
        return true;
    }

    public override unsafe void Main<TGarnetApi>(TGarnetApi api, ref CustomProcedureInput procInput, ref MemoryResult<byte> output)
    {
        int offset = 0;
        var src = GetNextArg(ref procInput, ref offset);
        var dst = GetNextArg(ref procInput, ref offset);
        long cents = long.Parse(System.Text.Encoding.ASCII.GetString(GetNextArg(ref procInput, ref offset).ReadOnlySpan));
        if (api.GET(src, out PinnedSpanByte s) != GarnetStatus.OK || api.GET(dst, out PinnedSpanByte d) != GarnetStatus.OK)
        {
            WriteError(ref output, "ERR NOACCOUNT");
            return;
        }
        long sb = BinaryPrimitives.ReadInt64LittleEndian(s.ReadOnlySpan);
        if (sb < cents)
        {
            WriteError(ref output, "ERR NOFUNDS");
            return;
        }
        Span<byte> a = stackalloc byte[s.Length];
        Span<byte> b = stackalloc byte[d.Length];
        s.ReadOnlySpan.CopyTo(a);
        d.ReadOnlySpan.CopyTo(b);
        BinaryPrimitives.WriteInt64LittleEndian(a, sb - cents);
        BinaryPrimitives.WriteInt64LittleEndian(b, BinaryPrimitives.ReadInt64LittleEndian(b) + cents);
        api.SET(src, PinnedSpanByte.FromPinnedSpan(a));
        api.SET(dst, PinnedSpanByte.FromPinnedSpan(b));
        WriteSimpleString(ref output, "OK");
    }
}
