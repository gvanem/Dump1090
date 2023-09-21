## TODO items:

* Extract more information from captured *Mode* S messages. `DF20` / `DF21` additional field etc.

* Fix the *SDRPlay* interface.

* Add *Hack-RF* support.

* Improve the default web interface [`web_root/index.html`](web_root/index.html).

* Make a working web-socket implementation for the Web-clients.

* Add *zip* / *gzip* compression for Web-data.

* Enhance the algorithm to reliably decode more messages (add a 2.4 MB/S decoder?).

* Improve **SBS** (*Serving Base Station*) messages when we're it's client.
  Ref: http://woodair.net/sbs/article/barebones42_socket_data.htm
  Ref: https://wiki.jetvision.de/wiki/Mode-S_Beast:Data_Output_Formats

* **UAT** (*Universal Access Transceiver*) for 978 MHz data-link.

* Pack several web-roots into an .DLL. Thus allowing to select a
  web-root at runtime. Use option:
   * `--web-page some.dll;1` for the 1st resource. :heavy_check_mark: *Done*
   * `--web-page some.dll;2` for the 2nd resource etc. :red: *Done*

* *SQLite3* features:
   * store `airport-codes.csv` into `airport-codes.csv.sqlite`.
   * store `aircraft-database.csv` into `aircraft-database.csv.sqlite`. :heavy_check_mark: *Done*
   * add a build-time option to use `WinSqlite3.dll` as part of Win-10. :heavy_check_mark: *Removed*
   * update the above `*.csv` files into `*.csv.sqlite` automatically or by an `--update` option.

* Switch from `getopt_long()` to `yopt_init()` + `yopt_next()`.
  Ref: https://g.blicky.net/ylib.git/plain/yopt.h

* Reception and decoding of **ACARS** (*Aircraft Communications Addressing and Reporting System*)
  using:
    * libacars: `https://github.com/szpajder/libacars.git`.
    * DumpVDL2:  `https://github.com/szpajder/dumpvdl2`.
    * An intro:  `https://medium.com/@xesey/receiving-airplane-data-with-acars-353291cf2786`.

  This will need a second RTLSDR/SDRPlay device.

* Use [pyModeS](https://github.com/junzis/pyModeS.git) as the decoder. Spawn:
   * `python modeslive --source net --connect localhost 30002 raw` or
   * `python modeslive --source net --connect 127.0.0.1 30005 beast`
   using simple C-embedding.

* *Airports API*:
  * expire cached records after a configurable time (not 10 min as iut is now).
  * regenerate `airport-codes.csv` automatically after certain number of days using
    `tools/gen_airport_codes_csv.py`.

* *Curses interface* (`--interactive` mode), add:
  * a statistics *sub-window* with accumulated statistics:
    * Number of unique planes/IP-addresses, network-clients, CSV/SQL-lookups and cache hits,
    * Number of network clients, bytes transferred etc.
  * a tool-tip handler; show more flight-details when mouse is over a specific call-sign.

* Add *config file*. Move some command-line options into `dump1090.cfg`. Like:
  * `aircrafts = my-own-aircrafts.csv` (do not use the default `aircraft-database.csv` file).
  * `aircrafts-sql = yes` (enable the SQL-version of the above instead).
  * `metric = 1` (always show metric units).

* *IP Access List* for controlling who is allowed to connect to a service; use the new [*mg_check_ip_acl()*](https://mongoose.ws/documentation/#mg_check_ip_acl) function.
  * E.g. in `dump1090.cfg` add a `[raw_in]` section with:
    * `deny = 113.30.148.*`  (a Spanish network).
    * `deny = 91.224.92.0/24` (a Lithuanian network).



