#!/usr/env python
"""
A tool to generate a .c-file for a built-in "Packed FileSystem".
Inspired by Mongoose' 'test/pack.c' program.
"""

import os, sys, stat, time, fnmatch, argparse

opt         = None
total_bytes = 0
files_dict  = dict()
my_name     = os.path.basename (__file__)

C_TOP = """//
// Generated at %s
// DO NOT EDIT!
//
#include <time.h>
#include <ctype.h>

int         mg_pack_case (int on);
const char *mg_unlist (size_t i);
const char *mg_unpack (const char *name, size_t *size, time_t *mtime);
"""

C_ARRAY = """
static const struct packed_file {
  const unsigned char *data;
  size_t               size;
  time_t               mtime;
  const char          *name;
} packed_files[] = {
//  data ---------------------------------- fsize, modified"""

C_BOTTOM = """{ NULL, 0, 0, NULL }
};

static int str_cmp (const char *a, const char *b)
{
  while (*a && (*a == *b))
  {
    a++;
    b++;
  }
  return *(const unsigned char*)a - *(const unsigned char*)b;
}

static int str_cmpi (const char *a, const char *b)
{
  int _a, _b;

  while (*a && (toupper(*a) == toupper(*b)))
  {
    a++;
    b++;
  }
  _a = toupper (*a);
  _b = toupper (*b);
  return (_a - _b);
}

static int casesensitive = %d;

int mg_pack_case (int on)
{
  int old = casesensitive;

  casesensitive = on;
  return (old);
}

const char *mg_unlist (size_t i)
{
  return (packed_files[i].name);
}

const char *mg_unpack (const char *name, size_t *size, time_t *mtime)
{
  const struct packed_file *p;
  const char *ret = NULL;
  int       (*cmp_func) (const char*, const char*) = (casesensitive ? str_cmp : str_cmpi);

  for (p = packed_files; p->name && ret == NULL; p++)
  {
    if ((*cmp_func)(p->name, name))
       continue;
    if (size)
       *size = p->size - 1;
    if (mtime)
       *mtime = p->mtime;
    ret = (const char*) p->data;
  }
  return (ret);
}"""

def trace (s):
  if opt.verbose:
     print (s)

def fmt_number (num):
  if num > 1000000:
     ret = "%d.%03d.%03d" % (num/1000000, (num/1000) % 1000, num % 1000)
  elif num > 1000:
     ret = "%d.%03d" % (num/1000, num % 1000)
  else:
     ret = "%d" % (num)
  return ret

def generate_array (num, in_file, out):
  with open (in_file, "rb") as f:
       print ("//\n// Generated from '%s'\n//" % in_file, file=out)
       print ("static const unsigned char file%d[] = {" % num, file=out)
       data = f.read (-1)
       for n in range(0, len(data)):
           print (" 0x%02X," % data[n], file=out, end="")
           if (n + 1) % 16 == 0:
              print (file=out)
       print (" 0x00\n};\n", file=out)

def write_files_array (out):
  bytes = 0
  for i, f in enumerate(files_dict):
      ftime = files_dict [f]["mtime"]
      fsize = files_dict [f]["fsize"]
      fname = files_dict [f]["fname"]

      comment = " // %6d, %s" % (fsize, time.strftime ('%Y-%m-%d %H:%M:%S', time.localtime(ftime)))
      line    = "  { file%d, sizeof(file%d), %d,  %s\n    \"%s\"\n  }," % (i, i, ftime, comment, fname)
      print (line, file=out)
      bytes += fsize
  return bytes

#
# Taken from the Python manual and modified.
# Better than 'glob.glob (".\\*"')
#
def walktree (top, callback, cb_data):
  """recursively descend the directory tree rooted at top,
     calling the callback function for each regular file"""

  for f in os.listdir (top):
      fqfn = os.path.join (top, f)  # Fully Qualified File Name
      mode = os.stat (fqfn).st_mode
      if opt.recursive and stat.S_ISDIR(mode):
         walktree (fqfn, callback, cb_data)
      elif stat.S_ISREG(mode):
         callback (fqfn, cb_data)

def add_file (file, cb_data):
  f = file.replace ("\\", "/")
  match = [ fnmatch.fnmatch, fnmatch.fnmatchcase] [opt.casesensitive]
  if match(f, opt.spec):
     trace ("Adding '%s'" % f)
     cb_data.append (f)
  else:
     trace ("File '%s' does not match 'opt.spec'" % f)

def show_help (error=None):
  if error:
     print ("%s. Use '%s -h' for usage." % (error, my_name))
     sys.exit (1)

  print (__doc__[1:])
  print ("""Usage: %s [options] <file-spec>
  -h, --help:         Show this help.
  -c, --case:         Be case-sensitive.
  -o, --outfile:      File to generate.
  -r, --recursive:    Walk the sub-directies recursively.
  -s, --strip X:      Strip 'X' from paths.
  -v, --verbose:      Turn on verbose-mode.
  <file-spec> files to include in '--outfile'.""" % my_name)
  sys.exit (0)

def parse_cmdline():
  parser = argparse.ArgumentParser (add_help = False)
  parser.add_argument ("-h", "--help",      dest = "help", action = "store_true")
  parser.add_argument ("-c", "--case",      dest = "casesensitive", action = "store_true")
  parser.add_argument ("-o", "--outfile",   dest = "outfile", type = str)
  parser.add_argument ("-r", "--recursive", dest = "recursive", action = "store_true")
  parser.add_argument ("-s", "--strip",     nargs = "?")
  parser.add_argument ("-v", "--verbose",   dest = "verbose", action = "store_true")
  parser.add_argument ("spec", nargs = argparse.REMAINDER)
  return parser.parse_args()

opt = parse_cmdline()
if opt.help:
   show_help()

if not opt.outfile:
   show_help ("Missing '--outfile'")

if not opt.spec:
   show_help ("Missing 'spec'")

opt.spec = opt.spec[0].replace ("\\", "/")
if opt.spec[-1] == "\\" or opt.spec[-1] == "/":
   opt.spec += "*"

if not os.path.dirname(opt.spec):  # A '*.xx' -> './*.xx'
   opt.spec = "./" + opt.spec

print ("spec: '%s'" % opt.spec)

files = []
walktree (os.path.dirname(opt.spec), add_file, files)
if len(files) == 0:
   print ("No files matching '%s'" % opt.spec)
   sys.exit (1)

out = open (opt.outfile, "w+")

print (C_TOP % time.ctime(), file=out)

for n, f in enumerate(files):
    st = os.stat (f)
    if not stat.S_ISREG(st.st_mode): # Not a regular file
       continue

    files_dict [f] = { "mtime" : st.st_mtime,
                       "fsize" : st.st_size,
                       "fname" : f
                     }
    if opt.strip:
       files_dict [f]["fname"] = f [len(opt.strip)+1:]

    print ("Processing file '%s" % f)
    generate_array (n, f, out)

print (C_ARRAY, file=out)
total_bytes = write_files_array (out)
print (C_BOTTOM % opt.casesensitive, file=out)
out.close()
print ("Total %s bytes data to '%s'" % (fmt_number(total_bytes), opt.outfile))

