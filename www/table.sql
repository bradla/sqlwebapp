-- table.sql - display rows from an existing table in an interactive grid.
-- Reads the `users` table created by deploy/schema.sql.  Open it at /table.sql

SELECT 'shell' AS component, '2019' AS title;

SELECT 'text' AS component, 'User directory' AS title,
       'Click a column header to sort, type to filter, page through at the bottom.' AS contents;

-- sort + search + per_page make it a live grid (simple-datatables).
SELECT 'table' AS component, '2019' AS title,
       TRUE AS sort, TRUE AS search, 10 AS per_page;

-- Select only safe columns from the real table (never expose `password`).
SELECT id, username FROM users ORDER BY id;
