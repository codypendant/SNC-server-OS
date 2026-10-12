\set id0 random(1, 1000000)
\set r0 random(1, 100)
\set id1 random(1, 1000000)
\set r1 random(1, 100)
\set id2 random(1, 1000000)
\set r2 random(1, 100)
\set id3 random(1, 1000000)
\set r3 random(1, 100)
\set id4 random(1, 1000000)
\set r4 random(1, 100)
\set id5 random(1, 1000000)
\set r5 random(1, 100)
\set id6 random(1, 1000000)
\set r6 random(1, 100)
\set id7 random(1, 1000000)
\set r7 random(1, 100)
\set id8 random(1, 1000000)
\set r8 random(1, 100)
\set id9 random(1, 1000000)
\set r9 random(1, 100)
\set id10 random(1, 1000000)
\set r10 random(1, 100)
\set id11 random(1, 1000000)
\set r11 random(1, 100)
\set id12 random(1, 1000000)
\set r12 random(1, 100)
\set id13 random(1, 1000000)
\set r13 random(1, 100)
\set id14 random(1, 1000000)
\set r14 random(1, 100)
\set id15 random(1, 1000000)
\set r15 random(1, 100)
\startpipeline
\if :r0 <= :reads
SELECT balance, op_count FROM accounts WHERE id = :id0;
\else
UPDATE accounts SET balance = balance + 1, op_count = op_count + 1, updated_ns = :id0 WHERE id = :id0 RETURNING balance, op_count;
\endif
\if :r1 <= :reads
SELECT balance, op_count FROM accounts WHERE id = :id1;
\else
UPDATE accounts SET balance = balance + 1, op_count = op_count + 1, updated_ns = :id1 WHERE id = :id1 RETURNING balance, op_count;
\endif
\if :r2 <= :reads
SELECT balance, op_count FROM accounts WHERE id = :id2;
\else
UPDATE accounts SET balance = balance + 1, op_count = op_count + 1, updated_ns = :id2 WHERE id = :id2 RETURNING balance, op_count;
\endif
\if :r3 <= :reads
SELECT balance, op_count FROM accounts WHERE id = :id3;
\else
UPDATE accounts SET balance = balance + 1, op_count = op_count + 1, updated_ns = :id3 WHERE id = :id3 RETURNING balance, op_count;
\endif
\if :r4 <= :reads
SELECT balance, op_count FROM accounts WHERE id = :id4;
\else
UPDATE accounts SET balance = balance + 1, op_count = op_count + 1, updated_ns = :id4 WHERE id = :id4 RETURNING balance, op_count;
\endif
\if :r5 <= :reads
SELECT balance, op_count FROM accounts WHERE id = :id5;
\else
UPDATE accounts SET balance = balance + 1, op_count = op_count + 1, updated_ns = :id5 WHERE id = :id5 RETURNING balance, op_count;
\endif
\if :r6 <= :reads
SELECT balance, op_count FROM accounts WHERE id = :id6;
\else
UPDATE accounts SET balance = balance + 1, op_count = op_count + 1, updated_ns = :id6 WHERE id = :id6 RETURNING balance, op_count;
\endif
\if :r7 <= :reads
SELECT balance, op_count FROM accounts WHERE id = :id7;
\else
UPDATE accounts SET balance = balance + 1, op_count = op_count + 1, updated_ns = :id7 WHERE id = :id7 RETURNING balance, op_count;
\endif
\if :r8 <= :reads
SELECT balance, op_count FROM accounts WHERE id = :id8;
\else
UPDATE accounts SET balance = balance + 1, op_count = op_count + 1, updated_ns = :id8 WHERE id = :id8 RETURNING balance, op_count;
\endif
\if :r9 <= :reads
SELECT balance, op_count FROM accounts WHERE id = :id9;
\else
UPDATE accounts SET balance = balance + 1, op_count = op_count + 1, updated_ns = :id9 WHERE id = :id9 RETURNING balance, op_count;
\endif
\if :r10 <= :reads
SELECT balance, op_count FROM accounts WHERE id = :id10;
\else
UPDATE accounts SET balance = balance + 1, op_count = op_count + 1, updated_ns = :id10 WHERE id = :id10 RETURNING balance, op_count;
\endif
\if :r11 <= :reads
SELECT balance, op_count FROM accounts WHERE id = :id11;
\else
UPDATE accounts SET balance = balance + 1, op_count = op_count + 1, updated_ns = :id11 WHERE id = :id11 RETURNING balance, op_count;
\endif
\if :r12 <= :reads
SELECT balance, op_count FROM accounts WHERE id = :id12;
\else
UPDATE accounts SET balance = balance + 1, op_count = op_count + 1, updated_ns = :id12 WHERE id = :id12 RETURNING balance, op_count;
\endif
\if :r13 <= :reads
SELECT balance, op_count FROM accounts WHERE id = :id13;
\else
UPDATE accounts SET balance = balance + 1, op_count = op_count + 1, updated_ns = :id13 WHERE id = :id13 RETURNING balance, op_count;
\endif
\if :r14 <= :reads
SELECT balance, op_count FROM accounts WHERE id = :id14;
\else
UPDATE accounts SET balance = balance + 1, op_count = op_count + 1, updated_ns = :id14 WHERE id = :id14 RETURNING balance, op_count;
\endif
\if :r15 <= :reads
SELECT balance, op_count FROM accounts WHERE id = :id15;
\else
UPDATE accounts SET balance = balance + 1, op_count = op_count + 1, updated_ns = :id15 WHERE id = :id15 RETURNING balance, op_count;
\endif
\endpipeline
