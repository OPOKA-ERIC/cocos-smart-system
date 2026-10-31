#include "occupants.h"

#include <string.h>
#include <time.h>

/* ---------------------------------------------------------------
   RFID ALLOW-LIST
   Only cards listed here may enter.
--------------------------------------------------------------- */
const allowed_user_t g_allowed[] = {
    {"01:02:03:04",          "Blue Card (Alice)"},
    {"55:66:77:88",          "Yellow Card (Bob)"},
    {"AA:BB:CC:DD",          "Red Card (Carol)"},
    {"04:11:22:33:44:55:66", "NFC Tag (Dave)"},
    {"04:11:22:33",          "NFC Tag (Dave)"},
};
const int g_allowed_count = sizeof(g_allowed) / sizeof(g_allowed[0]);

/* ---------------------------------------------------------------
   ALLOW-LIST LOOKUP
--------------------------------------------------------------- */
const allowed_user_t *find_user(const char *uid)
{
    for (int i = 0; i < g_allowed_count; i++) {
        if (strcmp(g_allowed[i].uid, uid) == 0) {
            return &g_allowed[i];
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------
   OCCUPANT LIST  (call with g_state_lock held)
--------------------------------------------------------------- */
int find_occupant(const char *uid)
{
    for (int i = 0; i < g_occupant_count; i++) {
        if (strcmp(g_occupants[i].uid, uid) == 0) return i;
    }
    return -1;
}

bool add_occupant(const char *uid, const char *name)
{
    if (g_occupant_count >= MAX_PEOPLE) return false;

    int idx = g_occupant_count;
    strlcpy(g_occupants[idx].uid,  uid,  sizeof(g_occupants[0].uid));
    strlcpy(g_occupants[idx].name, name, sizeof(g_occupants[0].name));
    g_occupants[idx].entered = time(NULL);
    g_occupant_count++;
    return true;
}

void remove_occupant(int index)
{
    for (int i = index; i < g_occupant_count - 1; i++) {
        g_occupants[i] = g_occupants[i + 1];
    }
    g_occupant_count--;
}
