\set a0 random(1, 1000000)
\set b0 random(1, 1000000)
\set a1 random(1, 1000000)
\set b1 random(1, 1000000)
\set a2 random(1, 1000000)
\set b2 random(1, 1000000)
\set a3 random(1, 1000000)
\set b3 random(1, 1000000)
\set a4 random(1, 1000000)
\set b4 random(1, 1000000)
\set a5 random(1, 1000000)
\set b5 random(1, 1000000)
\set a6 random(1, 1000000)
\set b6 random(1, 1000000)
\set a7 random(1, 1000000)
\set b7 random(1, 1000000)
\set a8 random(1, 1000000)
\set b8 random(1, 1000000)
\set a9 random(1, 1000000)
\set b9 random(1, 1000000)
\set a10 random(1, 1000000)
\set b10 random(1, 1000000)
\set a11 random(1, 1000000)
\set b11 random(1, 1000000)
\set a12 random(1, 1000000)
\set b12 random(1, 1000000)
\set a13 random(1, 1000000)
\set b13 random(1, 1000000)
\set a14 random(1, 1000000)
\set b14 random(1, 1000000)
\set a15 random(1, 1000000)
\set b15 random(1, 1000000)
\startpipeline
UPDATE accounts SET balance = balance + CASE WHEN id = :a0 THEN -1 ELSE 1 END, op_count = op_count + 1, updated_ns = :a0 WHERE id IN (:a0, :b0);
UPDATE accounts SET balance = balance + CASE WHEN id = :a1 THEN -1 ELSE 1 END, op_count = op_count + 1, updated_ns = :a1 WHERE id IN (:a1, :b1);
UPDATE accounts SET balance = balance + CASE WHEN id = :a2 THEN -1 ELSE 1 END, op_count = op_count + 1, updated_ns = :a2 WHERE id IN (:a2, :b2);
UPDATE accounts SET balance = balance + CASE WHEN id = :a3 THEN -1 ELSE 1 END, op_count = op_count + 1, updated_ns = :a3 WHERE id IN (:a3, :b3);
UPDATE accounts SET balance = balance + CASE WHEN id = :a4 THEN -1 ELSE 1 END, op_count = op_count + 1, updated_ns = :a4 WHERE id IN (:a4, :b4);
UPDATE accounts SET balance = balance + CASE WHEN id = :a5 THEN -1 ELSE 1 END, op_count = op_count + 1, updated_ns = :a5 WHERE id IN (:a5, :b5);
UPDATE accounts SET balance = balance + CASE WHEN id = :a6 THEN -1 ELSE 1 END, op_count = op_count + 1, updated_ns = :a6 WHERE id IN (:a6, :b6);
UPDATE accounts SET balance = balance + CASE WHEN id = :a7 THEN -1 ELSE 1 END, op_count = op_count + 1, updated_ns = :a7 WHERE id IN (:a7, :b7);
UPDATE accounts SET balance = balance + CASE WHEN id = :a8 THEN -1 ELSE 1 END, op_count = op_count + 1, updated_ns = :a8 WHERE id IN (:a8, :b8);
UPDATE accounts SET balance = balance + CASE WHEN id = :a9 THEN -1 ELSE 1 END, op_count = op_count + 1, updated_ns = :a9 WHERE id IN (:a9, :b9);
UPDATE accounts SET balance = balance + CASE WHEN id = :a10 THEN -1 ELSE 1 END, op_count = op_count + 1, updated_ns = :a10 WHERE id IN (:a10, :b10);
UPDATE accounts SET balance = balance + CASE WHEN id = :a11 THEN -1 ELSE 1 END, op_count = op_count + 1, updated_ns = :a11 WHERE id IN (:a11, :b11);
UPDATE accounts SET balance = balance + CASE WHEN id = :a12 THEN -1 ELSE 1 END, op_count = op_count + 1, updated_ns = :a12 WHERE id IN (:a12, :b12);
UPDATE accounts SET balance = balance + CASE WHEN id = :a13 THEN -1 ELSE 1 END, op_count = op_count + 1, updated_ns = :a13 WHERE id IN (:a13, :b13);
UPDATE accounts SET balance = balance + CASE WHEN id = :a14 THEN -1 ELSE 1 END, op_count = op_count + 1, updated_ns = :a14 WHERE id IN (:a14, :b14);
UPDATE accounts SET balance = balance + CASE WHEN id = :a15 THEN -1 ELSE 1 END, op_count = op_count + 1, updated_ns = :a15 WHERE id IN (:a15, :b15);
\endpipeline
