#!/usr/bin/python2.7
import time, socket, subprocess, fileinput, os

measure_duration = 32 #seconds 62
ntests = 5
#ntests = 10

#gains = "20.7 22.9 25.4 28.0 29.7 32.8 33.8 36.4 37.2 38.6 40.2 42.1 43.4 43.9 44.5 48.0 49.6".split()
#gains = "20.7 22.9 25.4 28.0 29.7 32.8 33.8 36.4".split()
#gains = "36.4 38.6 40.2 42.1 44.5 48.0 49.6".split()
gains = "29.7 32.8 33.8 36.4 38.6 40.2 42.1 44.5 48.0 49.6".split()

gains.reverse()
results = {}

originalgain = ''         # stuff in the starting value form the file

for i in range(ntests):
   print "test", i+1, "of", ntests
   for g in gains:
      if g not in results:
         results[g] = [0,0,{}] #msgs, positions, aircraft

      for line in fileinput.input('/etc/default/dump1090-mutability', inplace=1):
         if line.startswith('GAIN'):
            print 'GAIN='+g
            if len(originalgain) == 0:
               originalgain = line         # save so we can restore it at the end
         else:
            print line,
      os.system("sudo /etc/init.d/dump1090-mutability restart >/dev/null")
      #os.system("sudo systemctl restart dump1090-fa")
      time.sleep(2)
      s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
      s.connect(('localhost',30003))
      t = time.time()
      d = ''
      while 1:
         d += s.recv(32)
         if time.time() - t > measure_duration:
            break
      s.close()
      messages = 0
      positions = 0
      planes = {}
      for l in d.split('\n'):
         a = l.split(',')
         messages += 1
         if len(a) > 4:
            if a[1] == '3':
               positions += 1
            planes[a[4]] = 1
      print "gain=",g, "messages=", messages, "positions=", positions, "planes=", len(planes.keys())
      results[g][0] += messages
      results[g][1] += positions
      for hex in planes.keys():
         results[g][2][hex] = 1

print "\n===Totals==="
print "Gain, Messages, Positions, Aircraft"
for g in gains:
   (messages,positions,planes) = results[g]
   print g, messages, positions, len(planes.keys())

# now restore the starting gain value

for line in fileinput.input('/etc/default/dump1090-mutability', inplace=1):
   if line.startswith('GAIN'):
      print 'GAIN='+originalgain 
   else:
      print line,
 
os.system("sudo /etc/init.d/dump1090-mutability restart >/dev/null")
#os.system("sudo systemctl restart dump1090-fa")