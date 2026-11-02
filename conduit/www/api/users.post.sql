-- POST /api/users: register. Body {"user": {"username", "email", "password"}}.
SET body = sqlpage.request_json();

-- 422 unless all three are non-empty strings (bcrypt reads at most 72 bytes).
SELECT 'json' AS component, 422 AS status,
       '{"errors":{"body":["username, email and password must be non-empty strings"]}}' AS contents
WHERE NOT coalesce((
    SELECT json_typeof(u -> 'username') = 'string' AND u ->> 'username' <> ''
       AND json_typeof(u -> 'email')    = 'string' AND u ->> 'email'    <> ''
       AND json_typeof(u -> 'password') = 'string' AND u ->> 'password' <> ''
       AND octet_length(u ->> 'password') <= 72
      FROM (SELECT $body::json -> 'user' AS u) s), false);

SET password = $body::json -> 'user' ->> 'password';
SET password_hash = sqlpage.hash_password($password);

-- A taken username or email is a unique violation, answered with 422.
SET (user_id, user_json) = (
    INSERT INTO users (username, email, password_hash)
    VALUES ($body::json -> 'user' ->> 'username', $body::json -> 'user' ->> 'email', $password_hash)
    RETURNING id, json_build_object('email', email, 'username', username, 'bio', bio, 'image', image)
);
SET token = sqlpage.jwt($user_id);

SELECT 'json' AS component, 201 AS status,
       json_build_object('user', $user_json::jsonb || jsonb_build_object('token', $token::text)) AS contents;
