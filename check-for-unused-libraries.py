  #
  # DO NOT EDIT! This file was automatically generated
  # from F:/gv/dx-radio/gv-Dump1090/Makefile.Windows at 26-June-2022.
  # Edit that file instead.
  #
if 1:
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
    lines = f.readlines()
    f.close()

    for l in lines:
        l = l.strip()
        if l == "Unused libraries:":
          state = State.UNUSED
          continue
        if state == State.UNUSED:
           if l == "":
              break
           unused_libs.append (l)
    return unused_libs

  report (sys.argv[1], process(sys.argv[1], State.IDLE))
