#ifndef SERENITAS_DEBUG_H
#define SERENITAS_DEBUG_H

#include <stdio.h>

#ifdef SERENITAS_DEBUG
#define logDebug(...) ((void)(fprintf(stderr, __VA_ARGS__), fflush(stderr)))
#else
#define logDebug(...) ((void)0)
#endif

#define logError(...) ((void)(fprintf(stderr, __VA_ARGS__), fflush(stderr)))

#endif
