-- DELETE /api/profiles/:username/follow (auth required).
SET viewer = sqlpage.require_user_id();

WITH u AS (SELECT id, username, bio, image FROM users WHERE username = $username),
     f AS (DELETE FROM follows fo
            USING u
            WHERE fo.follower_id = $viewer::bigint AND fo.followee_id = u.id)
SELECT 'json' AS component,
       json_build_object('profile', json_build_object(
           'username', u.username, 'bio', u.bio, 'image', u.image, 'following', false)) AS contents
  FROM u;

SELECT 'json' AS component, 404 AS status, '{"errors":{"body":["profile not found"]}}' AS contents;
