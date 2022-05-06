// Copyright (c) 2022 Valtteri Koskivuori (vkoskiv). All rights reserved.

#include "logging.h"
#include <stddef.h>
#include <stdio.h>
#include <time.h>
#include <stdarg.h>

void logr(const char *fmt, ...) {
	if (!fmt) return;
	char buf[1024];
	int ret = 0;
	va_list vl;
	va_start(vl, fmt);
	ret += vsnprintf(buf, sizeof(buf), fmt, vl);
	va_end(vl);
	printf("%u %s", (unsigned)time(NULL), buf);
	if (ret > 1024) {
		printf("...\n");
	}
}

