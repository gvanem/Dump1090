#!/usr/bin/env python3

"""
Simple Python script for Dump1090 testing:
Supported modes: RAW-OUT, RAW-IN and SBS.

RAW-OUT server: Connect to host at port 30001 and send '*...;' messages and print it to console.
RAW-IN client:  Connect to host at port 30002, receive '*...;' messages and print it to console.
SBS client:     Connect to host at port 30003, listen for 'MSG,' text and print it to console.
"""

import sys, os, time, argparse, socket

LOG_FILE     = "SBS_client.log"
REMOTE_HOST  = "localhost"
RAW_IN_PORT  = 30001
RAW_OUT_PORT = 30002
SBS_PORT     = 30003

class cfg():
  quit = False
  logf = None
  sock = None
  data_len = 0

#
# Print to both stdout and log-file
#
def modes_log (s):
  os.write (1, bytes(s, encoding="ascii"))
  cfg.logf.write ("%s: %s" % (time.strftime("%H:%M:%S"), str(s)))

def show_help (error=None):
  if error:
    print (error)
    sys.exit (1)

  print (__doc__[1:])
  print ("""Usage: %s [options] [RAW-OUT | RAW-IN | SBS]
  -h, --help: Show this help.
  --host      Host to connect to.
  --port      TCP port to connect to.
  --wait      Seconds to wait before connecting (default=0).""" % __file__)

def parse_cmdline():
  parser = argparse.ArgumentParser (add_help=False)
  parser.add_argument ("-h", "--help", dest = "help", action = "store_true")
  parser.add_argument ("--host",       dest = "host", type = str, default = REMOTE_HOST)
  parser.add_argument ("--port",       dest = "port", type = int, default = 0)
  parser.add_argument ("--wait",       dest = "wait", type = int, default = 0)
  parser.add_argument ("mode", nargs = argparse.REMAINDER)
  return parser.parse_args()

def SBS_connect (opt):
  try:
    s = socket.socket (socket.AF_INET, socket.SOCK_STREAM)
    s.connect ((opt.host, opt.port))
    modes_log ("Connected to %s\n" % opt.host)
    return s
  except:
    modes_log ("Connection refused\n")
    cfg.logf.close()
    sys.exit (1)

#
# For receiving SBS or RAW-IN messages.
#
def raw_sbs_in_loop (sock):
  while not cfg.quit:
    data = sock.readline (100)
    if not data:
      modes_log ("Connection gone.\n")
      cfg.quit = True
      break
    modes_log (data)
    cfg.data_len += len(data)
    time.sleep (0.01)

#
# For sending RAW-OUT messages.
#
# Simulate a Dump1090 client and test the function:
#   read_from_client (cli, "\n", decode_hex_message);
#
# Todo: send random messages in round-robin:
#   *8d4aca0599141911c8301577bf5d;
#   *8d4780a099080c9a6858194590a0;
#   *8d4780a0f8200002004bb85be2ef;
#   *8d4780a099080d9a6854190ec06e;
#
# Or construct realistics messages on the fly?
#
def raw_out_loop (sock):
  while not cfg.quit:
    data = b"*8d4b969699155600e87406f5b69f;\n"
    modes_log ("Sending RAW message: %s.\n" % str(data))
    rc = sock.send (data)
    if rc > 0:
      cfg.data_len += rc
    time.sleep (10)

### main() ####################################

opt = parse_cmdline()
if opt.help:
  show_help()

if len(opt.mode) == 0:
  show_help ("Missing 'mode'. Use '%s -h' for usage" % __file__)

mode = opt.mode[0].upper()
if mode != "SBS" and mode != "RAW-IN" and mode != "RAW-OUT":
  show_help ("Unknown 'mode = %s'. Use '%s -h' for usage" % (opt.mode[0], __file__))

if opt.port == 0:
  if mode == "SBS":
    opt.port = SBS_PORT
  elif mode == "RAW-IN":
    opt.port = RAW_IN_PORT
  elif mode == "RAW-OUT":
    opt.port = RAW_OUT_PORT

cfg.logf = open (LOG_FILE, "a+t")
cfg.logf.write ("%s: --- Starting -------\n" % time.strftime("%H:%M:%S"))
modes_log ("Connecting to %s:%d\n" % (opt.host, opt.port))

if opt.wait:
  modes_log ("Waiting %d sec before connecting\n" % opt.wait)
  for i in range(opt.wait):
    time.sleep (1)
    os.write (1, b".")
  modes_log ("\n")

cfg.sock = SBS_connect (opt)

if mode == "RAW-OUT":
  loop   = raw_out_loop
  format = "Sent %d bytes\n"
else:
  loop   = raw_sbs_in_loop
  format = "Received %d bytes\n"
  cfg.sock = cfg.sock.makefile()

try:
  loop (cfg.sock)

except (ConnectionResetError, ConnectionAbortedError):
  modes_log ("Connection reset.\n")
  cfg.quit = True

except KeyboardInterrupt:
  print ("^C")
  cfg.quit = True

modes_log (format % cfg.data_len)
cfg.sock.close()
cfg.logf.close()
