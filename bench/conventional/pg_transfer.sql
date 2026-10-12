\set a random(1, 1000000)
\set b random(1, 1000000)
UPDATE accounts SET balance = balance + CASE WHEN id = :a THEN -1 ELSE 1 END, op_count = op_count + 1, updated_ns = :a WHERE id IN (:a, :b);
