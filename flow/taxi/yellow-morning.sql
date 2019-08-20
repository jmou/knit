.mode csv
.headers on
.import csv tripdata

SELECT tpep_pickup_datetime, tpep_dropoff_datetime, trip_distance, PULocationID, DOLocationID, total_amount
FROM tripdata
WHERE TIME(tpep_pickup_datetime) > '05:00:00'
AND TIME(tpep_pickup_datetime) < '08:00:00';
