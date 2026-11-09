DROP TABLE IF EXISTS daily_stats;
CREATE TABLE daily_stats AS
SELECT date_trunc('day', created_at)::date AS d,
       count(*) AS c,
       sum(score) AS s
FROM users
GROUP BY 1
ORDER BY 1;
