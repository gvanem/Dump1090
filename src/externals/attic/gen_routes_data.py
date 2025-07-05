#!/usr/bin/env python3
"""
A tool to generate a 'routes.c' file from
  https://vrs-standing-data.adsb.lol/routes.csv
  https://vrs-standing-data.adsb.lol/routes.csv.gz

"""
import os, sys, csv, time, operator

max_rec = 0
data    = dict()
out     = sys.stdout


C_TOP = """/*
 * Generated at %s by
 * %s %s
 * DO NOT EDIT!
 */
#include "routes.h"

"""

C_BOTTOM = "size_t route_records_num = %d;\n"

def split_airports (airports):
  #
  # Split an airport string like "KBUR-KSTS" into departure == "KBUR", and
  # destination == "KSTS". Handle max 5 possible stop-overs. Like:
  # "PANC-PASI-PABE-KSEA-PABE-PACV-PAJN"
  #  ^    ^    ^              ^    ^
  #  |    |    |_s[1]         |    |_ a[-1]
  #  a[0] |_s[0]              |_ s[4]
  #
  a = airports.split ("-")
  s = a [1:-1]
  dep_dest = r'"%s", "%s", { ' % (a[0], a[-1])

  if len(s) >= 5:
     return dep_dest + r'"%s", "%s", "%s", "%s", "%s"' % (s[0], s[1], s[2], s[3], s[4])
  if len(s) == 4:
     return dep_dest + r'"%s", "%s", "%s", "%s", ""' % (s[0], s[1], s[2], s[3])
  if len(s) == 3:
     return dep_dest + r'"%s", "%s", "%s", "", ""' % (s[0], s[1], s[2])
  if len(s) == 2:
     return dep_dest + r'"%s", "%s", "", "", ""' % (s[0], s[1])
  if len(s) == 1:
     return dep_dest + r'"%s", "", "", "", ""' % (s[0])
  return dep_dest + r'"", "", "", "", ""'

#
# Dump all 'route_records' in 'data' to 'out'
# Sorted on "callsign".
#
def dump_records (out, data):
  out.write ("const route_record route_records [%d] = {\n" % len(data))
  for i, _ in enumerate(data):
      c = data[i] [0]
      a = data[i] [4]
      if c != "Callsign":  # Ignore the header record
         out.write ("    { \"%s\", %s } },\n" % (c, split_airports(a)))
      if max_rec and i > max_rec:
         break

  out.write ("};\n\n")

if len(sys.argv) == 3 and (sys.argv[1] == "-t" or sys.argv[1] == "--test"):
   max_rec = 10
   del sys.argv[1]

if len(sys.argv) < 2:
   print ("Usage: %s [-t|--test] path-of-routes.csv" % __file__)
   sys.exit(1)

with open (sys.argv[1], "r", newline="",  encoding="utf-8-sig") as f:
  data = csv.reader (f, delimiter = ",")
  data = sorted (data, key=operator.itemgetter(0))

out.write (C_TOP % (time.ctime(), sys.executable, " ".join(sys.argv)))
dump_records (out, data)
out.write (C_BOTTOM % len(data))


