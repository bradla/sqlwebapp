-- GET /api/user: the current user, with the token the request presented.
SET viewer = sqlpage.require_user_id();

SELECT 'json' AS component,
       json_build_object('user', json_build_object(
           'email', u.email, 'token', sqlpage.token()::text,
           'username', u.username, 'bio', u.bio, 'image', u.image)) AS contents
  FROM users u
 WHERE u.id = $viewer::bigint;

SELECT 'json' AS component, 401 AS status, '{"errors":{"body":["unknown user"]}}' AS contents;
