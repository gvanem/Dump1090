#!/usr/env python
"""
A tool to generate a .c-file for a built-in "Packed FileSystem".
Inspired by Mongoose' 'test/pack.c' program.
"""

import os, sys, stat, time, fnmatch, argparse

try:
  import csscompressor, htmlmin, jsmin, io
  have_minify = True
except ImportError:
  have_minify = False

opt        = None
files_dict = dict()
my_name    = os.path.basename (__file__)
PY2        = (sys.version_info.major == 2)

total_in_bytes  = 0
total_out_bytes = 0

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

#if defined(__clang__) || defined(__GNUC__)
  #pragma GCC diagnostic ignored "-Wcomment"  /* multi-line // comment [-Wcomment] */
#elif defined(_MSC_VER)
  #pragma warning (disable:4010) /* single-line comment contains line-continuation character */
#endif

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
  int (*cmp_func) (const char*, const char*) = (casesensitive ? str_cmp : str_cmpi);

  for (p = packed_files; p->name; p++)
  {
    if ((*cmp_func)(p->name, name))
       continue;
    if (size)
       *size = p->size - 1;
    if (mtime)
       *mtime = p->mtime;
    return (const char*) p->data;
  }
  return (NULL);
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

def dump_hex (in_file, out_file, data, data_len, len_in, num):
  files_dict [in_file]["fsize"] = data_len
  out_file.write ("//\n// Minified version generated from '%s' (%d%% saving) \n//\n" % (in_file, 100 - 100*data_len/len_in))
  out_file.write ("static const unsigned char file%d[] = {\n" % num)
  comment = [ " // ", "" ] [opt.nocomments]
  for n in range(0, data_len):
      c = data[n]
      assert ord(c) <= 255
      out_file.write (" 0x%02X," % ord(c))
      if not opt.nocomments:
         if c not in ['\r', '\n']:
            comment += str(c)
      if (n + 1) % 16 == 0:
         out_file.write ("%s\n" % comment)
         comment = [ " // ", "" ] [opt.nocomments]
  out_file.write (" 0x00\n};\n\n")

def generate_array_css (in_file, out_file, num):
  with open (in_file, "r") as f:
       data_in  = f.read (-1)
       data_out = csscompressor.compress (data_in, preserve_exclamation_comments = False)
       len_in   = len(data_in)
       len_out  = len(data_out)
       dump_hex (in_file, out_file, data_out, len_out, len_in, num)
  return len_in, len_out

def generate_array_html (in_file, out_file, num):
  with open (in_file, "r") as f:
       data_in  = f.read (-1)
       data_out = htmlmin.minify (data_in, remove_comments = True, remove_empty_space = False)
       len_in   = len(data_in)
       len_out  = len(data_out)
       dump_hex (in_file, out_file, data_out, len_out, len_in, num)
  return len_in, len_out

def generate_array_js (in_file, out_file, num):
  with open (in_file, "r") as f:
       data_in = f.read (-1)
       ins  = io.StringIO (data_in)
       outs = io.StringIO()
       jsmin.JavascriptMinify().minify (ins, outs)
       data_out = outs.getvalue()
       len_in   = len(data_in)
       len_out  = len(data_out)
       dump_hex (in_file, out_file, data_out, len_out, len_in, num)
  return len_in, len_out

def generate_array (in_file, out_file, num):
  with open (in_file, "rb") as f:
       out_file.write ("//\n// Generated from '%s'\n//\n" % in_file)
       out_file.write ("static const unsigned char file%d[] = {\n" % num)
       data_in  = f.read (-1)
       data_out = data_in
       len_in   = len(data_in)
       len_out  = len_in
       for n in range(0, len_in):
           if PY2:
              out.write (" 0x%02X," % ord(data_out[n]))
           else:
              out_file.write (" 0x%02X," % data_out[n])
           if (n + 1) % 16 == 0:
              out_file.write ("\n")
       out_file.write (" 0x00\n};\n\n")
  return len_in, len_out

def write_packed_files_array (out):
  bytes = 0
  for i, f in enumerate (files_dict):
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
  for f in sorted (os.listdir (top)):
      fqfn = os.path.join (top, f)  # Fully Qualified File Name
      st   = os.stat (fqfn)
      if opt.recursive and stat.S_ISDIR(st.st_mode):
         walktree (fqfn, callback)
      elif stat.S_ISREG(st.st_mode):
         callback (fqfn, st)

def add_file (file, st):
  file  = file.replace ("\\", "/")
  match = [ fnmatch.fnmatch, fnmatch.fnmatchcase] [opt.casesensitive]
  if match (file, opt.spec):
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
  -m, --minify:       Compress the .js/.css/.html files first (not for Python2).
  -o, --outfile:      File to generate.
  -r, --recursive:    Walk the sub-directies recursively.
  -s, --strip X:      Strip 'X' from paths.
  -v, --verbose:      Increate verbose-mode. I.e. '-vv' sets level=2.
  <file-spec> files to include in '--outfile'.""" % my_name)
  sys.exit (0)

def parse_cmdline():
  parser = argparse.ArgumentParser (add_help = False)
  parser.add_argument ("-h", "--help",        dest = "help", action = "store_true")
  parser.add_argument ("-c", "--case",        dest = "casesensitive", action = "store_true")
  parser.add_argument ("-m", "--minify",      dest = "minify", action = "store_true")
  parser.add_argument (      "--no-comments", dest = "nocomments", action = "store_true", default = False)
  parser.add_argument ("-o", "--outfile",     dest = "outfile", type = str)
  parser.add_argument ("-r", "--recursive",   dest = "recursive", action = "store_true")
  parser.add_argument ("-s", "--strip",       nargs = "?")
  parser.add_argument ("-v", "--verbose",     dest = "verbose", action = "count", default = 0)
  parser.add_argument ("spec", nargs = argparse.REMAINDER)

  return parser.parse_args()

opt = parse_cmdline()
if opt.help:
   show_help()

if not opt.outfile:
   show_help ("Missing '--outfile'")

if not opt.spec:
   show_help ("Missing 'spec'")

if PY2:
   have_minify = False

if opt.minify and not have_minify:
   show_help ("Option '--minify' not available")

opt.spec = opt.spec[0].replace ("\\", "/")
if opt.spec[-1] in [ "\\", "/" ]:
   opt.spec += "*"

if not os.path.dirname(opt.spec):  # A '*.xx' -> './*.xx'
   opt.spec = "./" + opt.spec

dirname = os.path.dirname(opt.spec)
if not os.path.exists(dirname):
   show_help ("Directory '%s' not found" % dirname)

trace (1, "spec: '%s'" % opt.spec)

walktree (os.path.dirname(opt.spec), add_file)
if len(files_dict) == 0:
   print ("No files matching '%s'" % opt.spec)
   sys.exit (1)

out = open (opt.outfile, "w+")

out.write (C_TOP % (time.ctime(), sys.executable, __file__))

minifiers = { }
minifiers [".css"]  = generate_array_css
minifiers [".js"]   = generate_array_js
minifiers [".html"] = generate_array_html
minifiers [".*"]    = generate_array

for n, f in enumerate (files_dict):
    size = fmt_number (files_dict[f]["fsize"])
    already_minified = (f.endswith(".min.css") or f.endswith(".js.css"))
    if already_minified:
       trace (1, "Not minifying '%s'" % f)

    if already_minified or not opt.minify:
       trace (1, "%10s: Generating C-array for '%s'" % (size, f))
       len_in, len_out = generate_array (f, out, n)
    else:
       ext = f [f.rfind("."):]
       if ext in minifiers:
          trace (1, "%10s: Generating minified C-array for '%s'" % (sz, f))
          len_in, len_out = minifiers [ext] (f, out, n)
       else:
          trace (1, "%10s: Generating C-array for '%s'" % (sz, f))
          len_in, len_out = minifiers [".*"] (f, out, n)

    total_in_bytes  += len_in
    total_out_bytes += len_out

out.write (C_ARRAY)
write_packed_files_array (out)

out.write (C_BOTTOM % opt.casesensitive)
out.close()

if opt.minify:
   savings = 100 - 100 * total_out_bytes / total_in_bytes
   trace (1, "The '--minify' option gave %d%% total savings." % savings)
