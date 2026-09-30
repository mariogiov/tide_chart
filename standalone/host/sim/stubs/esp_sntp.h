#pragma once
enum { SNTP_SYNC_STATUS_RESET, SNTP_SYNC_STATUS_COMPLETED };
int sntp_get_sync_status();
void configTzTime(const char *tz, const char *, const char *);
