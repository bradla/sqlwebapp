-- POST /api/articles/:slug/comments (auth required). Body {"comment": {"body"}}.
SET viewer = sqlpage.require_user_id();
SET body = sqlpage.request_json();

SELECT 'json' AS component, 422 AS status,
       '{"errors":{"body":["body must be a non-empty string"]}}' AS contents
WHERE NOT coalesce((
    SELECT json_typeof(c -> 'body') = 'string' AND c ->> 'body' <> ''
      FROM (SELECT $body::json -> 'comment' AS c) s), false);

WITH c AS (
    INSERT INTO comments (article_id, author_id, body)
    SELECT a.id, $viewer::bigint, $body::json -> 'comment' ->> 'body'
      FROM articles a WHERE a.slug = $slug
    RETURNING id, body, created_at, updated_at, author_id
)
SELECT 'json' AS component, 201 AS status,
       json_build_object('comment', json_build_object(
           'id', c.id,
           'createdAt', to_char(c.created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.MS"Z"'),
           'updatedAt', to_char(c.updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.MS"Z"'),
           'body', c.body,
           'author', json_build_object(
               'username', u.username, 'bio', u.bio, 'image', u.image, 'following', false))) AS contents
  FROM c JOIN users u ON u.id = c.author_id;

SELECT 'json' AS component, 404 AS status, '{"errors":{"body":["article not found"]}}' AS contents;
