#!/usr/bin/env python3

"""
Simple Python script for Dump1090 testing:
Supported modes: RAW-OUT, RAW-IN and SBS.

RAW-OUT server: Connect to host at port 30001 and send '*...;' messages and print it to console.
RAW-IN client:  Connect to host at port 30002, receive '*...;' messages and print it to console.
SBS client:     Connect to host at port 30003, listen for 'MSG,' text and print it to console.
"""

import sys, os, time, argparse
from socket import socket, AF_INET, SOCK_STREAM, SHUT_WR
from io     import BytesIO

LOG_FILE     = "SBS_client.log"
REMOTE_HOST  = "localhost"
RAW_IN_PORT  = 30001
RAW_OUT_PORT = 30002
SBS_PORT     = 30003

quit = False
logf = None
data_len = 0

#
# Print to both stdout and log-file
#
def modes_log (s):
  os.write (1, bytes(s, encoding="ascii"))
  logf.write ("%s: %s" % (time.strftime("%H:%M:%S"), str(s)))

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
    s = socket (AF_INET, SOCK_STREAM)
    s.connect ((opt.host, opt.port))
    modes_log ("Connected to %s\n" % opt.host)
    return s
  except:
    modes_log ("Connection refused\n")
    logf.close()
    sys.exit (1)

def sbs_in_loop (sock):
  global quit
  while not quit:
    data = sock.readline (100)
    if not data:
      modes_log ("Connection gone.\n")
      quit = True
      break
    modes_log (data)
    global data_len
    data_len += len(data)
    time.sleep (0.01)

#
# Simulate a Dump1090 client and test 'read_from_client (cli, "\n", decode_hex_message)'
#
def raw_out_loop (sock):
  global quit
  while not quit:
    data = b"*8d4b969699155600e87406f5b69f;\n"
    modes_log ("Sending RAW message: %s.\n" % str(data))
    global data_len
    rc = sock.send (data)
    if rc > 0:
      data_len += rc
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

logf = open (LOG_FILE, "a+t")
logf.write ("%s: --- Starting -------\n" % time.strftime("%H:%M:%S"))
modes_log ("Connecting to %s:%d\n" % (opt.host, opt.port))

if opt.wait:
  modes_log ("Waiting %d sec before connecting\n" % opt.wait)
  for i in range(opt.wait):
    time.sleep (1)
    os.write (1, b".")
  modes_log ("\n")

s = SBS_connect (opt)

if mode == "RAW-OUT":
  loop   = raw_out_loop
  format = "Sent %d bytes\n"
else:
  loop = sbs_in_loop
  s    = s.makefile()
  format = "Received %d bytes\n"

try:
  loop (s)

except (ConnectionResetError, ConnectionAbortedError):
  modes_log ("Connection reset.\n")
  quit = True

except KeyboardInterrupt:
  print ("^C")
  quit = True

modes_log (format % data_len)
s.close()
logf.close()
