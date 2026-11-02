-- GET /api/profiles/:username (auth optional).
SET viewer = sqlpage.user_id();

SELECT 'json' AS component,
       json_build_object('profile', json_build_object(
           'username', u.username, 'bio', u.bio, 'image', u.image,
           'following', EXISTS (SELECT 1 FROM follows fo
                                 WHERE fo.follower_id = $viewer::bigint AND fo.followee_id = u.id))) AS contents
  FROM users u
 WHERE u.username = $username;

SELECT 'json' AS component, 404 AS status, '{"errors":{"body":["profile not found"]}}' AS contents;
