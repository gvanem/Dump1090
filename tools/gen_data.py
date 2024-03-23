#!/usr/bin/env python3
"""
A rewrite of https://vrs-standing-data.adsb.lol/generate-csvs.sh
into Python. Plus some more features.

Generate these files:
  aircrafts.csv + aircrafts.bin + aircrafts-test.exe
  airports.csv  + airports.bin  + airports-test.exe
  routes.csv    + routes.bin    + routes-test.exe

from GitHub: 'https://github.com/vradarserver/standing-data.git'
"""

import os, sys, stat, struct, time, csv, argparse
import fnmatch, msvcrt, shutil

#
# Globals:
#
opt        = None
temp_dir   = os.getenv ("TEMP").replace("\\", "/") + "/dump1090/standing-data"
result_dir = temp_dir + "/results"
git_config = temp_dir + "/.git/config"
git_url    = "https://github.com/vradarserver/standing-data.git"

bin_marker = "BIN-dump1090"   # magic marker
bin_header = "<12sqII"        # must match 'struct BIN_header'

aircrafts_csv = "%s/aircrafts.csv" % result_dir
airports_csv  = "%s/airports.csv"  % result_dir
routes_csv    = "%s/routes.csv"    % result_dir

aircrafts_bin = "%s/aircrafts.bin" % result_dir
airports_bin  = "%s/airports.bin"  % result_dir
routes_bin    = "%s/routes.bin"    % result_dir

aircrafts_c   = "%s/aircrafts-test.c" % result_dir
airports_c    = "%s/airports-test.c"  % result_dir
routes_c      = "%s/routes-test.c"    % result_dir
gen_data_h    = "%s/gen_data.h"       % result_dir

def error (s, prefix = ""):
  if s is None:
     s = ""
  print ("%s%s\n" % (prefix, s), file=sys.stderr)
  sys.exit (1)

def fatal (s):
  error (s, "Fatal error: ")

#
# Open a file for read or write
#
def open_file (fname, mode, encoding = None):
  try:
    if encoding:
       return open (fname, mode, encoding = encoding)
    return open (fname, mode)
  except (IOError, NameError):
    fatal ("Failed to open %s." % fname)

def create_c_file (file):
  f = open_file (file, "w+t")
  print ("Creating %s" % file)
  print ("""/*
 * Generated at %s by:
 * %s %s
 * DO NOT EDIT!
 */""" % (time.ctime(), sys.executable, __file__), file=f)
  return f

def make_dir (d):
  try:
    os.mkdir (d)
  except:
    pass

def clean_dir (d):
  print ("Remove '%s' (y/N)? " % d, end="")
  sys.stdout.flush()
  yes = int (msvcrt.getch().upper()[0])
  if yes == 'Y':
     shutil.rmtree (d)
  sys.exit (0)

#
# Recursively descend the directory tree from top, adding all
# '*.csv' files to '_dict'.
#
def walk_csv_tree (top, _dict):
  for f in sorted (os.listdir (top)):
      fqfn = os.path.join (top, f).replace ("\\", "/")
      st = os.stat (fqfn)
      if stat.S_ISDIR(st.st_mode):
         walk_csv_tree (fqfn, _dict)
      elif fnmatch.fnmatch (fqfn, "*.csv") and stat.S_ISREG(st.st_mode): # A regular '.csv' file
         _dict [fqfn] = { "header": "?",
                          "fsize" : st.st_size,
                          "fname" : fqfn
                        }

#
# For 'opt.list'
#
def list_files (name, _dict):
  print ("%d %s:" % (len(_dict), name))
  fsize = 0
  for f in _dict:
      print ("  %s" % f)
      fsize += _dict [f]["fsize"]
  print ("  %d bytes\n" % fsize)
  return fsize

#
# Open and read entire file with "utf-8-sig" encoding to take care of the BOM.
# Return content as list of lines.
#
def read_csv_file (fname, _dict):
  lines = []
  f = open_file (fname, "rt", "utf-8-sig")
  for i, l in enumerate(f.readlines()):
      if i == 0:     # save CSV header line.
         _dict [fname]["header"] = l
      else:
         lines.append (l)
  f.close()
  return lines

def append_csv_file (f, lines, header):
  if header:
     f.write (header)
  for l in lines:
      f.write (l)

def create_csv_file (to_file, name, _dict):
  f = open_file (to_file, "w+t")
  print ("Processing %-*s ... " % (len("aircraft_files"), name), end="")
  for i, from_file in enumerate(_dict):
      lines = read_csv_file (from_file, _dict)
      if i == 0:
         append_csv_file (f, lines, _dict[from_file]["header"])
      else:
         append_csv_file (f, lines, None)
  f.close()
  print ("wrote %d bytes" % os.stat(to_file).st_size)

#
# Handling of .BIN files:
#
def to_bytes (s):
  return bytes (s, encoding = "utf-8")

aircraft_format  = "<6s10s10s40s"
aircraft_rec_len = 6 + 10 + 10 + 40  # == 66

def aircraft_record (data):
  icao_addr = to_bytes (data[0])
  regist    = to_bytes (data[1])
  manuf     = to_bytes (data[2])
  model     = to_bytes (data[5])
  return struct.pack (aircraft_format, icao_addr, regist, manuf, model)

airport_format  = "<4s3s40s20s2sff"
airport_rec_len = 4 + 3 + 40 + 20 + 2 + 4 + 4   # == 77

def airport_record (data):
  icao_name = to_bytes (data[2])
  iata_name = to_bytes (data[3])
  full_name = to_bytes (data[1])
  location  = to_bytes (data[4])
  country   = to_bytes (data[5])
  latitude  = float (data[6])
  longitude = float (data[7])
  return struct.pack (airport_format,
         icao_name, iata_name, full_name, location, country, latitude, longitude)

routes_format  = "<8s20s"
routes_rec_len = 8 + 20    # == 28

def routes_record (data):
  call_sign = to_bytes (data[0])
  airports  = to_bytes (data[4])
  return struct.pack (routes_format, call_sign, airports)

def create_bin_file (to_file, from_file, dict_len, rec_len, rec_func):
  print ("Creating %s... " % to_file, end="")

  f_csv = open_file (from_file, "rt", "utf-8-sig")
  data  = csv.reader (f_csv, delimiter = ",")

  f_bin = open_file (to_file, "w+b")
  f_bin.seek (struct.calcsize(bin_header), 0)

  for rows, d in enumerate(data):
      if rows > 0:      # ignore the CSV header row
         f_bin.write (rec_func(d))

  #
  # Seek to start of .BIN file and write the header
  #
  f_bin.seek (0, 0)
  hdr = struct.pack (bin_header, to_bytes(bin_marker), int(time.time()), rows, rec_len)
  f_bin.write (hdr)
  f_bin.close()
  f_csv.close()

  print ("wrote %d records, %d bytes" % (rows, os.stat(to_file).st_size))
  return rows

#
# Create gen_data.h-file for all records matching the .BIN-files.
#
def create_gen_data_h():
  f = create_c_file (gen_data_h)
  print ("""
#ifndef GEN_DATA_H
#define GEN_DATA_H

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#ifndef _CRT_NONSTDC_NO_WARNINGS
#define _CRT_NONSTDC_NO_WARNINGS
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#pragma pack(push, 1)

/* Note: these 'char' members may NOT have be 0-terminated.
 */
typedef struct aircraft_record {  /* matching 'aircraft_format = "%s"' */
        char icao_addr [6];
        char regist   [10];
        char manuf    [10];
        char model    [40];
      } aircraft_record;

typedef struct airport_record {   /* matching 'airport_format = "%s"' */
        char  icao_name [4];
        char  iata_name [3];
        char  full_name [40];
        char  location  [20];
        char  country   [2];
        float latitude;
        float longitude;
      } airport_record;

typedef struct route_record {     /* matching 'routes_format = "%s"' */
        char call_sign [8];
        char airports [20];
      } route_record;

extern const aircraft_record *gen_aircraft_lookup (const char *icao_addr);
extern const airport_record  *gen_airport_lookup (const char *icao);
extern const route_record    *gen_route_lookup (const char *call_sign);

#pragma pack(pop)

#if defined(AIRCRAFT_LOOKUP)
  #define RECORD   aircraft_record
  #define FIELD_1  icao_addr

#elif defined(AIRPORT_LOOKUP)
  #define RECORD   airport_record
  #define FIELD_1  icao_name

#elif defined(ROUTE_LOOKUP)
  #define RECORD   route_record
  #define FIELD_1  call_sign
#endif

#if defined(AIRCRAFT_LOOKUP) || defined(AIRPORT_LOOKUP) || defined(ROUTE_LOOKUP)
  static RECORD dummy;
  #define FIELD_1_SIZE  (int) sizeof (dummy.FIELD_1)
#endif

#endif /* GEN_DATA_H */
"""  % (aircraft_format, airport_format, routes_format), file=f)
  f.close()

#
# Create a .c-file which tests the created .BIN-file.
# Also add a function 'const x_record *gen_x_lookup (const char *key)'.
#
def create_c_test_file (c_file, bin_file, rec_len, rec_num):
  f = create_c_file (c_file)
  print ("""#include "gen_data.h"

#include <time.h>

#pragma pack(push, 1)

typedef struct BIN_header {
        char     bin_marker [%d];   /* BIN-file marker == "%s" */
        time_t   created;           /* time of creation (64-bits) */
        uint32_t rec_num;           /* number of records in .BIN-file == %u */
        uint32_t rec_len;           /* sizeof(record) in .BIN-file == %u */
      } BIN_header;

#pragma pack(pop)

static const char *bin_file = "%s";

static char buf [1000];  /* work buffer */""" % \
  (len(bin_marker), bin_marker, rec_num, rec_len, bin_file), file=f)

  print ("""
#if defined(AIRCRAFT_LOOKUP)
  #define HEADER  "ICAO    Regist      Manuf       Model"

  static const char *format_rec (const aircraft_record *rec)
  {
    snprintf (buf, sizeof(buf), "%-6.6s  %-10.10s  %-10.10s  %.40s",
              rec->icao_addr, rec->regist, rec->manuf, rec->model);
    return (buf);
  }

  const aircraft_record *gen_aircraft_lookup (const char *FIELD_1)
  {
    (void) FIELD_1;
    return (NULL);
  }

#elif defined(AIRPORT_LOOKUP)
  #define HEADER  "ICAO IATA  Name                            Location             Cntry    Lat.  Long."

  /* TODO: should use 'MultiByteToWideChar()' on 'rec->full_name' and 'rec->location' here.
   */
  static const char *format_rec (const airport_record *rec)
  {
    snprintf (buf, sizeof(buf), "%-4.4s %-3.3s   %-30.30s  %-20.20s %-2.2s    %+7.2f %+7.2f",
              rec->icao_name, rec->iata_name, rec->full_name, rec->location,
              rec->country, rec->latitude, rec->longitude);
    return (buf);
  }

  const airport_record *gen_airport_lookup (const char *FIELD_1)
  {
    (void) FIELD_1;
    return (NULL);
  }

#elif defined(ROUTE_LOOKUP)
  #define HEADER  "Call-sign   Airports"

  static const char *format_rec (const route_record *rec)
  {
    snprintf (buf, sizeof(buf), "%-8.8s    %-20.20s", rec->call_sign, rec->airports);
    return (buf);
  }

  const route_record *gen_route_lookup (const char *FIELD_1)
  {
    (void) FIELD_1;
    return (NULL);
  }
#else
  #error "A 'x_TEST' must be defined."
#endif
""", file=f)

  print ("""
#if defined(AIRCRAFT_LOOKUP) || defined(AIRPORT_LOOKUP) || defined(ROUTE_LOOKUP)

static uint32_t num_rec = 0;  /* record-counter; [ 0 - hdr.rec_num-1] */
static uint32_t num_err = 0;  /* number of sort errors */

/*
 * Check that 'rec->FIELD_1' is sorted accending.
 */
static const char *check_record (uint32_t num_rec, const RECORD *rec, const RECORD *prev_rec)
{
  static char buf [100];

  buf[0] = '\\0';

  /* Check that 'rec->FIELD_1' is sorted accending.
   */
  if (num_rec >= 1 && rec->FIELD_1[0] &&
      strnicmp(prev_rec->FIELD_1, rec->FIELD_1, FIELD_1_SIZE) >= 0)
  {
    snprintf (buf, sizeof(buf), ": '%.*s' not greater than '%.*s'",
              FIELD_1_SIZE, rec->FIELD_1,
              FIELD_1_SIZE, prev_rec->FIELD_1);
    num_err++;
  }
  return (buf);
}

static void *allocate_records (const BIN_header *hdr)
{
  size_t size = hdr->rec_len * hdr->rec_num;
  void  *mem = malloc (size);

  if (!mem)
  {
    fprintf (stderr, "Failed to allocate %zu bytes for %s!\\n", size, bin_file);
    exit (1);
  }
  return (mem);
}

int main (void)
{
  FILE      *f = fopen (bin_file, "rb");
  BIN_header hdr;
  RECORD    *rec, *start, *prev_rec = NULL;

  fread (&hdr, 1, sizeof(hdr), f);
  printf ("bin_marker: %.*s\\n", (int)sizeof(hdr.bin_marker), hdr.bin_marker);
  printf ("created:    %.24s\\n", ctime(&hdr.created));
  printf ("rec_len:    %u\\n", hdr.rec_len);
  printf ("rec_num:    %u\\n\\n", hdr.rec_num);

  start = allocate_records (&hdr);

  printf ("Record %s\\n-------------------------------------------------"
          "-------------------------------------------\\n", HEADER);

  for (rec = start; num_rec < hdr.rec_num; rec++)
  {
    fread (rec, 1, sizeof(*rec), f);
    printf ("%5d: %s%s\\n", num_rec, format_rec(rec), check_record(num_rec, rec, prev_rec));
    prev_rec = rec;
    num_rec++;
  }

  fclose (f);
  free (start);
  printf ("\\nnum_err: %u\\n", num_err);
  return (num_err == 0 ? 0 : 1);
}
#endif /* AIRCRAFT_LOOKUP || AIRPORT_LOOKUP || ROUTE_LOOKUP */
""", file=f)
  f.close()

#
# Compile the above .c-file to .exe.
#
def compile_to_exe (c_file, exe_file, define):
  obj_file = c_file.replace (".c", ".obj")
  if opt.clang:
     cmd = "clang-cl"
  else:
     cmd = "cl"

  cmd += " -nologo -MDd -W3 -Zi -I%s -Fe%s -Fo%s %s %s -link -nologo -incremental:no" % \
          (result_dir, exe_file, obj_file, define, c_file)
  rc = os.system (cmd)
  if rc:
     error ("Failed to compile '%s'" % cmd)

def c_to_exe (c_file):
  exe_file = c_file.replace (".c", ".exe")
  return exe_file.replace ("/", "\\")

#
# With 'opt.test', run the above compiled .exe
#
def run_exe (exe_file):
  print ("\nRunning:\n  %s" % exe_file, file=sys.stderr)
  rc = os.system (exe_file)
  print ("-" * 80, flush=True)
  return rc

def run_git (cmd):
  print ("Running '%s'..." % cmd)
  sys.stdout.flush()
  if os.system (cmd) != 0:
     error ("'git' failed")

##############################################################################

def show_help():
  print (__doc__[1:])
  print ("""Usage: %s [options]
  -c, --clang:  Use 'clang-cl' to compile (not 'cl').
  -C, --clean:  Clean all built stuff in '%s'.
  -h, --help:   Show this help.
  -l, --list:   List all .csv-file
  -t, --test:   Run the test .exe-programs.""" % (__file__, result_dir))
  sys.exit (0)

def parse_cmdline():
  parser = argparse.ArgumentParser (add_help = False)
  parser.add_argument ("-c", "--clang", dest = "clang", action = "store_true")
  parser.add_argument ("-C", "--clean", dest = "clean", action = "store_true")
  parser.add_argument ("-h", "--help",  dest = "help",  action = "store_true")
  parser.add_argument ("-l", "--list",  dest = "list",  action = "store_true")
  parser.add_argument ("-t", "--test",  dest = "test",  action = "store_true")
  return parser.parse_args()

def do_init():
  opt = parse_cmdline()
  if opt.help:
     show_help()
  if opt.clean:
     clean_dir (result_dir)
  return opt

def do_init_git():
  if os.path.exists (git_config):
     run_git ("git.exe -C %s pull" % temp_dir)
  else:
     run_git ("git.exe clone --depth=1 %s %s" % (git_url, temp_dir))

def main():
  global opt
  opt = do_init()
  do_init_git()

  make_dir (result_dir)

  aircraft_files = dict()
  airport_files  = dict()
  route_files    = dict()

  walk_csv_tree ("%s/aircraft" % temp_dir, aircraft_files)
  walk_csv_tree ("%s/airports" % temp_dir, airport_files)
  walk_csv_tree ("%s/routes"   % temp_dir, route_files)

  if opt.list:
     total_fsize  = list_files ("aircraft_files", aircraft_files)
     total_fsize += list_files ("airport_files", airport_files)
     total_fsize += list_files ("route_files", route_files)
     print ("total_fsize: %.2f MB" % (float(total_fsize) / 1E6))
     sys.exit (0)

  create_csv_file (aircrafts_csv, "aircraft_files", aircraft_files)
  create_csv_file (airports_csv,  "airport_files",  airport_files)
  create_csv_file (routes_csv,    "route_files",    route_files)

  num_aircrafts = create_bin_file (aircrafts_bin, aircrafts_csv, len(aircraft_files), aircraft_rec_len, aircraft_record)
  num_airports  = create_bin_file (airports_bin,  airports_csv,  len(airport_files),  airport_rec_len,  airport_record)
  num_routes    = create_bin_file (routes_bin,    routes_csv,    len(route_files),    routes_rec_len,   routes_record)

  create_c_test_file (aircrafts_c, aircrafts_bin, aircraft_rec_len, num_aircrafts)
  create_c_test_file (airports_c,  airports_bin,  airport_rec_len,  num_airports)
  create_c_test_file (routes_c,    routes_bin,    routes_rec_len,   num_routes)
  create_gen_data_h()
  sys.stdout.flush()

  aircrafts_exe = c_to_exe (aircrafts_c)
  airports_exe  = c_to_exe (airports_c)
  routes_exe    = c_to_exe (routes_c)

  compile_to_exe (aircrafts_c, aircrafts_exe, "-DAIRCRAFT_LOOKUP")
  compile_to_exe (airports_c,  airports_exe,  "-DAIRPORT_LOOKUP")
  compile_to_exe (routes_c,    routes_exe,    "-DROUTE_LOOKUP")

  if opt.test:
     rc  = run_exe (aircrafts_exe)
     rc += run_exe (airports_exe)
     rc += run_exe (routes_exe)
     if rc == 0:
        print ("All tests succeeded!", file=sys.stderr)
     else:
        error ("There were some errors!")
  else:
     print ("Run %s, %s or %s to test them" % (aircrafts_exe, airports_exe, routes_exe))

if __name__ == "__main__":
  main()
