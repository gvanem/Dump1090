## External dependencies

Keep most externals here. Only in source form:
 * `mongoose.[ch] -----` : The Web-server code from **[Mongoose](https://www.cesanta.com/)**.
 * `rtlsdr-emul.[ch] --` : Interface for the **[rtlsdr-emulator-sdrplay](https://github.com/JvanKatwijk/rtlsdr-emulator-sdrplay/)** DLL.
 * `rtl-sdr/* ---------` : *RTLSDR* interface; heavily modified from **[old-DAB's](https://github.com/old-dab/rtlsdr/blob/master/src/)** version.
 * `SDRplay-API/* -----` : The *SDRPlay API* (v. 3.09) from **[SDRplay Ltd](https://www.sdrplay.com/)**.
 * `Curses/* ----------` : The **[PDCurses](https://github.com/wmcbrine/PDCurses)** screen library used when `USE_CURSES=1`.
 * `wepoll.[ch] -------` : **[Linux `epoll()`](https://github.com/piscisaureus/wepoll)** emulation for Windows.
 * `zip.* -------------` : A portable simple **[zip library](https://github.com/kuba--/zip)**.
