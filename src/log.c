#include "log.h"

#if ENABLE_LOGGING == 1
#include <psp2/io/fcntl.h>
#include <psp2/types.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>

void LOG(const char *str, ...)
{
	static SceUID logfd = -1;
	char buff[256];
	va_list arglist;
	int len;

	if (logfd == -1) {
		logfd = sceIoOpen("ux0:dump/vitacompanion.txt", SCE_O_APPEND |
			SCE_O_CREAT | SCE_O_WRONLY, 0666);
	}

	va_start(arglist, str);
	len = vsnprintf(buff, sizeof(buff), str, arglist);
	va_end(arglist);
	if (len > 0) {
		if ((size_t)len >= sizeof(buff))
			len = (int)sizeof(buff) - 1;
		sceIoWrite(logfd, buff, len);
	}
}

void LOG_CALLBACK(const char *str)
{
	if (str)
		LOG("%s", str);
}
#endif
