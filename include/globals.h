#ifndef GLOBALS_H
#define GLOBALS_H

/* Shared data types, global state, and mutexes used across all modules. */

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/spi_master.h"
#include "rc522.h"
#include "config.h"

/* ---------------------------------------------------------------
   DATA TYPES
--------------------------------------------------------------- */
typedef struct {
    char uid[24];
    char name[24];
} allowed_user_t;

typedef struct {
    char   uid[24];
    char   name[24];
    time_t entered;
} occupant_t;

/* ---------------------------------------------------------------
   RFID ALLOW-LIST  (defined in occupants.c)
--------------------------------------------------------------- */
extern const allowed_user_t g_allowed[];
extern const int             g_allowed_count;

/* ---------------------------------------------------------------
   ENTRANCE STATE
--------------------------------------------------------------- */
extern rc522_t           g_entry_reader;
extern rc522_t           g_exit_reader;
extern bool              g_entry_ok;
extern bool              g_exit_ok;

extern SemaphoreHandle_t g_state_lock;   /* protects occupant list */
extern SemaphoreHandle_t g_sd_lock;      /* protects SD file */
extern SemaphoreHandle_t g_room_lock;    /* protects room variables */

extern bool              g_sd_ok;
extern int               g_sched_start_min;
extern int               g_sched_end_min;
extern char              g_admin_pin[12];
extern bool              g_authenticated;
extern TickType_t        g_auth_until;

extern occupant_t        g_occupants[MAX_PEOPLE];
extern int               g_occupant_count;

/* ---------------------------------------------------------------
   ROOM STATE
--------------------------------------------------------------- */
extern bool       g_room1_occupied;
extern bool       g_room1_light;
extern char       g_room1_mode[8];
extern TickType_t g_room1_last_motion;

extern bool       g_room2_occupied;
extern bool       g_room2_light;
extern char       g_room2_mode[8];
extern TickType_t g_room2_last_motion;

#endif
