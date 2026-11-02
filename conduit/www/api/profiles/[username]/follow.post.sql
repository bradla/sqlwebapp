-- POST /api/profiles/:username/follow (auth required).
SET viewer = sqlpage.require_user_id();

WITH u AS (SELECT id, username, bio, image FROM users WHERE username = $username),
     f AS (INSERT INTO follows (follower_id, followee_id)
           SELECT $viewer::bigint, u.id FROM u
           ON CONFLICT DO NOTHING)
SELECT 'json' AS component,
       json_build_object('profile', json_build_object(
           'username', u.username, 'bio', u.bio, 'image', u.image, 'following', true)) AS contents
  FROM u;

SELECT 'json' AS component, 404 AS status, '{"errors":{"body":["profile not found"]}}' AS contents;
