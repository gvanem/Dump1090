#
import os, sys, argparse

my_dir      = os.path.dirname (__file__)
web_page    = my_dir + r"\web_root\gmap.html"
py_launcher = os.getenv("WinDir", "") + "\\py.exe -3"

os.putenv ("LIBUSB_DEBUG", "3")
os.putenv ("RTLSDR_TRACE", "1")

ppm       = " --ppm 58"
frequency = " --freq 1088.934M"
mode      = " --interactive"
logfile   = ""
infile    = ""
sbs_mode  = ""
sbs_port  = ""

home_pos = os.getenv ("DUMP1090_HOMEPOS")
if not home_pos:
  #
  # 'dump1090.exe' uses this to calculate the distance to
  # planes. This is my location in Bergen, Norway.
  # Change to suite your location. "North, East" in degrees.
  #
  os.putenv ("DUMP1090_HOMEPOS", "60.3016821,5.3208769")

#
# TODO: Generate demo input for 'tools/SBS_client --raw'
#
def generate_demo_input():
  pass

parser = argparse.ArgumentParser()
parser.add_argument ("--debug", dest = "debug",  action = "store_true")
parser.add_argument ("--log",   dest = "log",    action = "store_true")
parser.add_argument ("--sbs",   dest = "sbs",    action = "store_true")
parser.add_argument ("--raw",   dest = "raw",    action = "store_true")
parser.add_argument ("--demo",  dest = "demo",   action = "store_true")
parser.add_argument ("--infile",dest = "infile", action = "store_true")

parser.add_argument ("rest", nargs = argparse.REMAINDER, default="")
opt = parser.parse_args()

if opt.log:
  logfile = "--logfile %s\\dump1090.log" % my_dir

if opt.sbs:
  sbs_mode = " SBS"
  sbs_port = " 30003"

elif opt.raw:
  sbs_mode = " RAW-OUT"
  sbs_port = " 30001"

elif opt.demo:
  sbs_mode = " RAW-OUT"
  sbs_port = " 30001"
  opt.raw  = True
  generate_demo_input()

if opt.infile:
  infile = " --infile testfiles\\modes1.bin"

if opt.debug:
  mode = " --debug gn"

if 0:
  print (opt.rest)
  rest = " --".join (opt.rest)
else:
  rest = ""

full_cmd = "%s\\dump1090.exe --agc --net %s %s%s%s%s %s" % (my_dir, frequency, mode, logfile, infile, ppm, rest)
print (full_cmd)

try:
  if opt.sbs or opt.raw:
     os.system ("start %s" % full_cmd)
     os.system ("%s tools\\SBS_client.py --host localhost --wait 10 --port %s %s" % (py_launcher, sbs_port, sbs_mode))
  else:
     os.system (full_cmd)

except KeyboardInterrupt:
  pass




