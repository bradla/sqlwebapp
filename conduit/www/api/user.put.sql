-- PUT /api/user. Body {"user": {...}} with any of email, username, password, bio, image;
-- absent or null fields are unchanged.
SET viewer = sqlpage.require_user_id();
SET body = sqlpage.request_json();

-- 422 unless each given field is a string, and email, username and password are non-empty.
SELECT 'json' AS component, 422 AS status,
       '{"errors":{"body":["invalid user fields"]}}' AS contents
WHERE NOT coalesce((
    SELECT json_typeof(u) = 'object'
       AND coalesce(json_typeof(u -> 'email'),    'null') IN ('null', 'string') AND coalesce(u ->> 'email', 'x')    <> ''
       AND coalesce(json_typeof(u -> 'username'), 'null') IN ('null', 'string') AND coalesce(u ->> 'username', 'x') <> ''
       AND coalesce(json_typeof(u -> 'password'), 'null') IN ('null', 'string') AND coalesce(u ->> 'password', 'x') <> ''
       AND coalesce(octet_length(u ->> 'password'), 0) <= 72
       AND coalesce(json_typeof(u -> 'bio'),      'null') IN ('null', 'string')
       AND coalesce(json_typeof(u -> 'image'),    'null') IN ('null', 'string')
      FROM (SELECT $body::json -> 'user' AS u) s), false);

SET password = $body::json -> 'user' ->> 'password';
SET password_hash = sqlpage.hash_password($password);

-- A taken username or email is a unique violation, answered with 422.
WITH u AS (
    UPDATE users
       SET email         = COALESCE($body::json -> 'user' ->> 'email', email),
           username      = COALESCE($body::json -> 'user' ->> 'username', username),
           password_hash = COALESCE($password_hash, password_hash),
           bio           = COALESCE($body::json -> 'user' ->> 'bio', bio),
           image         = COALESCE($body::json -> 'user' ->> 'image', image)
     WHERE id = $viewer::bigint
    RETURNING email, username, bio, image
)
SELECT 'json' AS component,
       json_build_object('user', json_build_object(
           'email', u.email, 'token', sqlpage.token()::text,
           'username', u.username, 'bio', u.bio, 'image', u.image)) AS contents
  FROM u;

SELECT 'json' AS component, 401 AS status, '{"errors":{"body":["unknown user"]}}' AS contents;
