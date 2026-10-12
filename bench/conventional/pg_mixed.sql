\set id random(1, 1000000)
\set r random(1, 100)
\if :r <= :reads
SELECT balance, op_count FROM accounts WHERE id = :id;
\else
UPDATE accounts SET balance = balance + 1, op_count = op_count + 1, updated_ns = :id WHERE id = :id RETURNING balance, op_count;
\endif
