#include "menu.h"
#include "globals.h"
#include "auth.h"
#include "occupants.h"
#include "sd_log.h"
#include "rooms.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"

/* ---------------------------------------------------------------
   PRINT HELPERS
--------------------------------------------------------------- */
static void print_help(void)
{
    printf("\nCoCIS Single-ESP32 commands:\n");
    printf("  status                      show hub, rooms and state\n");
    printf("  list                        list people currently inside\n");
    printf("  users                       show the RFID allow-list\n");
    printf("  logs [n]                    show the last n SD log lines\n");
    printf("  login <pin>                 unlock admin commands for 60 s\n");
    printf("  logout                      lock admin commands\n");
    printf("  schedule                    show the access schedule\n");
    printf("  schedule <HH:MM> <HH:MM>    change the schedule (admin)\n");
    printf("  settime YYYY-MM-DD HH:MM:SS set the clock (admin)\n");
    printf("  setpin <old> <new>          change the admin PIN (admin)\n");
    printf("  room1 on|off|auto|status    control Room 1 lighting (admin)\n");
    printf("  room2 on|off|auto|status    control Room 2 lighting (admin)\n");
    printf("  reboot                      restart the controller (admin)\n");
    printf("  help                        show this help\n\n");
    fflush(stdout);
}

static void print_status(void)
{
    char timestamp[32], start[8], end[8];
    fmt_time(timestamp, sizeof(timestamp));
    fmt_hhmm(g_sched_start_min, start, sizeof(start));
    fmt_hhmm(g_sched_end_min,   end,   sizeof(end));

    printf("\n=== CoCIS Single ESP32 ===\n");
    printf("Time       : %s\n", timestamp);
    printf("Schedule   : %s-%s (%s)\n", start, end,
           access_window_open() ? "OPEN" : "CLOSED");
    printf("SD card    : %s\n", g_sd_ok ? "mounted" : "not mounted");
    printf("Admin      : %s\n", is_authenticated() ? "unlocked" : "locked");

    xSemaphoreTake(g_state_lock, portMAX_DELAY);
    printf("Inside     : %d/%d\n", g_occupant_count, MAX_PEOPLE);
    for (int i = 0; i < g_occupant_count; i++) {
        char when[32];
        struct tm time_in;
        localtime_r(&g_occupants[i].entered, &time_in);
        strftime(when, sizeof(when), "%H:%M:%S", &time_in);
        printf("  %2d. %-22s %s since %s\n",
               i + 1, g_occupants[i].name, g_occupants[i].uid, when);
    }
    xSemaphoreGive(g_state_lock);

    xSemaphoreTake(g_room_lock, portMAX_DELAY);
    printf("Room 1     : occupied=%d light=%d mode=%s\n",
           g_room1_occupied, g_room1_light, g_room1_mode);
    printf("Room 2     : occupied=%d light=%d mode=%s\n",
           g_room2_occupied, g_room2_light, g_room2_mode);
    xSemaphoreGive(g_room_lock);

    printf("==========================\n\n");
    fflush(stdout);
}

static void print_users(void)
{
    printf("\nRFID allow-list:\n");
    for (int i = 0; i < g_allowed_count; i++) {
        printf("  %-24s %s\n", g_allowed[i].uid, g_allowed[i].name);
    }
    printf("  All other cards are denied.\n\n");
    fflush(stdout);
}

static void tail_logs(int n)
{
    if (!g_sd_ok) {
        printf("SD card is not mounted.\n");
        fflush(stdout);
        return;
    }

    if (n <= 0) n = 10;
    if (n > 50) n = 50;

    xSemaphoreTake(g_sd_lock, portMAX_DELAY);

    FILE *file = fopen(SD_LOG_FILE, "r");
    if (file == NULL) {
        xSemaphoreGive(g_sd_lock);
        printf("No log file yet.\n");
        fflush(stdout);
        return;
    }

    fseek(file, 0, SEEK_END);
    long size  = ftell(file);
    long start = (size > 4096) ? (size - 4096) : 0;
    fseek(file, start, SEEK_SET);

    char   buffer[4097];
    size_t bytes_read = fread(buffer, 1, sizeof(buffer) - 1, file);
    buffer[bytes_read] = '\0';
    fclose(file);
    xSemaphoreGive(g_sd_lock);

    char *text = buffer;
    if (start > 0) {
        char *newline = strchr(buffer, '\n');
        if (newline != NULL) text = newline + 1;
    }

    char *lines[64];
    int   count = 0;
    char *save  = NULL;
    char *token = strtok_r(text, "\n", &save);
    while (token != NULL && count < 64) {
        lines[count++] = token;
        token = strtok_r(NULL, "\n", &save);
    }

    int from = (count > n) ? (count - n) : 0;
    printf("\nLast %d log line(s):\n", count - from);
    for (int i = from; i < count; i++) printf("%s\n", lines[i]);
    printf("\n");
    fflush(stdout);
}

/* ---------------------------------------------------------------
   COMMAND HANDLER
--------------------------------------------------------------- */
static void to_uppercase(char *text)
{
    for (int i = 0; text[i] != '\0'; i++) {
        if (text[i] >= 'a' && text[i] <= 'z') text[i] = text[i] - 'a' + 'A';
    }
}

static void handle_menu_line(char *line)
{
    while (*line == ' ' || *line == '\t') line++;

    size_t len = strlen(line);
    while (len > 0 &&
           (line[len-1] == '\r' || line[len-1] == '\n' ||
            line[len-1] == ' '  || line[len-1] == '\t')) {
        line[--len] = '\0';
    }
    if (len == 0) return;

    char *command = line;
    char *args    = strchr(line, ' ');
    if (args != NULL) {
        *args++ = '\0';
        while (*args == ' ') args++;
    } else {
        args = line + strlen(line);
    }

    if (strcmp(command, "help") == 0 || strcmp(command, "?") == 0) {
        print_help();

    } else if (strcmp(command, "status") == 0 || strcmp(command, "list") == 0) {
        print_status();

    } else if (strcmp(command, "users") == 0) {
        print_users();

    } else if (strcmp(command, "logs") == 0) {
        tail_logs((*args != '\0') ? atoi(args) : 10);

    } else if (strcmp(command, "login") == 0) {
        if (*args != '\0' && strcmp(args, g_admin_pin) == 0) {
            auth_ok();
            printf("Admin unlocked for 60 s.\n");
        } else {
            log_access("AUTH", "-", "admin", g_occupant_count, "DENIED_PIN");
            printf("Wrong PIN.\n");
        }
        fflush(stdout);

    } else if (strcmp(command, "logout") == 0) {
        g_authenticated = false;
        printf("Admin locked.\n");
        fflush(stdout);

    } else if (strcmp(command, "schedule") == 0) {
        char start[8], end[8];
        fmt_hhmm(g_sched_start_min, start, sizeof(start));
        fmt_hhmm(g_sched_end_min,   end,   sizeof(end));

        if (*args == '\0') {
            printf("Access schedule: %s-%s (%s)\n", start, end,
                   access_window_open() ? "OPEN" : "CLOSED");
            fflush(stdout);
            return;
        }
        if (!is_authenticated()) {
            printf("Login first: login <pin>\n"); fflush(stdout); return;
        }
        int h1, m1, h2, m2;
        if (sscanf(args, "%d:%d %d:%d", &h1, &m1, &h2, &m2) != 4 ||
            h1 < 0 || h1 > 23 || h2 < 0 || h2 > 23 ||
            m1 < 0 || m1 > 59 || m2 < 0 || m2 > 59) {
            printf("Usage: schedule HH:MM HH:MM\n"); fflush(stdout); return;
        }
        g_sched_start_min = h1 * 60 + m1;
        g_sched_end_min   = h2 * 60 + m2;
        config_save();
        printf("Schedule saved: %02d:%02d-%02d:%02d\n", h1, m1, h2, m2);
        fflush(stdout);

    } else if (strcmp(command, "settime") == 0) {
        if (!is_authenticated()) {
            printf("Login first: login <pin>\n"); fflush(stdout); return;
        }
        int year, month, day, hour, minute, second;
        if (sscanf(args, "%d-%d-%d %d:%d:%d",
                   &year, &month, &day, &hour, &minute, &second) != 6 ||
            year < 2020 || month < 1 || month > 12 ||
            day  < 1    || day   > 31 ||
            hour < 0    || hour  > 23 ||
            minute < 0  || minute > 59 ||
            second < 0  || second > 59) {
            printf("Usage: settime YYYY-MM-DD HH:MM:SS\n"); fflush(stdout); return;
        }
        struct tm ts = {
            .tm_year = year - 1900, .tm_mon  = month - 1, .tm_mday = day,
            .tm_hour = hour,        .tm_min  = minute,    .tm_sec  = second,
            .tm_isdst = -1
        };
        time_t t = mktime(&ts);
        if (t < 0) t = time(NULL);
        struct timeval tv = {.tv_sec = t, .tv_usec = 0};
        settimeofday(&tv, NULL);
        char timestamp[32];
        fmt_time(timestamp, sizeof(timestamp));
        printf("Clock set to %s\n", timestamp);
        fflush(stdout);

    } else if (strcmp(command, "setpin") == 0) {
        if (!is_authenticated()) {
            printf("Login first: login <pin>\n"); fflush(stdout); return;
        }
        char old_pin[8] = "", new_pin[8] = "";
        if (sscanf(args, "%7s %7s", old_pin, new_pin) != 2) {
            printf("Usage: setpin <old> <new>\n"); fflush(stdout); return;
        }
        if (strcmp(old_pin, g_admin_pin) != 0) {
            printf("Wrong current PIN.\n"); fflush(stdout); return;
        }
        if (!valid_pin(new_pin)) {
            printf("New PIN must be 4-7 digits.\n"); fflush(stdout); return;
        }
        strlcpy(g_admin_pin, new_pin, sizeof(g_admin_pin));
        config_save();
        auth_ok();
        printf("PIN updated.\n");
        fflush(stdout);

    } else if (strcmp(command, "room1") == 0 || strcmp(command, "room2") == 0) {
        if (!is_authenticated()) {
            printf("Login first: login <pin>\n"); fflush(stdout); return;
        }
        int room = command[4] - '0';
        char action[16] = "";
        if (sscanf(args, "%15s", action) != 1) {
            printf("Usage: %s on|off|auto|status\n", command); fflush(stdout); return;
        }
        to_uppercase(action);

        if (strcmp(action, "STATUS") == 0) {
            xSemaphoreTake(g_room_lock, portMAX_DELAY);
            if (room == 1) {
                printf("Room 1: occupied=%d light=%d mode=%s\n",
                       g_room1_occupied, g_room1_light, g_room1_mode);
            } else {
                printf("Room 2: occupied=%d light=%d mode=%s\n",
                       g_room2_occupied, g_room2_light, g_room2_mode);
            }
            xSemaphoreGive(g_room_lock);
        } else if (strcmp(action, "ON")   == 0 ||
                   strcmp(action, "OFF")  == 0 ||
                   strcmp(action, "AUTO") == 0) {
            room_set_mode(room, action);
            printf("Room %d set to %s\n", room, action);
        } else {
            printf("Usage: %s on|off|auto|status\n", command);
        }
        fflush(stdout);

    } else if (strcmp(command, "reboot") == 0) {
        if (!is_authenticated()) {
            printf("Login first: login <pin>\n"); fflush(stdout); return;
        }
        printf("Rebooting...\n");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();

    } else {
        printf("Unknown command '%s'. Type help.\n", command);
        fflush(stdout);
    }
}

/* ---------------------------------------------------------------
   MENU TASK
--------------------------------------------------------------- */
void menu_task(void *arg)
{
    (void)arg;

    char   line[160];
    size_t length      = 0;
    bool   last_was_cr = false;

    print_help();
    printf("cocis> ");
    fflush(stdout);

    while (true) {
        int c = fgetc(stdin);

        if (c == EOF) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (c == '\r' || c == '\n') {
            if (c == '\n' && last_was_cr) { last_was_cr = false; continue; }
            last_was_cr = (c == '\r');
            printf("\n");
            if (length > 0) {
                line[length] = '\0';
                handle_menu_line(line);
                length = 0;
            }
            printf("cocis> ");
            fflush(stdout);
            continue;
        }

        last_was_cr = false;

        if (c == '\b' || c == 0x7F) {
            if (length > 0) { length--; printf("\b \b"); fflush(stdout); }
            continue;
        }

        if (c >= 0x20 && c < 0x7F && length + 1 < sizeof(line)) {
            line[length++] = (char)c;
            putchar(c);
            fflush(stdout);
        }
    }
}
