import os, configparser

#
# Fix a line like 'key = value # some trailing comment\n'
# into 'key = value\n'
#
def strip_trailing_comment (s):
  a = s.find ("#")
  b = s.find ("=")
  if a > 0 and a > b:
     s = s[:a].rstrip (" ") + "\n"
  return s

def get_lines (file):
  f = open (file, "rt")
  lines = f.readlines()
  lines2 = ""
  f.close()
  for l in lines:
      lines2 += strip_trailing_comment (l)
  return lines2

#
# Configuration Loading
#
config = configparser.ConfigParser (strict=False)

script_dir = os.path.dirname (os.path.abspath(__file__)).replace ("\\", "/")
config.read_string (get_lines(f"{script_dir}/config.ini"))

#
# General Settings
#
FETCH_INTERVAL  = config.getint ("General", "FETCH_INTERVAL", fallback=10)
MIL_PREFIX_LIST = [ prefix.strip() for prefix in config.get("General", "MIL_PREFIX_LIST", fallback="7CF").split(",") ]
TAR1090_URL     = config.get ("General", "TAR1090_URL", fallback="http://localhost/data/aircraft.json")
BLINK_MILITARY  = config.getboolean ("General", "BLINK_MILITARY", fallback=True)
VERBOSE         = config.getboolean ("General", "VERBOSE", fallback=False)

#
# Audio Settings
#
ATC_STREAM_URL = config.get ("Audio", "ATC_STREAM_URL", fallback="")
ATC_VOLUME     = config.getint ("Audio", "ATC_VOLUME", fallback=100)
ATC_AUTO_START = config.getboolean ("Audio", "AUTO_START", fallback=False)

#
# Location Settings
#
LAT       = config.getfloat ("Location", "LAT", fallback=0.0)
LON       = config.getfloat ("Location", "LON", fallback=0.0)
AREA_NAME = config.get      ("Location", "AREA_NAME", fallback="UNKNOWN")
RADIUS_NM = config.getint   ("Location", "RADIUS_NM", fallback=60)

#
# Display Settings
#
SCREEN_WIDTH     = config.getint ("Display", "SCREEN_WIDTH", fallback=960)
SCREEN_HEIGHT    = config.getint ("Display", "SCREEN_HEIGHT", fallback=640)
FPS              = config.getint ("Display", "FPS", fallback=6)
MAX_TABLE_ROWS   = config.getint ("Display", "MAX_TABLE_ROWS", fallback=10)
FONT_PATH        = config.get    ("Display", "FONT_PATH", fallback=f"{script_dir}/TerminusTTF-4.49.3.ttf")
BACKGROUND_PATH  = config.get    ("Display", "BACKGROUND_PATH", fallback=None)
TRAIL_MIN_LENGTH = config.getint ("Display", "TRAIL_MIN_LENGTH", fallback=8)
TRAIL_MAX_LENGTH = config.getint ("Display", "TRAIL_MAX_LENGTH", fallback=25)
TRAIL_MAX_SPEED  = config.getint ("Display", "TRAIL_MAX_SPEED", fallback=500)
HEADER_FONT_SIZE = config.getint ("Display", "HEADER_FONT_SIZE", fallback=32)
RADAR_FONT_SIZE  = config.getint ("Display", "RADAR_FONT_SIZE", fallback=28)
TABLE_FONT_SIZE  = config.getint ("Display", "TABLE_FONT_SIZE", fallback=28)
BOTTOM_FONT_SIZE = config.getint ("Display", "BOTTOM_FONT_SIZE", fallback=28)

#
# Colours
#
BLACK        = (0, 0, 0)
GREEN        = (0, 255, 0)
BRIGHT_GREEN = (50, 255, 50)
DIM_GREEN    = (0, 180, 0)
RED          = (255, 50, 50)
YELLOW       = (255, 255, 0)
AMBER        = (255, 191, 0)

if FONT_PATH.startswith("$0"):
   FONT_PATH = f"{script_dir}/{FONT_PATH[3:]}"

if VERBOSE:
   print (f"BOTTOM_FONT_SIZE: {BOTTOM_FONT_SIZE}")
   print (f"FONT_PATH: {FONT_PATH}")
   print (f"script_dir: {script_dir}")

