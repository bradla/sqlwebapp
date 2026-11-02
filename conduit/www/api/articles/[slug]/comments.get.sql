-- GET /api/articles/:slug/comments (auth optional): the newest 100 comments.
SET viewer = sqlpage.user_id();

SELECT 'json' AS component,
       json_build_object('comments', coalesce((
           SELECT json_agg(json_build_object(
                      'id', c.id,
                      'createdAt', to_char(c.created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.MS"Z"'),
                      'updatedAt', to_char(c.updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.MS"Z"'),
                      'body', c.body,
                      'author', json_build_object(
                          'username', u.username, 'bio', u.bio, 'image', u.image,
                          'following', EXISTS (SELECT 1 FROM follows fo
                                                WHERE fo.follower_id = $viewer::bigint
                                                  AND fo.followee_id = c.author_id)))
                  ORDER BY c.created_at DESC, c.id DESC)
             FROM (SELECT * FROM comments c
                    WHERE c.article_id = a.id
                    ORDER BY c.created_at DESC, c.id DESC
                    LIMIT 100) c
             JOIN users u ON u.id = c.author_id), '[]'::json)) AS contents
  FROM articles a
 WHERE a.slug = $slug;

SELECT 'json' AS component, 404 AS status, '{"errors":{"body":["article not found"]}}' AS contents;
