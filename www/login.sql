-- login.sql - authenticate a user and start a session.  [PostgreSQL]
--
-- SQLite variants of the non-portable bits:
--   gen_random_uuid()::text       -> lower(hex(randomblob(16)))
--   now() + interval '7 days'     -> datetime('now', '+7 days')
-- (and the data-modifying CTE in step 1/2 becomes two statements ordered by rowid).
--
-- Schema (PostgreSQL):
--   CREATE TABLE users(id serial PRIMARY KEY, username text UNIQUE, password text);
--   CREATE TABLE sessions(token text PRIMARY KEY, user_id integer, expires timestamptz);
--
-- SECURITY: passwords are compared in plaintext so the demo runs without a
-- crypto primitive. Do NOT ship this — see the README "Passwords" note
-- (pgcrypto bcrypt on PostgreSQL).

-- 1+2. For valid credentials, create a session and set its cookie atomically:
--      the data-modifying CTE's RETURNING token feeds the Set-Cookie header.
WITH new_session AS (
    INSERT INTO sessions(token, user_id, expires)
    SELECT gen_random_uuid()::text, id, now() + interval '7 days'
    FROM users
    WHERE username = $username AND password = $password
    RETURNING token
)
SELECT 'http_header' AS component,
       'session=' || token || '; Path=/; HttpOnly; SameSite=Lax; Max-Age=604800'
       AS "Set-Cookie"
FROM new_session;

-- 3. On success, redirect into the app.
SELECT 'redirect' AS component, 'account.sql' AS link
WHERE EXISTS (SELECT 1 FROM users WHERE username = $username AND password = $password);

-- 4. Otherwise render the login form (with an error after a failed attempt).
SELECT 'shell' AS component, 'Sign in' AS title;

SELECT 'text' AS component, 'Invalid username or password.' AS contents
WHERE coalesce($username, '') <> ''   -- a login was attempted (and the '' pins the param type)
  AND NOT EXISTS (SELECT 1 FROM users WHERE username = $username AND password = $password);

SELECT 'form' AS component, 'login.sql' AS action, 'post' AS method, 'Sign in' AS title;
SELECT 'username' AS name, 'Username' AS label;
SELECT 'password' AS name, 'Password' AS label, 'password' AS type;
