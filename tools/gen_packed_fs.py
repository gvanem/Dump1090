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
PY2         = (sys.version_info.major == 2)

C_TOP = """//
// Generated at %s by
// %s %s.
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
//  data ---------------------------------- fsize, modified
"""

C_BOTTOM = """  { NULL, 0, 0, NULL }
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

static int casesensitive = %d; // 1 if option '--case' was used

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
}
"""

def trace (level, s):
  if opt.verbose >= level:
     print (s)

def fmt_number (num):
  if num > 1000000:
     ret = "%d.%03d.%03d" % (num/1000000, (num/1000) % 1000, num % 1000)
  elif num > 1000:
     ret = "%d.%03d" % (num/1000, num % 1000)
  else:
     ret = "%d" % (num)
  return ret

def generate_array (in_file, num, out):
  trace (1, "Generating C-array for '%s'" % in_file)
  with open (in_file, "rb") as f:
       out.write ("//\n// Generated from '%s'\n//\n" % in_file)
       out.write ("static const unsigned char file%d[] = {\n" % num)
       data = f.read (-1)
       for n in range(0, len(data)):
           if PY2:
              out.write (" 0x%02X," % ord(data[n]))
           else:
              out.write (" 0x%02X," % data[n])
           if (n + 1) % 16 == 0:
              out.write ("\n")
       out.write (" 0x00\n};\n\n")

def write_packed_files_array (out):
  bytes = 0
  for i, f in enumerate(files_dict):
      ftime = files_dict [f]["mtime"]
      fsize = files_dict [f]["fsize"]
      fname = files_dict [f]["fname"]

      ftime_str = time.strftime ('%Y-%m-%d %H:%M:%S', time.localtime(ftime))
      comment   = " // %6d, %s" % (fsize, ftime_str)
      line      = "  { file%d, sizeof(file%d), %d,  %s\n    \"%s\"\n  },\n" % (i, i, ftime, comment, fname)
      out.write (line)
      bytes += fsize
  trace (1, "Total %s bytes data to '%s'" % (fmt_number(bytes), opt.outfile))

#
# Taken from the Python manual and modified.
# Better than 'glob.glob (".\\**", recursive=True')
#
def walktree (top, callback):
  """ Recursively descend the directory tree rooted at top,
      calling the callback function for each regular file
  """
  for f in sorted(os.listdir (top)):
      fqfn = os.path.join (top, f)  # Fully Qualified File Name
      st   = os.stat (fqfn)
      if opt.recursive and stat.S_ISDIR(st.st_mode):
         walktree (fqfn, callback)
      elif stat.S_ISREG(st.st_mode):
         callback (fqfn, st)

def add_file (file, st):
  file  = file.replace ("\\", "/")
  match = [ fnmatch.fnmatch, fnmatch.fnmatchcase] [opt.casesensitive]
  if match(file, opt.spec):
     files_dict [file] = { "mtime" : st.st_mtime,
                           "fsize" : st.st_size,
                           "fname" : file
                         }
     if opt.strip:
        files_dict [file]["fname"] = file [len(opt.strip):]
     trace (2, "Adding file '%s'" % files_dict[file]["fname"])

  else:
     trace (1, "File '%s' does not match 'opt.spec'" % file)

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
  -v, --verbose:      Increate verbose-mode. I.e. '-vv' sets level=2.
  <file-spec> files to include in '--outfile'.""" % my_name)
  sys.exit (0)

def parse_cmdline():
  parser = argparse.ArgumentParser (add_help = False)
  parser.add_argument ("-h", "--help",      dest = "help", action = "store_true")
  parser.add_argument ("-c", "--case",      dest = "casesensitive", action = "store_true")
  parser.add_argument ("-o", "--outfile",   dest = "outfile", type = str)
  parser.add_argument ("-r", "--recursive", dest = "recursive", action = "store_true")
  parser.add_argument ("-s", "--strip",     nargs = "?")
  parser.add_argument ("-v", "--verbose",   dest = "verbose", action = "count", default = 0)
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
if opt.spec[-1] in [ "\\", "/" ]:
   opt.spec += "*"

if not os.path.dirname(opt.spec):  # A '*.xx' -> './*.xx'
   opt.spec = "./" + opt.spec

trace (1, "spec: '%s'" % opt.spec)

walktree (os.path.dirname(opt.spec), add_file)
if len(files_dict) == 0:
   print ("No files matching '%s'" % opt.spec)
   sys.exit (1)

out = open (opt.outfile, "w+")

out.write (C_TOP % (time.ctime(), sys.executable, __file__))

for n, f in enumerate(files_dict):
    generate_array (f, n, out)

out.write (C_ARRAY)
write_packed_files_array (out)

out.write (C_BOTTOM % opt.casesensitive)
out.close()

