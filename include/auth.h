#ifndef AUTH_H
#define AUTH_H

#include <stdbool.h>
#include <stddef.h>

void nvs_setup(void);
void config_load(void);
void config_save(void);

bool access_window_open(void);
bool is_authenticated(void);
void auth_ok(void);
bool valid_pin(const char *pin);

void fmt_time(char *out, size_t len);
void fmt_hhmm(int mins, char *out, size_t len);

#endif
