# Dump1090-OpenLayers3-html SQL Supporting files
These files demonstrate how to query a remote mySql server to return details of the aircraft etc.

I used this before adapting a modified /db/ struture, but they are retained for interest.

They obviously wont work unless the apropriate server, database and table/s are available!

## The following files exist:

1. aircraft_all_test.html - Show all aircraft

2. aircraft_all_test.php

3. aircraft_one_test.html - show one aricraft

4. aircraft_one_test.php

5. aircraft_seen_all_test.html - show 'seen count' for all a/c

6. aircraft_seen_all_test.php

7. aircraft_seen_one_test.html - show 'seen count' for one a/c

8. aircraft_seen_one_test.php

9. sql_server.php - details of the mySql server

10. sql_table_aircraft.php - aircraft database/table details

11. sql_table_seen.php - seen table details

12. \tools\

REQUIRED:
apt-get install mysql-server
apt-get install mysql-server
apt-get install php5-common php5-cgi php5 php5-mysql
sudo lighty-enable-mod fastcgi-php