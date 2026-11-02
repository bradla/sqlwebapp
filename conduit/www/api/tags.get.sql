-- GET /api/tags: every tag, sorted by name (no auth; any Authorization header is ignored).
SELECT 'json' AS component,
       json_build_object('tags', ARRAY(SELECT name FROM tags ORDER BY name)) AS contents;
