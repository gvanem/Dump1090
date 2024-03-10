#!/usr/bin/env python3
"""
An alternative to a 'run.bat'.
Easier to customise for Pythonistas.
"""

import os, sys, argparse

class Colour():
  RESET = RED = GREEN = ""

try:
  from colorama import init, Fore, Style
  init()
  Colour.RESET = Style.RESET_ALL
  Colour.RED   = Fore.RED + Style.BRIGHT
  Colour.GREEN = Fore.GREEN + Style.BRIGHT
except:
  pass

my_dir   = os.path.dirname (__file__)
web_page = my_dir + r"\web_root\gmap.html"

os.putenv ("LIBUSB_DEBUG", "3")
os.putenv ("RTLSDR_TRACE", "0")

mode     = " --interactive"
logfile  = ""
infile   = ""
net_mode = ""
sbs_mode = ""
sbs_port = ""

#
# For '--rtl_tcp' or '--rtl2_tcp'. Change to suite:
#
rtl_tcp_arg = "tcp://localhost:1234"

#
# Print with colours if 'colorama' was imported OK.
#
def cprint (colour, s):
  print ("%s%s%s" % (colour, s, Colour.RESET))

#
# 'dump1090.exe' uses this to calculate the distance to
# planes. Try to use 'winsdk.windows.devices.geolocation' if not set.
#
def check_home_pos():
  home_pos = os.getenv ("DUMP1090_HOMEPOS")
  if home_pos:
     return True

  #
  # From:
  #   https://stackoverflow.com/questions/44400560/using-windows-gps-location-service-in-a-python-script/44462120#44462120
  #
  try:
    import asyncio
    import winsdk.windows.devices.geolocation as wdg

    async def get_coords():
      locator = wdg.Geolocator()
      pos     = await locator.get_geoposition_async()
      return pos.coordinate.latitude, pos.coordinate.longitude

    def get_location():
      try:
        return asyncio.run (get_coords())
      except PermissionError:
        cprint (Colour.RED, "ERROR: You need to allow applications to access you location in Windows settings")
        return None, None

    cprint (Colour.GREEN, "Looking up your position using 'Windows Geolocation'...", end="")
    os.flush (sys.stdout)

    lat, lon = get_location()
    if lat and lon:
       pos = "%.6f,%.6f" % (lat, lon)
       os.putenv ("DUMP1090_HOMEPOS", "%s" % pos)
       print (" Found pos: %s" % pos)
       return True
  except:
    return False

#
# TODO: Generate demo input for 'tools/SBS_client --raw'
#
def generate_demo_input():
  pass

def show_help():
  print (__doc__[1:])
  print ("""Usage: %s [options]
  -d, --debug:    enable '--debug gn' (not '--interactive').
      --demo:     enable demo-mode (read from generated .BIN-file)
      --infile:   read from '%s\\testfiles\\modes1.bin'
      --log:      log details to '%s\\dump1090.log'
      --sdrplay:  use SDRPlay (not a RTLSDR device)
      --sbs:      enable SBS-mode
      --raw:      enable RAW-mode
      --rtl_tcp:  use a remote RTLSDR device (via already running 'rtl_tcp.exe')
      --rtl2_tcp: use a remote RTLSDR device (via already running 'rtl2_tcp.exe')
  -h, --help:     Show this help.""" % (__file__, my_dir, my_dir))
  sys.exit (0)

parser = argparse.ArgumentParser (add_help = False)
parser.add_argument ("-d", "--debug", dest = "debug",    action = "store_true")
parser.add_argument ("--log",         dest = "log",      action = "store_true")
parser.add_argument ("--sdrplay",     dest = "sdrplay",  action = "store_true")
parser.add_argument ("--sbs",         dest = "sbs",      action = "store_true")
parser.add_argument ("--raw",         dest = "raw",      action = "store_true")
parser.add_argument ("--rtl_tcp",     dest = "rtl_tcp",  action = "store_true")
parser.add_argument ("--rtl2_tcp",    dest = "rtl2_tcp", action = "store_true")
parser.add_argument ("--demo",        dest = "demo",     action = "store_true")
parser.add_argument ("--infile",      dest = "infile",   action = "store_true")
parser.add_argument ("-h", "--help",  dest = "help",     action = "store_true")
parser.add_argument ("rest", nargs = argparse.REMAINDER, default="")
opt = parser.parse_args()
if opt.help:
   show_help()

#
# This is my location in Bergen, Norway.
# Change to suite your location. "North, East" in degrees.
#
if not check_home_pos():
   os.putenv ("DUMP1090_HOMEPOS", "60.3016821,5.3208769")

if opt.log:
   logfile = "--logfile %s\\dump1090.log" % my_dir

if opt.sdrplay:
   device = "--device sdrplay"
elif opt.rtl_tcp or opt.rtl2_tcp:
   device   = "--device %s" % rtl_tcp_arg
   net_mode =  " --net-active"
else:
   device = "--device 0"  # defaults to 1st RTLSDR device

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
   infile = " --infile %s\\testfiles\\modes1.bin" % my_dir

if opt.debug:
   mode = " --debug gn"
   if opt.rtl_tcp or opt.rtl2_tcp:
    # os.system ("start rtl_tcp -d1 -v")
    # config = " --config rtl_tcp_in.cfg" ; Disables "sbs-in" client in dump1090.exe
      mode = " --debug gn"   # more details please

if opt.rest:
   rest = " --".join (opt.rest)
   cprint (Colour.RED, "rest: '%s'" % rest)
else:
   rest = ""

dump1090_cmd = "%s\\dump1090.exe --net %s%s%s%s %s %s" % (my_dir, mode, logfile, infile, net_mode, device, rest)

if opt.sbs or opt.raw:
   dump1090_cmd = "start %s" % dump1090_cmd
   py_cmd = "%s %s\\tools\\SBS_client.py --host localhost --wait 10 --port %s %s" % \
            (sys.executable, my_dir, sbs_port, sbs_mode)
else:
   py_cmd = "<None>"

cprint (Colour.GREEN, "Running dump1090_cmd: '%s'" % dump1090_cmd)
cprint (Colour.GREEN, "Running py_cmd: '%s'" % py_cmd)

try:
  if opt.sbs or opt.raw:
     os.system (dump1090_cmd)
     os.system (py_cmd)
  else:
     os.system (dump1090_cmd)

except KeyboardInterrupt:
  pass




