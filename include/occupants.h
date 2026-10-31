#ifndef OCCUPANTS_H
#define OCCUPANTS_H

#include <stdbool.h>
#include "globals.h"

const allowed_user_t *find_user(const char *uid);
int                   find_occupant(const char *uid);
bool                  add_occupant(const char *uid, const char *name);
void                  remove_occupant(int index);

#endif
