#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "esp_timer.h"
#include "log_util.h"

log_level raop_loglevel = lINFO;
log_level util_loglevel = lINFO;

static char log_time_buf[32];

const char *logtime(void) {
	uint64_t us = esp_timer_get_time();
	uint32_t ms = (uint32_t)(us / 1000ULL);
	uint32_t sec = ms / 1000U;
	uint32_t rem_ms = ms % 1000U;
	snprintf(log_time_buf, sizeof(log_time_buf), "%" PRIu32 ".%03" PRIu32, sec, rem_ms);
	return log_time_buf;
}

void logprint(const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	vprintf(fmt, args);
	va_end(args);
}
