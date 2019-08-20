.mode csv
.headers on
.import csv zones

SELECT LocationID, Borough
FROM zones;
