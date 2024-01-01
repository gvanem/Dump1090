OK This is the procedure I followed for running the Gain Script for a standard piaware 3.3.0 setup/

I placed the script file (optimize-gain-piaware.py) into /home/pi 

ssh to you piaware. check the folder you in. 

pi@piaware:~$ pwd
pi@piaware:~$ /home/pi

now install python2.7

pi@piaware:~$ sudo apt-get update
pi@piaware:~$ sudo apt-get install python2.7

once that has finished installing python2.7 run the gain test script - making sure your dump1090-fa is not running first

pi@piaware:~$ sudo systemctl stop dump1090-fa

then run the script.

pi@piaware:~$ ./optimize-gain-piaware.py

once this has finished - your output will be shown. 5 tests and accross many frequencies. Default for the script is, 62 seconds for each frequency and test each frequency 5 times.  you can of course change this to your liking.

Then if you wish to change your Gain select the best result gain like so: Please note this is just an example....

===Totals===
Gain, Messages, Positions, Aircraft
49.6 2455 281 4
48.0 2496 267 5
44.5 2383 275 5
42.1 2860 306 4
40.2 2300 274 5
38.6 2570 295 5
36.4 2286 255 4


pi@piaware:~$ piaware-config rtlsdr-gain 42.1
Set rtlsdr-gain to 42.1 in /boot/piaware-config.txt:60

and the  restart the service.

pi@piaware:~$ sudo systemctl restart dump1090-fa

to change back to default the command "piaware-config rtlsdr-gain -10" is given.

and the  restart the service.

pi@piaware:~$ sudo systemctl restart dump1090-fa