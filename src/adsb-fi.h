#pragma once

bool     adsb_fi_init (void);
void     adsb_fi_exit (void);
void     adsb_fi_poll (void);
void     adsb_fi_show_stats (void);
void     adsb_fi_read_loop (void);
bool     adsb_fi_max_distance (const char *arg);
bool     adsb_fi_sleep_time (const char *arg);
bool     adsb_fi_raw_buf_time (const char *arg);
bool     adsb_fi_print_old (const char *arg);
bool     adsb_fi_print_non_icao (const char *arg);
uint32_t adsb_fi_num_planes (void);
uint32_t adsb_fi_get_max_distance (void);


