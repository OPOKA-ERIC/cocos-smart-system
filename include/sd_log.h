#ifndef SD_LOG_H
#define SD_LOG_H

void sd_setup(void);
void log_access(const char *event, const char *uid,
                const char *name, int inside, const char *result);

#endif
