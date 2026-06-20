-- map.sql - display a map with points (Leaflet + OpenStreetMap tiles).
-- Self-contained: literal rows, runs on SQLite and PostgreSQL.  Open at /map.sql

SELECT 'shell' AS component, 'Map' AS title;

SELECT 'text' AS component, 'Tech landmarks' AS title,
       'Each row with latitude/longitude becomes a marker; the title is its popup.' AS contents;

-- No center/zoom given, so the map auto-fits to the points.
SELECT 'map' AS component, 'Locations' AS title;

SELECT 51.5007 AS latitude, -0.1246 AS longitude, 'Big Ben, London'        AS title
UNION ALL SELECT 48.8584,  2.2945, 'Eiffel Tower, Paris'
UNION ALL SELECT 40.7484, -73.9857, 'Empire State Building, New York'
UNION ALL SELECT 37.8199, -122.4783, 'Golden Gate Bridge, San Francisco'
UNION ALL SELECT 35.6586, 139.7454, 'Tokyo Tower, Tokyo';

-- To center/zoom explicitly instead of auto-fit, add props to the map row:
--   SELECT 'map' AS component, 'Locations' AS title,
--          48.85 AS latitude, 2.35 AS longitude, 5 AS zoom;
