#
# Generate a I/Q -> Magnitude lookup table.
#
# This script is used when 'USE_GEN_LUT = 1' in 'Makefile.Windows'.
# Because 'hypot()' and 'round()' may be CPU expensive at runtime.
#
import math

width = 5
print ("static uint16_t py_gen_magnitude_lut [129*129] = {")

for I in range(0, 129):
  print ("\n     // I: %d\n%*s" % (I, width, ""), end="")
  for Q in range(0, 129):
      val = round (360 * math.hypot (I, Q))
      print ("%*d, " % (width, val), end="")
      if (Q+1) % 17 == 0:
         print ("\n%*s" % (width, ""), end="")
print ("\n};")
