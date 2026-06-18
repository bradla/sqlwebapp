-- Demo page exercising the built-in components.
SELECT 'shell' AS component, 'csqlpage demo' AS title;

SELECT 'text' AS component, 'Welcome' AS title,
       'This page is rendered by a C99 CGI program reading .sql files.' AS contents;

SELECT 'list' AS component, 'Navigation' AS title;
SELECT 'Users table' AS title, 'Browse all users' AS description, '?'      AS link;
SELECT 'Greeting'    AS title, 'Try ?name=Ada'    AS description, '?name=Ada' AS link;

SELECT 'text' AS component, 'Hello, ' || COALESCE($name, 'stranger') || '!' AS contents;

SELECT 'table' AS component, 'Users' AS title;
SELECT id, name, email FROM users ORDER BY id;

SELECT 'card' AS component, 'Cards' AS title;
SELECT name AS title, email AS description FROM users ORDER BY id;
