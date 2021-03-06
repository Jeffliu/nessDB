/*
 * nessDB storage engine
 * Copyright (c) 2011-2012, BohuTANG <overred.shuttler at gmail dot com>
 * All rights reserved.
 * Code is licensed with BSD. See COPYING.BSD file.
 *
 */

#ifndef _DEBUG_H
#define _DEBUG_H

#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>

typedef enum {LEVEL_DEBUG = 0, LEVEL_INFO, LEVEL_WARNING, LEVEL_ERROR} DEBUG_LEVEL;

void __debug(char *file, int line, DEBUG_LEVEL level, const char *fmt, ...);

#define __DEBUG(format, args...)\
	do { __debug(__FILE__, __LINE__, LEVEL_DEBUG, format, ##args); } while (0)

#define __ERROR(format, args...)\
	do { __debug(__FILE__, __LINE__, LEVEL_ERROR, format, ##args); } while (0)

#define __WARN(format, args...)\
	do { __debug(__FILE__, __LINE__, LEVEL_WARNING, format, ##args); } while (0)

#define __INFO(format, args...)\
	do { __debug(__FILE__, __LINE__, LEVEL_INFO, format, ##args); } while (0)

#define __PANIC(format, args...)\
	do { __debug(__FILE__, __LINE__, LEVEL_ERROR, format, ##args); exit(1); } while (0)

#endif
