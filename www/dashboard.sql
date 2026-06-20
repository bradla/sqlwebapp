-- dashboard.sql - showcases the chart component and Tabler-styled components.
-- Self-contained: uses literal rows (UNION ALL), so it needs no tables and
-- runs on both SQLite and PostgreSQL.
--
--   /dashboard.sql?name=Ada

SELECT 'shell' AS component, 'csqlpage dashboard' AS title;

SELECT 'text' AS component,
       'Welcome, ' || COALESCE($name, 'guest') AS title,
       'A demo page built from SQL: charts via ApexCharts, layout via Tabler.' AS contents;

-- Bar chart -------------------------------------------------------------
SELECT 'chart' AS component, 'Monthly revenue' AS title, 'bar' AS type;
SELECT 'Jan' AS x, 120 AS y
UNION ALL SELECT 'Feb', 180
UNION ALL SELECT 'Mar', 90
UNION ALL SELECT 'Apr', 240
UNION ALL SELECT 'May', 200
UNION ALL SELECT 'Jun', 310;

-- Line chart ------------------------------------------------------------
SELECT 'chart' AS component, 'Daily active users' AS title, 'area' AS type;
SELECT 'Mon' AS x, 42 AS y
UNION ALL SELECT 'Tue', 55
UNION ALL SELECT 'Wed', 48
UNION ALL SELECT 'Thu', 67
UNION ALL SELECT 'Fri', 80
UNION ALL SELECT 'Sat', 35
UNION ALL SELECT 'Sun', 30;

-- Donut chart -----------------------------------------------------------
SELECT 'chart' AS component, 'Traffic by platform' AS title, 'donut' AS type;
SELECT 'Linux' AS x, 70 AS y
UNION ALL SELECT 'macOS', 20
UNION ALL SELECT 'Windows', 10;

-- Summary cards ---------------------------------------------------------
SELECT 'card' AS component, 'At a glance' AS title;
SELECT 'Revenue'  AS title, '$1,140 this half'      AS description, '?'            AS link
UNION ALL SELECT 'Users', '367 active this week',     '?'
UNION ALL SELECT 'Uptime', '99.98% last 30 days',     '?';

-- Data table ------------------------------------------------------------
SELECT 'table' AS component, 'Recent signups' AS title;
SELECT 1 AS id, 'Ada Lovelace'   AS name, 'ada@example.com'   AS email
UNION ALL SELECT 2, 'Linus Torvalds', 'linus@example.com'
UNION ALL SELECT 3, 'Grace Hopper',   'grace@example.com';

-- Navigation list -------------------------------------------------------
SELECT 'list' AS component, 'Explore' AS title;
SELECT 'Home'        AS title, 'Back to the index page' AS description, 'index.sql'             AS link
UNION ALL SELECT 'Greeting', 'Personalized hello',      'dashboard.sql?name=Grace';
