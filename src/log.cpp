#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "log.hpp"

static FILE *g_fh         = NULL;
static bool  g_inited     = false;
static CRITICAL_SECTION g_cs;
static bool  g_csReady    = false;

static void ensure_init()
{
	if (!g_csReady)
	{
		InitializeCriticalSection(&g_cs);
		g_csReady = true;
	}
	if (!g_inited)
	{
		g_inited = true;
		const char *path = getenv("DPSHIM_LOG");
		if (path != NULL)
		{
			g_fh = fopen(path, "a");
			if (g_fh != NULL)
				setbuf(g_fh, NULL);
		}
	}
}

void shim_log(const char *fmt, ...)
{
	ensure_init();
	if (g_fh == NULL)
		return;

	EnterCriticalSection(&g_cs);
	fprintf(g_fh, "[tid=%lu t=%lu] ",
		(unsigned long)GetCurrentThreadId(), (unsigned long)GetTickCount());
	va_list ap;
	va_start(ap, fmt);
	vfprintf(g_fh, fmt, ap);
	va_end(ap);
	fprintf(g_fh, "\n");
	LeaveCriticalSection(&g_cs);
}
