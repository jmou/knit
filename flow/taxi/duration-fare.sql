.mode csv
.headers on
.import csv tripdata

SELECT STRFTIME('%s', tpep_dropoff_datetime) - STRFTIME('%s', tpep_pickup_datetime) AS duration_s, trip_distance, total_amount
FROM tripdata;
