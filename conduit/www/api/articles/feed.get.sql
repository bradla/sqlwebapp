-- GET /api/articles/feed?limit=&offset= (auth required): articles by the users the viewer follows.
SET viewer = sqlpage.require_user_id();

SELECT 'json' AS component, 422 AS status,
       '{"errors":{"body":["limit must be 1..100 and offset >= 0"]}}' AS contents
WHERE NOT (coalesce($limit::text, '20') ~ '^0*([1-9][0-9]?|100)$'
       AND coalesce($offset::text, '0') ~ '^0*[0-9]{1,18}$');

SELECT 'json' AS component,
       json_build_object(
           'articles', coalesce((
               SELECT json_agg(json_build_object(
                          'slug', a.slug, 'title', a.title, 'description', a.description,
                          'tagList', ARRAY(SELECT t.name FROM article_tags at JOIN tags t ON t.id = at.tag_id
                                            WHERE at.article_id = a.id ORDER BY t.name),
                          'createdAt', to_char(a.created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.MS"Z"'),
                          'updatedAt', to_char(a.updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.MS"Z"'),
                          'favorited', EXISTS (SELECT 1 FROM favorites fa
                                                WHERE fa.article_id = a.id AND fa.user_id = $viewer::bigint),
                          'favoritesCount', (SELECT count(*) FROM favorites fa WHERE fa.article_id = a.id),
                          'author', json_build_object(
                              'username', u.username, 'bio', u.bio, 'image', u.image,
                              'following', EXISTS (SELECT 1 FROM follows fo
                                                    WHERE fo.follower_id = $viewer::bigint
                                                      AND fo.followee_id = a.author_id)))
                      ORDER BY a.id DESC)
                 FROM articles a JOIN users u ON u.id = a.author_id
                WHERE a.id IN (
                    SELECT x.id FROM follows fo CROSS JOIN LATERAL (
                        SELECT a2.id FROM articles a2
                         WHERE a2.author_id = fo.followee_id
                         ORDER BY a2.id DESC
                         LIMIT coalesce($limit::text, '20')::bigint + coalesce($offset::text, '0')::bigint) x
                     WHERE fo.follower_id = $viewer::bigint
                     ORDER BY x.id DESC
                     LIMIT coalesce($limit::text, '20')::int OFFSET coalesce($offset::text, '0')::bigint)
           ), '[]'::json),
           'articlesCount', (SELECT count(*) FROM (
               SELECT 1 FROM articles a
                WHERE a.author_id IN (SELECT fo.followee_id FROM follows fo WHERE fo.follower_id = $viewer::bigint)
                LIMIT 1000) s)) AS contents;
