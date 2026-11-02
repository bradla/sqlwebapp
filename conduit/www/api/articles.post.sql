-- POST /api/articles (auth required). Body {"article": {"title", "description", "body", "tagList"?}}.
SET viewer = sqlpage.require_user_id();
SET body = sqlpage.request_json();

-- 422 unless title, description and body are non-empty strings and tagList,
-- when given, is an array of non-empty strings.
SELECT 'json' AS component, 422 AS status,
       '{"errors":{"body":["title, description and body must be non-empty strings; tagList an array of non-empty strings"]}}' AS contents
WHERE NOT coalesce((
    SELECT json_typeof(a -> 'title')       = 'string' AND a ->> 'title'       <> ''
       AND json_typeof(a -> 'description') = 'string' AND a ->> 'description' <> ''
       AND json_typeof(a -> 'body')        = 'string' AND a ->> 'body'        <> ''
       AND CASE WHEN json_typeof(a -> 'tagList') = 'array'
                THEN NOT EXISTS (SELECT 1 FROM json_array_elements(a -> 'tagList') e
                                  WHERE json_typeof(e) <> 'string' OR e #>> '{}' = '')
                ELSE coalesce(json_typeof(a -> 'tagList'), 'null') = 'null'
           END
      FROM (SELECT $body::json -> 'article' AS a) s), false);

BEGIN;

SET (article_id) = (
    INSERT INTO articles (id, slug, author_id, title, description, body)
    SELECT n.id,
           trim(both '-' from lower(regexp_replace(b.a ->> 'title', '[^A-Za-z0-9]+', '-', 'g'))) || '-' || n.id,
           $viewer::bigint, b.a ->> 'title', b.a ->> 'description', b.a ->> 'body'
      FROM (SELECT nextval(pg_get_serial_sequence('articles', 'id')) AS id) n,
           (SELECT $body::json -> 'article' AS a) b
    RETURNING id
);

SET tag_list = ARRAY(SELECT json_array_elements_text(
                   CASE WHEN json_typeof($body::json -> 'article' -> 'tagList') = 'array'
                        THEN $body::json -> 'article' -> 'tagList' END));

INSERT INTO tags (name) SELECT DISTINCT unnest($tag_list::text[]) ON CONFLICT (name) DO NOTHING;

INSERT INTO article_tags (article_id, tag_id)
SELECT $article_id::bigint, t.id FROM tags t WHERE t.name = ANY($tag_list::text[]);

COMMIT;

SELECT 'json' AS component, 201 AS status,
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
 WHERE a.id = $article_id::bigint;
