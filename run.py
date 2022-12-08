#
import os, sys, argparse

my_dir   = os.path.dirname (__file__)
web_page = my_dir + r"\web_root\gmap.html"

os.putenv ("LIBUSB_DEBUG", "3")
os.putenv ("RTLSDR_TRACE", "0")

ppm       = " --ppm 58"
mode      = " --interactive"
logfile   = ""
infile    = ""
sbs_mode  = ""
sbs_port  = ""

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
        print ("ERROR: You need to allow applications to access you location in Windows settings")
        return None, None

    print ("Looking up your position using 'Windows Geolocation'...", end="")

    lat, lon = get_location()
    if lat and lon:
       pos = "%.6f,%.6f" % (lat, lon)
       os.putenv ("DUMP1090_HOMEPOS", "%s" % pos)
       print (" Found pos: %s" % pos)
       return True
  except:
    pass
    return False

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

#
# This is my location in Bergen, Norway.
# Change to suite your location. "North, East" in degrees.
#
if not check_home_pos():
   os.putenv ("DUMP1090_HOMEPOS", "60.3016821,5.3208769")

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

if opt.rest:
   # print (opt.rest)
   rest = " --".join (opt.rest)
else:
   rest = ""

full_cmd = "%s\\dump1090.exe --agc --net %s%s%s%s %s" % (my_dir, mode, logfile, infile, ppm, rest)
print (full_cmd)

try:
  if opt.sbs or opt.raw:
     os.system ("start %s" % full_cmd)
     os.system ("%s %s\\tools\\SBS_client.py --host localhost --wait 10 --port %s %s" % (sys.executable, my_dir, sbs_port, sbs_mode))
  else:
     os.system (full_cmd)

except KeyboardInterrupt:
  pass




