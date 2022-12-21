## TODO items

* Extract more information from captured Mode S messages.
* Improve the web interface gmap.html.
* Enhance the algorithm to reliably decode more messages.
* On the Web-page:
  - show the distance to the selected plane
    (based on an exact position of the RTLSDR; use a cookie to store it?).
  - show debug info and parsed messages. Use EmScripten to push it?
  - a toggle for 3D view. Use LuaJIT + Anima + EmScripten to push it?
* UAT; Universal Access Transceiver for 978 MHz datalink.
* Kerberos SDR; Directional Finding Feature:
   ref: https://github.com/rfjohnso/kerberossdr/tree/PyQt5
* Pack several web-roots into an .DLL. Thus allowing to select a
  web-root at runtime. Use option `--web-page some.dll`. Then `some.dll`
  would then need some exported `CreateInstance()` and `DeleteInstance()`
  functions.

