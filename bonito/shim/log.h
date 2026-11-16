#pragma once
#include <stdarg.h>
#include <stdint.h>

/*
 * Text log kept in RAM and shown on the screen (bonito has no UFS: eMMC).
 */
#define LOG_SIZE	(256 * 1024)

extern char log_buf[LOG_SIZE];
extern uint32_t log_len;

void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_vprintf(const char *fmt, va_list ap);
int fmt_vsnprintf(char *out, unsigned size, const char *fmt, va_list ap);
int fmt_snprintf(char *out, unsigned size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
