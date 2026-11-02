-- DELETE /api/articles/:slug/comments/:id (auth required, comment author only).
-- A non-integer id, an unknown id, or a comment on another article is 404.
SET viewer = sqlpage.require_user_id();

SELECT 'json' AS component,
       CASE WHEN c.id IS NULL THEN 404 ELSE 403 END AS status,
       CASE WHEN c.id IS NULL THEN '{"errors":{"body":["comment not found"]}}'
            ELSE '{"errors":{"body":["only the author can delete this comment"]}}' END AS contents
  FROM (SELECT 1) one
  LEFT JOIN (comments c JOIN articles a ON a.id = c.article_id AND a.slug = $slug)
         ON c.id = CASE WHEN $id::text ~ '^[0-9]{1,18}$' THEN $id::text::bigint END
 WHERE c.id IS NULL OR c.author_id <> $viewer::bigint;

WITH d AS (DELETE FROM comments WHERE id = $id::text::bigint AND author_id = $viewer::bigint)
SELECT 'json' AS component, 204 AS status;
