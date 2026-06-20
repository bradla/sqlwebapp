-- logout.sql - destroy the session and clear the cookie.

DELETE FROM sessions WHERE token = $cookie_session;

-- Expire the cookie (Max-Age=0) and bounce back to the login page.
SELECT 'http_header' AS component,
       'session=; Path=/; HttpOnly; Max-Age=0' AS "Set-Cookie";

SELECT 'redirect' AS component, 'login.sql' AS link;
