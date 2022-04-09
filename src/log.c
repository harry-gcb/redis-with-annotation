#include "log.h"

#ifdef ENABLE_DEBUG
#include <stdio.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#include <stdarg.h>

#include <sys/uio.h>

static const char *logstr[] = {
    "FATAL",
    "ERROR",
    "WARN",
    "INFO",
    "DEBUG",
    "TRACE",
};

static int console = 1;
static int fd = STDERR_FILENO;
static unsigned int logLevel = LOG_LEVEL_TRACE;
#ifndef CONSOLE_DEBUG
static const char *filename = "debug.log";
#endif

/*
Name            FG  BG
Black           30  40
Red             31  41
Green           32  42
Yellow          33  43
Blue            34  44
Magenta         35  45
Cyan            36  46
White           37  47
Bright Black    90  100
Bright Red      91  101
Bright Green    92  102
Bright Yellow   93  103
Bright Blue     94  104
Bright Magenta  95  105
Bright Cyan     96  106
Bright White    97  107
*/

#define LOG_DATA_LEN (4 * 1024)
#define LOG_HEAD_LEN 128
static char NEWLINE[1] = {'\n'};
static const char *nocolor = "\033[0m";
static const char *logcolor[] = {
    "\033[0;35m",
    "\033[0;31m",
    "\033[0;33m",
    "\033[0;37m",
    "\033[0;94m",
    "\033[0;32m",
};

#ifndef CONSOLE_DEBUG
__attribute__((constructor)) static void debug_constructor(void)
{
    fd = open(filename, O_RDWR | O_CREAT | O_APPEND, 0644);
    if (fd < 0)
    {
        return;
    }
    console = 0;
}

__attribute__((destructor)) static void debug_destructor(void)
{
    if (!(fd < 0))
    {
        close(fd);
    }
}
#endif

void setLogLevel(unsigned int level)
{
    logLevel = level;
}

void logMessage(unsigned int level, const char* filename, int line, const char* function,
                pthread_t tid, const char* fmt, ...)
{
    if (level > logLevel) {
        return;
    }
    struct timeval tv = {0};
    gettimeofday(&tv, NULL);
    struct tm *tm = localtime((const time_t *)&tv.tv_sec);
    if (!tm) {
        return;
    }

    char data[LOG_DATA_LEN] = {0};
    char head[LOG_HEAD_LEN] = {0};
    va_list args;
    va_start(args, fmt);
    int data_size = vsnprintf(data, sizeof(data), fmt, args);
    va_end(args);

    int head_size = 0;
    if (console) {
        head_size = snprintf(head,
                             sizeof(head),
                             "%s[%04d-%02d-%02d %02d:%02d:%02d.%06ld]%s %-5s "
                             "%s (%s:%d) [%ld] ",
                             logcolor[level],
                             tm->tm_year + 1900,
                             tm->tm_mon + 1,
                             tm->tm_mday,
                             tm->tm_hour,
                             tm->tm_min,
                             tm->tm_sec,
                             tv.tv_usec,
                             nocolor,
                             logstr[level],
                             function,
                             filename,
                             line,
                             tid);
    }
    else {
        head_size = snprintf(head,
                             sizeof(head),
                             "[%04d-%02d-%02d %02d:%02d:%02d.%06ld] %-5s %s "
                             "(%s:%d) [%ld] ",
                             tm->tm_year + 1900,
                             tm->tm_mon + 1,
                             tm->tm_mday,
                             tm->tm_hour,
                             tm->tm_min,
                             tm->tm_sec,
                             tv.tv_usec,
                             logstr[level],
                             function,
                             filename,
                             line,
                             tid);
    }
    struct iovec vec[3] = {0};
    vec[0].iov_base = head;
    vec[0].iov_len = head_size;
    vec[1].iov_base = data;
    vec[1].iov_len = data_size;
    vec[2].iov_base = NEWLINE;
    vec[2].iov_len = sizeof(NEWLINE);
    writev(fd, vec, 3);
}

#endif
