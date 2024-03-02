## External dependencies

Keep most externals here. Only in source form:
 * `Curses/*.[ch] ---------` : The **[PDCurses](https://github.com/wmcbrine/PDCurses)** screen library used when `USE_CURSES=1`.
 * `mimalloc/*.[ch] -------` : A mini-implementation of **[mimalloc](https://github.com/microsoft/mimalloc)** used when `USE_MIMALLOC=1`.
 * `mongoose.[ch] ---------` : The Web-server code from **[Mongoose](https://github.com/cesanta/mongoose)**.
 * `rtl-sdr/*.[ch] --------` : *RTLSDR* interface; heavily modified from **[old-DAB's](https://github.com/old-dab/rtlsdr)** version.
 * `SDRplay-API/*.h -------` : The *SDRPlay API* (v. 3.09) `.h`-files from **[SDRplay Ltd](https://www.sdrplay.com)**.
 * `sqlite3.[ch] ----------` : The Embeddable **[SQLite](http://www.sqlite.org)** Database Engine. The amalgamated version.
 * `sqlite3-shell.c -------` : The source for `sqlite3.exe` shell program.
 * `wepoll.[ch] -----------` : **[Linux `epoll()`](https://github.com/piscisaureus/wepoll)** emulation for Windows.
 * `zip.[ch] + miniz.h ----` : A portable simple **[zip library](https://github.com/kuba--/zip)**.
