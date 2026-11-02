-- GET /api/articles/:slug (auth optional).
SET viewer = sqlpage.user_id();

SELECT 'json' AS component,
       json_build_object('article', json_build_object(
           'slug', a.slug, 'title', a.title, 'description', a.description, 'body', a.body,
           'tagList', ARRAY(SELECT t.name FROM article_tags at JOIN tags t ON t.id = at.tag_id
                             WHERE at.article_id = a.id ORDER BY t.name),
           'createdAt', to_char(a.created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.MS"Z"'),
           'updatedAt', to_char(a.updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.MS"Z"'),
           'favorited', EXISTS (SELECT 1 FROM favorites f WHERE f.article_id = a.id AND f.user_id = $viewer::bigint),
           'favoritesCount', (SELECT count(*) FROM favorites f WHERE f.article_id = a.id),
           'author', json_build_object(
               'username', u.username, 'bio', u.bio, 'image', u.image,
               'following', EXISTS (SELECT 1 FROM follows fo
                                     WHERE fo.follower_id = $viewer::bigint AND fo.followee_id = a.author_id)))) AS contents
  FROM articles a JOIN users u ON u.id = a.author_id
 WHERE a.slug = $slug;

SELECT 'json' AS component, 404 AS status, '{"errors":{"body":["article not found"]}}' AS contents;
