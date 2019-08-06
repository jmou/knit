.mode csv
.headers on
.import gen/zones.csv zones

SELECT LocationID, Borough
FROM zones;
