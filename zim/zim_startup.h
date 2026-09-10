/* Optional startup measurements, saved after the keyboard is presented. */
#ifndef ZIM_STARTUP_H
#define ZIM_STARTUP_H

void zim_startup_init(void);
void zim_startup_begin(const char *archive_path);
void zim_startup_file_ready(void);
void zim_startup_archive_ready(void);
void zim_startup_keyboard_ready(void);
void zim_startup_flush(void);
int zim_startup_logging(void);

#endif
