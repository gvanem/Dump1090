#
# Ref: https://datahub.io/core/airport-codes#python
#
# Meta info:
#  curl -LO https://datahub.io/core/airport-codes/datapackage.json
#
from datapackage import Package
from pprint      import pprint

import sys

csv_file = "airport-codes.csv"
if len(sys.argv) >= 2:
   csv_file = sys.argv[1]

#
# TODO: regenerate this too.
#
sql_file = "%s.sqlite" % csv_file

#
# https://stackoverflow.com/a/29988426/1213231
#
def uprint (*objects, sep=" ", end="\n", file=sys.stdout):
  enc = file.encoding
  if enc == "UTF-8":
     print (*objects, sep=sep, end=end, file=file)
  else:
    f = lambda obj: str(obj).encode (enc, errors="backslashreplace").decode(enc)
    print (*map(f, objects), sep=sep, end=end, file=file)

if 0:
   package = Package ("https://datahub.io/core/airport-codes/datapackage.json")
else:
   package = Package ("datapackage.json")

if 0:
   # print list of all resources:
   pprint (package.resource_names)

for resource in package.resources:
    if 1:
       pprint (resource.descriptor)
       pprint ("")
    if resource.descriptor["datahub"]["type"] == "derived/csv":
       json_data = resource.read()
       f = open (csv_file, "w+t")
       print ("#ICAO,IATA,Full_name,Continent,Location,Longitude,Latitude", file=f)

       rec = 0
       for d in json_data:
           if (rec % 100) == 0:
              print ("Record %d\r" % rec, end="", file=sys.stderr)
           rec += 1

           #
           # Convert the JSON record (d):
           #  0: [ 'ENBR',                         == ICAO airport name
           #       'large_airport',                == Class
           #  2:   'Bergen Airport Flesland',      == Name
           #       '170',                          == Elevation
           #  4:   'EU',                           == Continent
           #       'NO',                           == Country
           #       'NO-12',                        == ?
           #  7:   'Bergen',                       == Location
           #  8:   'ENBR',                         == ICAO airport name
           #  9:   'BGO',                          == IATA airport name
           #       None,                           == ?
           #  11:   '5.218140125, 60.29339981' ],  == GeoPos
           #
           # to simplified CSV:
           #   "ENBR","BGO","Bergen Airport Flesland","EU","NO","Bergen","5.218140125","60.29339981"
           #
           ICAO   = d [0] or ""
           IATA   = d [9] or ""
           name   = d [2] or ""
           cont   = d [4] or ""
           city   = d [7] or ""
           geopos = d [11]
           name   = name.rstrip(",\"")         # drop any trailing ',"'
           name   = name.replace("\"","\\\"")  # replace '"' with '\"'

           comma  = geopos.index(",")
           lat    = geopos [:comma-1]
           lon    = geopos [comma+1:].lstrip()
           uprint ("\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\"" % (ICAO, IATA, name, cont, city, lat, lon), file=f)

       f.close()
       break


