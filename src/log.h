#ifndef REDIS_DEBUG_LOG_H_
#define REDIS_DEBUG_LOG_H_

#include <pthread.h>

typedef enum LogLevel {
    LOG_LEVEL_FATAL = 0,
    LOG_LEVEL_ERROR = 1,
    LOG_LEVEL_WARN  = 2,
    LOG_LEVEL_INFO  = 3,
    LOG_LEVEL_DEBUG = 4,
    LOG_LEVEL_TRACE = 5,
} LOG_LEVEL;

#ifdef ENABLE_DEBUG

extern void logMessage(unsigned int level, const char *filename, int line, const char *function, pthread_t tid, const char *fmt, ...);
#define LOG_FILE                   (strrchr("/" __FILE__,'/')+1)
#define LOG_LEVEL(level)           LOG_LEVEL_##level, LOG_FILE, __LINE__, __func__, pthread_self()
#define LOG(level, ...)      logMessage(LOG_LEVEL(level), __VA_ARGS__)
#else 
#define LOG(level, ...)         
#endif

#endif