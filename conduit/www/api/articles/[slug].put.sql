-- PUT /api/articles/:slug (auth required, author only). Body {"article": {...}} with any of
-- title, description, body; absent or null fields are unchanged. A new title renames the slug.
SET viewer = sqlpage.require_user_id();
SET body = sqlpage.request_json();

SELECT 'json' AS component, 422 AS status,
       '{"errors":{"body":["title, description and body must be non-empty strings"]}}' AS contents
WHERE NOT coalesce((
    SELECT json_typeof(a) = 'object'
       AND coalesce(json_typeof(a -> 'title'),       'null') IN ('null', 'string') AND coalesce(a ->> 'title', 'x')       <> ''
       AND coalesce(json_typeof(a -> 'description'), 'null') IN ('null', 'string') AND coalesce(a ->> 'description', 'x') <> ''
       AND coalesce(json_typeof(a -> 'body'),        'null') IN ('null', 'string') AND coalesce(a ->> 'body', 'x')        <> ''
      FROM (SELECT $body::json -> 'article' AS a) s), false);

-- 404 for an unknown slug, 403 for someone else's article.
SELECT 'json' AS component,
       CASE WHEN a.id IS NULL THEN 404 ELSE 403 END AS status,
       CASE WHEN a.id IS NULL THEN '{"errors":{"body":["article not found"]}}'
            ELSE '{"errors":{"body":["only the author can edit this article"]}}' END AS contents
  FROM (SELECT 1) one LEFT JOIN articles a ON a.slug = $slug
 WHERE a.id IS NULL OR a.author_id <> $viewer::bigint;

WITH a AS (
    UPDATE articles
       SET title       = COALESCE($body::json -> 'article' ->> 'title', title),
           description = COALESCE($body::json -> 'article' ->> 'description', description),
           body        = COALESCE($body::json -> 'article' ->> 'body', body),
           slug        = CASE WHEN $body::json -> 'article' ->> 'title' IS NULL THEN slug
                              ELSE trim(both '-' from lower(regexp_replace($body::json -> 'article' ->> 'title',
                                                                            '[^A-Za-z0-9]+', '-', 'g'))) || '-' || id END,
           updated_at  = now()
     WHERE slug = $slug AND author_id = $viewer::bigint
    RETURNING *
)
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
  FROM a JOIN users u ON u.id = a.author_id;

-- Deleted between the check and the update.
SELECT 'json' AS component, 404 AS status, '{"errors":{"body":["article not found"]}}' AS contents;
