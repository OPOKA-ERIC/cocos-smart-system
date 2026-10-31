#include "globals.h"

/* ---------------------------------------------------------------
   ENTRANCE STATE
--------------------------------------------------------------- */
rc522_t           g_entry_reader;
rc522_t           g_exit_reader;
bool              g_entry_ok          = false;
bool              g_exit_ok           = false;

SemaphoreHandle_t g_state_lock        = NULL;
SemaphoreHandle_t g_sd_lock           = NULL;
SemaphoreHandle_t g_room_lock         = NULL;

bool              g_sd_ok             = false;
int               g_sched_start_min   = ACCESS_START_HOUR_DEFAULT * 60;
int               g_sched_end_min     = ACCESS_END_HOUR_DEFAULT   * 60;
char              g_admin_pin[12]     = ADMIN_PIN_DEFAULT;
bool              g_authenticated     = false;
TickType_t        g_auth_until        = 0;

occupant_t        g_occupants[MAX_PEOPLE];
int               g_occupant_count    = 0;

/* ---------------------------------------------------------------
   ROOM STATE
--------------------------------------------------------------- */
bool       g_room1_occupied    = false;
bool       g_room1_light       = false;
char       g_room1_mode[8]     = "AUTO";
TickType_t g_room1_last_motion = 0;

bool       g_room2_occupied    = false;
bool       g_room2_light       = false;
char       g_room2_mode[8]     = "AUTO";
TickType_t g_room2_last_motion = 0;
