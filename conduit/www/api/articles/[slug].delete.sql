-- DELETE /api/articles/:slug (auth required, author only).
SET viewer = sqlpage.require_user_id();

SELECT 'json' AS component,
       CASE WHEN a.id IS NULL THEN 404 ELSE 403 END AS status,
       CASE WHEN a.id IS NULL THEN '{"errors":{"body":["article not found"]}}'
            ELSE '{"errors":{"body":["only the author can delete this article"]}}' END AS contents
  FROM (SELECT 1) one LEFT JOIN articles a ON a.slug = $slug
 WHERE a.id IS NULL OR a.author_id <> $viewer::bigint;

WITH d AS (DELETE FROM articles WHERE slug = $slug AND author_id = $viewer::bigint)
SELECT 'json' AS component, 204 AS status;
