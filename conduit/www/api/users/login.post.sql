-- POST /api/users/login. Body {"user": {"email", "password"}}.
SET body = sqlpage.request_json();

SELECT 'json' AS component, 422 AS status,
       '{"errors":{"body":["email and password must be non-empty strings"]}}' AS contents
WHERE NOT coalesce((
    SELECT json_typeof(u -> 'email')    = 'string' AND u ->> 'email'    <> ''
       AND json_typeof(u -> 'password') = 'string' AND u ->> 'password' <> ''
      FROM (SELECT $body::json -> 'user' AS u) s), false);

SET (user_id, password_hash, password, user_json) = (
    SELECT u.id, u.password_hash, $body::json -> 'user' ->> 'password',
           json_build_object('email', u.email, 'username', u.username, 'bio', u.bio, 'image', u.image)
      FROM users u
     WHERE u.email = $body::json -> 'user' ->> 'email'
);
SET ok = sqlpage.verify_password($password, $password_hash);
SET token = sqlpage.jwt($user_id);

SELECT 'json' AS component,
       json_build_object('user', $user_json::jsonb || jsonb_build_object('token', $token::text)) AS contents
WHERE $ok::boolean;

SELECT 'json' AS component, 401 AS status,
       '{"errors":{"body":["invalid email or password"]}}' AS contents;
