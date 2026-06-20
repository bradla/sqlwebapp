-- account.sql - a page that requires a valid session.
--
-- The guard runs first: `redirect` is a header component, so if there is no
-- valid session it sends the browser to login.sql before any body is rendered.
-- The session token comes from the cookie via the $cookie_session parameter.

SELECT 'redirect' AS component, 'login.sql' AS link
WHERE NOT EXISTS (
    SELECT 1 FROM sessions
    WHERE token = $cookie_session AND expires > now()   -- SQLite: datetime('now')
);

-- Past the guard: a valid session exists.
SELECT 'shell' AS component, 'My account' AS title;

SELECT 'text' AS component,
       'Signed in as ' || (
           SELECT username FROM users u
           JOIN sessions s ON s.user_id = u.id
           WHERE s.token = $cookie_session
       ) AS title,
       'This page is only visible with a valid session cookie.' AS contents;

SELECT 'list' AS component, 'Account' AS title;
SELECT 'Log out' AS title, 'End your session' AS description, 'logout.sql' AS link;
