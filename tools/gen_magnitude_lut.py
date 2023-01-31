#
# Generate a I/Q -> Magnitude lookup table.
#
# This script is used when 'USE_GEN_LUT = 1' in 'Makefile.Windows'.
# Because 'hypot()' and 'round()' may be CPU expensive at runtime.
#
import math

lut_table = [ 0 ] * 129*129
print ("static uint16_t py_gen_magnitude_lut [%d] = {" % len(lut_table))

num = 0
for I in range(0, 129):
  for Q in range(0, 129):
      v = round (360 * math.hypot (I, Q))
      lut_table [I*129 + Q] = v
      print ("%6d, " % v, end="")
      num += 1
      if (num % 10) == 0:
         print ("   ")
print ("\n};")
