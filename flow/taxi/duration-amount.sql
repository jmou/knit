cat <<EOF
.mode csv
.headers on
.import csv input

SELECT duration_s / 60 AS duration_m, AVG($column) AS $column
FROM input
GROUP BY duration_s / 60
HAVING duration_m > 0
AND duration_m < $max_duration_m
ORDER BY duration_m;
EOF
