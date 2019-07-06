.mode csv
.headers on
.import gen/input.csv input

SELECT duration_s / 60 AS duration_m, AVG(trip_distance) AS trip_distance, AVG(total_amount) AS total_amount
FROM input
GROUP BY duration_s / 60
HAVING duration_m > 0
AND duration_m < 60
ORDER BY duration_m;
