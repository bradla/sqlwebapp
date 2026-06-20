-- schema.sql - tables for the authentication example (PostgreSQL).
--
--   psql 'postgresql://blog:blog1234@localhost/mydb' -f deploy/schema.sql
--
-- SQLite equivalent is noted in comments; for SQLite run with:
--   sqlite3 app.db < deploy/schema.sql   (after applying the noted swaps)

CREATE TABLE IF NOT EXISTS users (
    id       serial PRIMARY KEY,        -- SQLite: id INTEGER PRIMARY KEY
    username text UNIQUE NOT NULL,
    password text NOT NULL              -- plaintext in the demo; hash in production
);

CREATE TABLE IF NOT EXISTS sessions (
    token   text PRIMARY KEY,
    user_id integer NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    expires timestamptz NOT NULL        -- SQLite: expires TEXT
);

CREATE INDEX IF NOT EXISTS sessions_user_id_idx ON sessions(user_id);

-- Seed a demo user (no-op if it already exists).
INSERT INTO users(username, password) VALUES ('ada', 'secret')
ON CONFLICT (username) DO NOTHING;     -- SQLite: INSERT OR IGNORE INTO users(username,password) VALUES ('ada','secret');
