-- GET /api/articles?tag=|author=|favorited=&limit=&offset= (auth optional).
-- Only the first non-empty filter in that order applies. Newest first by id;
-- articlesCount is capped at 1,000.
SET viewer = sqlpage.user_id();

-- limit 1..100 (default 20), offset >= 0 (default 0), decimal digits only.
SELECT 'json' AS component, 422 AS status,
       '{"errors":{"body":["limit must be 1..100 and offset >= 0"]}}' AS contents
WHERE NOT (coalesce($limit::text, '20') ~ '^0*([1-9][0-9]?|100)$'
       AND coalesce($offset::text, '0') ~ '^0*[0-9]{1,18}$');

-- Each branch of the id union only runs when its filter is the one that
-- applies (its WHERE is a one-time filter on parameters alone), so every
-- filter walks its own index and stops after one page.
WITH f AS (
    SELECT nullif($tag::text, '') AS tag,
           CASE WHEN nullif($tag::text, '') IS NULL THEN nullif($author::text, '') END AS author,
           CASE WHEN nullif($tag::text, '') IS NULL AND nullif($author::text, '') IS NULL
                THEN nullif($favorited::text, '') END AS favorited
),
ids AS (
    (SELECT a2.id FROM articles a2
      WHERE nullif($tag::text, '') IS NULL AND nullif($author::text, '') IS NULL
        AND nullif($favorited::text, '') IS NULL
      ORDER BY a2.id DESC
      LIMIT coalesce($limit::text, '20')::int OFFSET coalesce($offset::text, '0')::bigint)
    UNION ALL
    (SELECT at.article_id FROM article_tags at
      WHERE nullif($tag::text, '') IS NOT NULL
        AND at.tag_id = (SELECT t.id FROM tags t WHERE t.name = $tag::text)
      ORDER BY at.article_id DESC
      LIMIT coalesce($limit::text, '20')::int OFFSET coalesce($offset::text, '0')::bigint)
    UNION ALL
    (SELECT a2.id FROM articles a2
      WHERE nullif($tag::text, '') IS NULL AND nullif($author::text, '') IS NOT NULL
        AND a2.author_id = (SELECT au.id FROM users au WHERE au.username = $author::text)
      ORDER BY a2.id DESC
      LIMIT coalesce($limit::text, '20')::int OFFSET coalesce($offset::text, '0')::bigint)
    UNION ALL
    (SELECT fv.article_id FROM favorites fv
      WHERE nullif($tag::text, '') IS NULL AND nullif($author::text, '') IS NULL
        AND nullif($favorited::text, '') IS NOT NULL
        AND fv.user_id = (SELECT fu.id FROM users fu WHERE fu.username = $favorited::text)
      ORDER BY fv.article_id DESC
      LIMIT coalesce($limit::text, '20')::int OFFSET coalesce($offset::text, '0')::bigint)
)
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
                WHERE a.id IN (SELECT id FROM ids)), '[]'::json),
           'articlesCount', (SELECT CASE
               WHEN f.tag IS NOT NULL THEN
                   (SELECT count(*) FROM (SELECT 1 FROM article_tags at
                     WHERE at.tag_id = (SELECT t.id FROM tags t WHERE t.name = f.tag) LIMIT 1000) s)
               WHEN f.author IS NOT NULL THEN
                   (SELECT count(*) FROM (SELECT 1 FROM articles a
                     WHERE a.author_id = (SELECT u.id FROM users u WHERE u.username = f.author) LIMIT 1000) s)
               WHEN f.favorited IS NOT NULL THEN
                   (SELECT count(*) FROM (SELECT 1 FROM favorites fv
                     WHERE fv.user_id = (SELECT fu.id FROM users fu WHERE fu.username = f.favorited) LIMIT 1000) s)
               ELSE
                   (SELECT count(*) FROM (SELECT 1 FROM articles a LIMIT 1000) s)
               END FROM f)) AS contents;
