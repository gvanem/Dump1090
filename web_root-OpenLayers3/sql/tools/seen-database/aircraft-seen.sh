#!/bin/sh
while true
  do
    sleep 30
        /usr/bin/python /home/pi/adsb-receiver/build/portal/logging/aircraft-seen.py
  done
