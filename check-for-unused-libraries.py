#!/usr/bin/env python
#
# Check for unused libraries in a MSVC link .map file.
# Prints with some colours using 'colorama'.
#
import os, sys

class State():
  IDLE   = 0
  UNUSED = 1

class Color():
  RESET = RED = WHITE = ""

try:
  from colorama import init, Fore, Style
  init()
  Color.RESET = Style.RESET_ALL
  Color.RED   = Fore.RED + Style.BRIGHT
  Color.WHITE = Fore.WHITE + Style.BRIGHT
except:
  pass

ignore_libs = [ "oldnames.lib" ]

def report (map_file, unused):
  num = len(unused)
  plural = [ "library", "libraries" ]
  if num > 0:
    print ("%s%d unused %s in %s:%s" % (Color.RED, num, plural[num > 1], map_file, Color.RESET))
    for u in unused:
      print ("  " + u)
  print ("%sDone.%s\n" % (Color.WHITE, Color.RESET))

def process (file, state):
  unused_libs = []
  f = open (file, "rt")
  try:
    lines = f.readlines()
  except IOError:
    return []
  finally:
    f.close()

  for l in lines:
    l = l.strip()
    if l == "Unused libraries:":
      state = State.UNUSED
      continue
    if state == State.UNUSED:
      if l == "":
        break
      if not os.path.basename (l).lower() in ignore_libs:
        unused_libs.append (l)
  return unused_libs

map_file = sys.argv[1]
report (map_file, process(map_file, State.IDLE))
