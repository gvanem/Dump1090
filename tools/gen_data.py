#!/usr/bin/env python3
"""
Download 'https://github.com/vradarserver/standing-data/archive/refs/heads/main.zip'
as needed and generate these files:
  aircrafts.csv   + aircrafts.bin   + test-aircrafts.exe
  airports.csv    + airports.bin    + test-airports.exe
  routes.csv      + routes.bin      + test-routes.exe
                    code-blocks.bin + test-code-blocks.exe
"""

import os, sys, stat, struct, time, csv, argparse, zipfile
import fnmatch, shutil, textwrap

#
# Globals:
#
opt        = None
temp_dir   = os.getenv ("TEMP").replace("\\", "/") + "/dump1090/standing-data"
result_dir = temp_dir   + "/results"
mingw_mark = result_dir + "/mingw"
zip_dir    = temp_dir + "/standing-data-main"   # the top-level directory within 'zip_file'
header     = "-" * 80
my_time    = os.stat(__file__).st_mtime

bin_marker = "BIN-dump1090"   # magic marker
bin_header = "<12sqII"        # must match 'struct BIN_header' == 28 byte

def error (s, prefix = ""):
  if s is None:
     s = ""
  print ("%s%s" % (prefix, s), file=sys.stderr)
  sys.exit (1)

def fatal (s):
  error (s, "Fatal error: ")

def make_dir (d):
  try:
    os.makedirs (d)
  except:
    pass

def remove_dir (d):
  try:
    os.rmdir (d)
    return True
  except:
    return False

#
# Open a file for read or write
#
def open_file (fname, mode, encoding = None):
  if mode[0] == 'w':
     make_dir (os.path.dirname(fname))

  try:
    if encoding:
       return open (fname, mode, encoding = encoding)
    return open (fname, mode)
  except (IOError, NameError):
    fatal ("Failed to open %s (mode=\"%s\")." % (fname, mode))

def create_c_file (fname):
  f = open_file (fname, "w+t")
  print ("Creating %s" % fname)
  f.write (textwrap.dedent ("""
                            /*
                             * Generated at %s by:
                             * %s %s
                             * DO NOT EDIT!
                             */
                             """ % (time.ctime(), sys.executable, __file__)))
  return f

def nice_size (num):
  one_MB = 1024*1024
  one_KB = 1024
  if num > one_MB:
     ret = "%d.%03d MB" % (num/one_MB, (num/one_KB) % one_MB)
  elif num > one_KB:
     ret = "%d.%03d kB" % (num/one_KB, num % one_KB)
  else:
     ret = "%d B" % num
  return ret

#
# Recursively descend the directory tree from top, adding all
# '*.csv' files to '_dict'.
#
def walk_csv_tree (top, _dict):
  for f in sorted (os.listdir (top)):
      fqfn = os.path.join (top, f).replace ("\\", "/")
      st = os.stat (fqfn)
      if stat.S_ISDIR(st.st_mode):
         walk_csv_tree (fqfn, _dict)  # recurse into sub-dir
      elif fnmatch.fnmatch (fqfn, "*.csv") and stat.S_ISREG(st.st_mode): # A regular '.csv' file
         _dict [fqfn] = { "header": "?",
                          "fsize" : st.st_size,
                          "fname" : fqfn
                        }

#
# Open and read entire file with "utf-8-sig" encoding to take
# care of the BOM. Return content as list of lines.
#
def read_csv_file (fname, _dict, is_bin_file):
  lines = []
  if is_bin_file:
     f = open_file (fname, "rb")
  else:
     f = open_file (fname, "rt", "utf-8-sig")

  for i, l in enumerate (f.readlines()):
      if i == 0:     # save CSV header line and strip the BOM.
         if l[0] == 0xEF and l[1] == 0xBB and l[2] == 0xBF:
            _dict [fname]["header"] = l[3:]
         else:
            _dict [fname]["header"] = l
      else:
         lines.append (l)
  f.close()
  return lines

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

blocks_format  = "<IIIIIB2s"
blocks_rec_len = 4 + 4 + 4 + 4 + 4 + 1 + 2   # = 23

def blocks_record (data):
  start        = int (data[0], 16)
  finish       = int (data[1], 16)
  count        = int (data[2])
  bitmask      = int (data[3], 16)
  sign_bitmask = int (data[4], 16)
  is_military  = int (data[5])
  country_ISO  = to_bytes (data[6])
  return struct.pack (blocks_format, start, finish, count, bitmask, sign_bitmask, is_military, country_ISO)

#
# Class for handling all CSV files
#
class csv_handler():
  def __init__ (self, name, rec_len, rec_func, no_csv = False):
    self._dict      = dict()
    self._dict_name = name + "_files"
    self.data       = list()
    self.rec_num    = 0
    self.rec_len    = rec_len
    self.rec_func   = rec_func
    self.no_csv     = no_csv   # Do not create a .CSV-file

    if self.no_csv:
       from_file = name
       name = os.path.basename (name)
       name = name [0:name.index (".csv")]
       self.csv_dir    = None
       self.csv_result = from_file

    else:
       self.csv_dir    = zip_dir + "/" + name
       self.csv_result = result_dir + "/" + name + ".csv"

    self.bin_result = result_dir + "/" + name + ".bin"
    self.c_test     = result_dir + "/test-" + name + ".c"
    self.exe_test   = result_dir + "/test-" + name + ".exe"
    self.define     = name.upper().replace("-", "_") + "_LOOKUP"

    if not self.no_csv:
       walk_csv_tree (self.csv_dir, self._dict)

  def list_files (self):
    print (f"Listing of '{self._dict_name}':")
    fsize = 0
    for num, f in enumerate(self._dict):
        print (f"  {f}")
        fsize += self._dict [f]["fsize"]
    print ("  %s\n" % nice_size(fsize))
    return num, fsize

  def create_csv_file (self, is_bin_file):
    f = open_file (self.csv_result, [ "w+t", "w+b" ][is_bin_file])
    print ("Processing %-14s ... " % self._dict_name, end = "")
    for i, from_file in enumerate(self._dict):
        lines = read_csv_file (from_file, self._dict, is_bin_file)
        if i == 0:
           f.write (self._dict[from_file]["header"])
        for l in lines:
            f.write (l)
    f.close()
    print ("wrote %d bytes" % os.stat(self.csv_result).st_size)

  def create_bin_file (self):
    print ("Creating %s... " % self.bin_result, end = "")
    f_csv = open_file (self.csv_result, "rt", "utf-8-sig")
    data  = csv.reader (f_csv, delimiter = ",")
    f_bin = open_file (self.bin_result, "w+b")
    f_bin.seek (struct.calcsize(bin_header), 0)

    for rows, d in enumerate(data):
        if rows > 0:      # ignore the CSV header row
           f_bin.write (self.rec_func(d))
           self.data.append (d)

    #
    # Seek to start of .BIN file and write the header
    #
    f_bin.seek (0, 0)
    hdr = struct.pack (bin_header, to_bytes(bin_marker), int(time.time()), rows, self.rec_len)
    f_bin.write (hdr)
    f_bin.close()
    f_csv.close()
    print ("wrote %d records, %d bytes" % (rows, os.stat(self.bin_result).st_size))
    self.rec_num = rows

  def create_c_test (self):
    if self.rec_num == 0:
       fatal ("Call 'self.create_bin_file()' first")
    create_c_test_file (self.c_test, self.bin_result, self.rec_len, self.rec_num)

  #
  # Compile and link 'self.c_test' to 'self.exe_test' if needed
  # and run it if opt.test == True.
  #
  def build_and_run (self):
    if not os.path.exists (self.exe_test) or \
       os.stat (self.exe_test).st_mtime < os.stat(self.c_test).st_mtime:
       if opt.mingw:
          open_file (mingw_mark, "w+")
          cmd = f"gcc.exe -O2 -g -o {self.exe_test} -I{result_dir} -D{self.define} {self.c_test}"
       else:
          obj_file = self.c_test.replace (".c", ".obj")
          cmd = [ "cl.exe", "clang-cl.exe" ] [opt.clang]
          cmd += f" -nologo -MD -W3 -Zi -I{result_dir} -Fe{self.exe_test} -Fo{obj_file} -D{self.define}"
          cmd += f" {self.c_test} -link -nologo -incremental:no"

       if run_prog (cmd) != 0:
          error ("Compile failed: '%s'" % cmd)

    if opt.test:
       return run_prog (self.exe_test.replace("/", "\\"), "-" * 80)
    return 0

#
# Class for handling all ZIP operation; exist, download, extract and listing
#
class zip_handler():
  def __init__ (self, file, url, to_dir):
    self.zipfile = file
    self.url     = url
    self.to_dir  = to_dir
    self.header  = "  Size Date     Filename\n  " + 70 * "-"
    self.pattern = "*.csv"

    if self.exist():
       print ("'%s' already exist. Assumed to be unzipped OK." % self.zipfile)
    else:
       self.download()
       self.extract()

  def exist (self):
    return (os.path.exists(self.zipfile) and os.stat(self.zipfile).st_size > 0)

  @staticmethod
  def download_progress (blocks, block_size, total_size):
    got_kBbyte = (blocks * block_size) / 1024
    print ("Got %d kBytes\r" % got_kBbyte, end="")

  def download (self):
    print ("Downloading %s..." % self.url)
    from urllib.request import urlretrieve
    urlretrieve (self.url, filename = self.zipfile, reporthook = self.download_progress)
    print ("")

  def extract (self):
    print ("Extracting '%s' to '%s'" % (self.zipfile, self.to_dir))
    f = zipfile.ZipFile (self.zipfile, "r")
    f.extractall (self.to_dir)
    f.close()

  def list_files (self):
    print ("Listing of '%s':" % self.zipfile)
    print (self.header)
    zf = zipfile.ZipFile (self.zipfile, "r")
    num = size = 0
    for f in zf.infolist():
        if not fnmatch.fnmatch (f.filename, self.pattern):
           continue
        date = "%4d%02d%02d" % (f.date_time[0:3])
        print ("%6d %s %s"  % (f.file_size, date, f.filename))
        num  += 1
        size += f.file_size
    return num, size

#
# Create a '%TEMP%/dump1090/standing-data/results/gen_data.h' file for all
# records matching the .BIN-files.
#
def create_gen_data_h (h_file):
  f = create_c_file (h_file)
  f.write (textwrap.dedent ("""
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
           #include <time.h>
           #include <io.h>
           #include <windows.h>

           #pragma pack(push, 1)

           /* Note: these 'char' members may NOT have be 0-terminated.
            */
           typedef struct aircraft_record {  /* matching 'aircraft_format = "%s"' == %d */
                   char icao_addr [6];
                   char regist   [10];
                   char manuf    [10];
                   char model    [40];
                 } aircraft_record;

           typedef struct airport_record {   /* matching 'airport_format = "%s"' == %d */
                   char  icao_name [4];
                   char  iata_name [3];
                   char  full_name [40];
                   char  location  [20];
                   char  country   [2];
                   float latitude;
                   float longitude;
                 } airport_record;

           typedef struct route_record {     /* matching 'routes_format = "%s"' == %d*/
                   char call_sign [8];
                   char airports [20];
                 } route_record;

           typedef struct blocks_record {    /* matching 'blocks_format = "%s"' == %d */
                   uint32_t start;
                   uint32_t finish;
                   uint32_t count;
                   uint32_t bitmask;
                   uint32_t sign_bitmask;
                   char     is_military;
                   char     country_ISO [2];
                 } blocks_record;

           #pragma pack(pop)

           extern const aircraft_record *gen_aircraft_lookup (const char *icao_addr);
           extern const airport_record  *gen_airport_lookup (const char *icao_addr);
           extern const route_record    *gen_route_lookup (const char *call_sign);
           extern const blocks_record   *gen_blocks_lookup (uint32_t icao_addr);


           #if defined(AIRCRAFT_LOOKUP)
             #define RECORD   aircraft_record
             #define FIELD_1  icao_addr

           #elif defined(AIRPORTS_LOOKUP)
             #define RECORD   airport_record
             #define FIELD_1  icao_name

           #elif defined(ROUTES_LOOKUP)
             #define RECORD   route_record
             #define FIELD_1  call_sign

           #elif defined(CODE_BLOCKS_LOOKUP)
             #define RECORD   blocks_record
             #define FIELD_1  start
           #endif

           #if defined(AIRCRAFT_LOOKUP) || defined(AIRPORTS_LOOKUP) || defined(ROUTES_LOOKUP)
             static RECORD dummy;
             #define FIELD_1_SIZE  (int) sizeof (dummy.FIELD_1)
           #endif

           #endif /* GEN_DATA_H */
           """  % (aircraft_format, aircraft_rec_len,
                   airport_format,  airport_rec_len,
                   routes_format,   routes_rec_len,
                   blocks_format,   blocks_rec_len)))
  f.close()
  sys.stdout.flush()

#
# Create a .c-file which tests the created .BIN-file.
# Also add a function 'const x_record *gen_x_lookup (const char *key)'.
#
def create_c_test_file (c_file, bin_file, rec_len, rec_num):
  f = create_c_file (c_file)
  f.write (textwrap.dedent ("""
     #include "gen_data.h"

     #pragma pack(push, 1)

     typedef struct BIN_header {
             char     bin_marker [%d];   /* BIN-file marker == "%s" */
             time_t   created;           /* time of creation (64-bits) */
             uint32_t rec_num;           /* number of records in .BIN-file == %u */
             uint32_t rec_len;           /* sizeof(record) in .BIN-file == %u */
           } BIN_header;                 /* == %u bytes */

     #pragma pack(pop)

     #ifndef U8_NUM
     #define U8_NUM 4
     #endif

     #ifndef U8_SIZE
     #define U8_SIZE 100
     #endif

     static const char *bin_file = "%s";

     static char buf [2000];  /* work buffer */
     """ % (len(bin_marker), bin_marker, rec_num, rec_len, struct.calcsize(bin_header), bin_file)))

  f.write (textwrap.dedent ("""
     /*
      * Turn off this annoying and incorrect warning:
      * precision used with 'S' conversion specifier, resulting in undefined behavior
      */
     #ifdef __clang__
     #pragma clang diagnostic push
     #pragma clang diagnostic ignored "-Wformat"
     #endif

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

     #elif defined(AIRPORTS_LOOKUP)
       #define HEADER  "ICAO IATA  Name                            Location             Cntry    Lat.  Long."

       /**
        * Return a `wchar_t *` string for a UTF-8 string with proper right padding.
        */
       const wchar_t *utf8_format (const char *s, int min_width)
       {
         static wchar_t buf [U8_NUM] [U8_SIZE];
         static int     idx = 0;
         wchar_t        wc_buf [U8_SIZE];
         wchar_t       *ret = buf [idx++];
         int            len;

         idx &= (U8_NUM - 1);   /* use `U8_NUM` buffers in round-robin */
         wcscpy (wc_buf, L"?");

         len = MultiByteToWideChar (CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
         len = min (len, U8_SIZE - 1);
         MultiByteToWideChar (CP_UTF8, 0, s, -1, wc_buf, len);
         _snwprintf (ret, U8_SIZE-1, L"%.*s", min_width, wc_buf);
         return (ret);
       }

       static const char *format_rec (const airport_record *rec)
       {
         const wchar_t *full_name = utf8_format (rec->full_name, 35);
         const wchar_t *location  = utf8_format (rec->location, 35);

         snprintf (buf, sizeof(buf), "%-4.4s %-3.3s   %-30.30S  %-20.20S %-2.2s    %+7.2f %+7.2f",
                   rec->icao_name, rec->iata_name, full_name, location,
                   rec->country, rec->latitude, rec->longitude);
         return (buf);
       }

       const airport_record *gen_airport_lookup (const char *FIELD_1)
       {
         (void) FIELD_1;
         return (NULL);
       }

     #elif defined(ROUTES_LOOKUP)
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

     #elif defined(CODE_BLOCKS_LOOKUP)
       #define HEADER  "Start     Finish       Count   Bitmask  Sign-bitmask is_mil  ISO2"

       static const char *format_rec (const blocks_record *rec)
       {
         snprintf (buf, sizeof(buf), "0x%06X  0x%06X  %8u  0x%06X     0x%06X       %u  %.2s",
                   rec->start, rec->finish, rec->count, rec->bitmask,
                   rec->sign_bitmask, rec->is_military, rec->country_ISO);
         return (buf);
       }

       const blocks_record *gen_blocks_lookup (uint32_t FIELD_1)
       {
         (void) FIELD_1;
         return (NULL);
       }
     #else
       #error "A 'x_LOOKUP' must be defined."
     #endif

     #ifdef __clang__
     #pragma clang diagnostic pop
     #endif

     #if defined(AIRCRAFT_LOOKUP) || defined(AIRPORTS_LOOKUP) || defined(ROUTES_LOOKUP) || defined(CODE_BLOCKS_LOOKUP)

     static uint32_t num_rec = 0;  /* record-counter; [ 0 - hdr.rec_num-1] */
     static uint32_t num_err = 0;  /* number of sort errors */

     #if defined(CODE_BLOCKS_LOOKUP)
     static uint32_t num_mil = 0;  /* number of 'rec->is_military' records */

     /*
      * Check that 'rec->start' is sorted accending.
      */
     static const char *check_record (uint32_t num_rec, const RECORD *rec, const RECORD *prev_rec)
     {
       static char buf [100];

       buf[0] = '\\0';

       if (num_rec >= 1)
       {
         if (prev_rec->start > rec->start)
         {
           snprintf (buf, sizeof(buf), " start:  0x%06X not greater than 0x%06X",
                     rec->start, prev_rec->start);
           num_err++;
         }
       }
       if (rec->is_military)
          num_mil++;
       return (buf);
     }

     #else
     /*
      * Check that 'rec->FIELD_1' is sorted accending.
      */
     static const char *check_record (uint32_t num_rec, const RECORD *rec, const RECORD *prev_rec)
     {
       static char buf [100];

       buf[0] = '\\0';

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
     #endif  /* CODE_BLOCKS_LOOKUP */

     static void *allocate_records (size_t size)
     {
       void *mem = malloc (size);

       if (!mem)
       {
         fprintf (stderr, "Failed to allocate %zu bytes for %s!\\n", size, bin_file);
         exit (1);
       }
       return (mem);
     }

     int main (void)
     {
       FILE         *f = fopen (bin_file, "rb");
       BIN_header    hdr;
       RECORD       *rec, *start, *prev_rec = NULL;
       const uint8_t BOM[] = { 0xEF, 0xBB, 0xBF };
       size_t        dsize;        /* data-size excluding BIN_header */
       long          fsize;        /* size of .BIN-file */

       if (!f)
       {
         fprintf (stderr, "Failed to open %s!\\n", bin_file);
         return (1);
       }

       /* Write an UTF-8 BOM at the start if stdout (STDOUT_FILENO=1) is redirected
        */
       if (!isatty(1))
          fwrite (&BOM, sizeof(BOM), 1, stdout);

       fread (&hdr, 1, sizeof(hdr), f);

       printf ("bin_marker: %.*s, sizeof(BIN_header): %zu\\n", (int)sizeof(hdr.bin_marker), hdr.bin_marker, sizeof(hdr));
       printf ("created:    %.24s\\n", ctime(&hdr.created));
       printf ("rec_len:    %u\\n", hdr.rec_len);
       printf ("rec_num:    %u\\n\\n", hdr.rec_num);

       /* Check the file-size vs. BIN-header
        */
       fsize = filelength (fileno(f));
       dsize = hdr.rec_len * hdr.rec_num;

       if (fsize != dsize + sizeof(hdr))
       {
         fprintf (stderr,
                 "Something is wrong with the records!\\n"
                 "file-size: %ld\\n"
                 "expecting: %zu (%u*%u + %zu)\\n", fsize, dsize + sizeof(hdr), hdr.rec_len, hdr.rec_num, sizeof(hdr));
         exit (1);
       }

       start = allocate_records (dsize);

       printf ("%s\\n-------------------------------------------------"
               "-------------------------------------------\\n", HEADER);

       for (rec = start; num_rec < hdr.rec_num; rec++)
       {
         fread (rec, 1, sizeof(*rec), f);

         printf ("%s%s\\n", format_rec(rec), check_record(num_rec, rec, prev_rec));
         prev_rec = rec;
         num_rec++;
       }

     #if defined(CODE_BLOCKS_LOOKUP)
       printf ("\\nnum_mil: %u, num_err: %u\\n", num_mil, num_err);
     #else
       printf ("\\nnum_err: %u\\n", num_err);
     #endif

       fclose (f);
       free (start);

       return (num_err == 0 ? 0 : 1);
     }
     #endif /* AIRCRAFT_LOOKUP || AIRPORTS_LOOKUP || ROUTES_LOOKUP || CODE_BLOCK_LOOKUP */
     """))

  f.close()

#
# Spawn a command:
#   1) 'cl.exe', 'clang-cl.exe' or 'gcc.exe'
#   2) with 'opt.test', run a compiled .exe-file
#
def run_prog (cmd, header = None):
  print ("\ncmd:\n  %s" % cmd, file = sys.stderr, flush = True)
  sys.stdout.flush()
  rc = os.system (cmd)
  if header:
     print (header, flush = True)
  return rc

##############################################################################

def show_help():
  print (__doc__[1:], end = "")
  print ("The above are generated under '%s'.\n" % result_dir)

  print ("""Usage: %s [options]
  -c, --clang:         Use 'clang-cl.exe' to compile (not 'cl.exe').
  -C, --clean:         Clean ALL stuff under '%s'.
  -g, --gen-c <file>:  Generate .c-code from 'code-blocks.bin' into '<file>'.
  -h, --help:          Show this help.
  -l, --list:          List all .csv-file
  -m, --mingw:         Use MinGW 'gcc.exe' to compile (not 'cl.exe').
  -t, --test:          Build and run the test-programs '%s/*.exe'.""" % \
    (__file__, zip_dir, result_dir))
  sys.exit (0)

def parse_cmdline():
  parser = argparse.ArgumentParser (add_help = False)
  parser.add_argument ("-c", "--clang", dest = "clang", action = "store_true")
  parser.add_argument ("-C", "--clean", dest = "clean", action = "store_true")
  parser.add_argument ("-g", "--gen-c", dest = "gen_c", type = str, default = None)
  parser.add_argument ("-h", "--help",  dest = "help",  action = "store_true")
  parser.add_argument ("-l", "--list",  dest = "list",  action = "store_true")
  parser.add_argument ("-m", "--mingw", dest = "mingw", action = "store_true")
  parser.add_argument ("-t", "--test",  dest = "test",  action = "store_true")
  return parser.parse_args()

def do_init():
  opt = parse_cmdline()
  if opt.help:
     show_help()
  if opt.clean:
     print ("Cleaning '%s/**':" % temp_dir)
     shutil.rmtree (temp_dir, ignore_errors = True)
     sys.exit (0)

  return opt

def main():
  global opt
  opt = do_init()

  make_dir (temp_dir)

  Zip = zip_handler ("%s/standing-data.zip" % temp_dir,
                     "https://github.com/vradarserver/standing-data/archive/refs/heads/main.zip",
                     temp_dir)

  aircraft = csv_handler ("aircraft", aircraft_rec_len, aircraft_record)
  airports = csv_handler ("airports", airport_rec_len, airport_record)
  routes   = csv_handler ("routes", routes_rec_len, routes_record)
  blocks   = csv_handler ("%s/code-blocks/schema-01/code-blocks.csv" % zip_dir, blocks_rec_len, blocks_record, True)

  #
  # If this file was created, use MinGW 'gcc.exe' to test the result.
  #
  if os.path.exists (mingw_mark):
     opt.mingw = True

  if opt.list:
     num, fsize = Zip.list_files()
     print ("num: %d, fsize: %s\n" % (num, nice_size(fsize)))

     num1, fsize1 = aircraft.list_files()
     num2, fsize2 = airports.list_files()
     num3, fsize3 = routes.list_files()
     print ("num: %d, fsize: %s" % (num1 + num2 + num3, nice_size(fsize1 + fsize2 + fsize3)))
     sys.exit (0)

  aircraft.create_csv_file (True)
  airports.create_csv_file (True)
  routes.create_csv_file (False)

  aircraft.create_bin_file()
  airports.create_bin_file()
  routes.create_bin_file()
  blocks.create_bin_file()

  aircraft.create_c_test()
  airports.create_c_test()
  routes.create_c_test()
  blocks.create_c_test()

  create_gen_data_h (result_dir + "/gen_data.h")

  if opt.gen_c:
     gen_c_file (blocks)

  rc  = aircraft.build_and_run()
  rc += airports.build_and_run()
  rc += routes.build_and_run()
  rc += blocks.build_and_run()
  if opt.test:
     if rc == 0:
        print ("All tests succeeded!", file = sys.stderr)
     else:
        error ("There were some errors!")

  else:
     print ("\nRun '%s %s --test' to test these:\n  %s\n  %s\n  %s\n  %s\n" % \
            (sys.executable, __file__, aircraft.exe_test, airports.exe_test, blocks.exe_test, routes.exe_test))

#
# For gen_data.py --gen-c:
#
generate_c_code_top = textwrap.dedent ("""
   #include "gen_data.h"

   /*
    * Squelch this:
    *   warning C4295: 'country_ISO': array is too small to include a terminating null character
    */
   #pragma warning (disable: 4295)

   /*
    * From `%s/code-blocks/schema-01/README.md:
    *   Iterate through each code block in descending order of `SignificantBitmask`.
    *   AND the aircraft Mode-S identifier with the `SignificantBitmask`.
    *   If the result equals the `Bitmask` then the code block matches.
    *   Stop searching as soon as you find a match.
    *
    * Create an array sorted on `data->sign_bitmask' (descending order)
    * as a static C-array. It could be included into `aircraft.c'
    * to replace `aircraft_is_military()'.
    */
   static const blocks_record sorted_blocks[] = {
     /* start     finish      count   bitmask  sign_bitmask is_mil country_ISO
      */
   """ % zip_dir)

generate_c_code_bottom_1 = textwrap.dedent ("""
     };

   static size_t num_sorted_blocks = %u;
   """)

generate_c_code_bottom_2 = textwrap.dedent ("""
   const blocks_record *aircraft_find_block (uint32_t addr)
   {
     const blocks_record *b = sorted_blocks + 0;
     size_t i;

     for (i = 0; i < num_sorted_blocks; i++, b++)
        if ((addr & b->sign_bitmask) == b->bitmask)
           return (b);
     return (NULL);
   }

   #ifdef TEST_CODE_BLOCKS
   static uint32_t random_range (uint32_t min, uint32_t max)
   {
     double scaled = (double) rand() / RAND_MAX;
     return (uint32_t) ((max - min + 1) * scaled) + min;
   }

   static void find_and_print_block (uint32_t addr)
   {
     const blocks_record *b = aircraft_find_block (addr);

     if (!b)
          printf ("%06X: not found!!\\n", addr);
     else printf ("%06X: %5zu  %d    '%.2s'\\n",
                  addr, ((char*)b - (char*)&sorted_blocks[0]) / sizeof(*b), b->is_military, b->country_ISO);
   }

   int main (void)
   {
     size_t i;

     srand (time(NULL));
     puts ("50 random ICAO-addresses:\\n"
           "ICAO      rec  Mil  Country");
     puts ("------------------------------");

     for (i = 0; i < 50; i++)
         find_and_print_block (random_range(0, 0x7FFFFF));

     puts ("\\nSome 47xx addresses:");
     find_and_print_block (0x4780C6);
     find_and_print_block (0x4780E0);
     find_and_print_block (0x47FFFF);
     return (0);
   }
   #endif /* TEST_CODE_BLOCKS */
   """)

def gen_c_file (blocks):
  f = create_c_file (opt.gen_c)
  f.write (generate_c_code_top)
  for d in sorted (blocks.data, reverse = True, key = lambda field: field[4]):
      f.write ("   { 0x%06X, 0x%06X, %7u, 0x%06X, 0x%06X,     %d,    \"%.2s\" },\n" % \
        (int(d[0], 16),    # blocks_record::start
         int(d[1], 16),    # blocks_record::finish
         int(d[2]),        # blocks_record::count
         int(d[3], 16),    # blocks_record::bitmask
         int(d[4], 16),    # blocks_record::sign_bitmask, field[4]
         int(d[5]),        # blocks_record::is_military
         d[6]))            # blocks_record::country_ISO

  f.write (generate_c_code_bottom_1 % blocks.rec_num)
  f.write (generate_c_code_bottom_2)
  f.close()

##########################################################

if __name__ == "__main__":
  main()

