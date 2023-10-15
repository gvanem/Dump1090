## External dependencies

Keep most externals here. Only in source form:
 * `Curses/* ------------` : The **[PDCurses](https://github.com/wmcbrine/PDCurses)** screen library used when `USE_CURSES=1`.
 * `SDRplay-API/* -------` : The *SDRPlay API* (v. 3.09) from **[SDRplay Ltd](https://www.sdrplay.com/)**.
 * `rtl-sdr/* -----------` : *RTLSDR* interface; heavily modified from **[old-DAB's](https://github.com/old-dab/rtlsdr/blob/master/src/)** version.
 * `mongoose.[ch] -------` : The Web-server code from **[Mongoose](https://www.cesanta.com/)**.
 * `sqlite3.[ch] --------` : The Embeddable SQL Database Engine. The amalgamated **[version](http://www.sqlite.org)**.
 * `sqlite3-shell.c -----` : The source for `sqlite3.exe` shell program.
 * `wepoll.[ch] ---------` : **[Linux `epoll()`](https://github.com/piscisaureus/wepoll)** emulation for Windows.
 * `zip.[ch] + miniz.h --` : A portable simple **[zip library](https://github.com/kuba--/zip)**.
