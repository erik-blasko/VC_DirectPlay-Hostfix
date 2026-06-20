/* Vietcong DirectPlay host-fix shim.
 *
 * This dpnet.dll is registered (per-user COM) in place of the system one, but
 * it does NOT reimplement DirectPlay. Every COM class request is delegated
 * straight to the real system dpnet.dll, so the game uses 100% stock
 * DirectPlay8 objects and stays fully interoperable with ordinary players.
 *
 * The only reason this shim exists is that being loaded into the game process
 * gives us in-process code execution, from which we install a hook on
 * logs.dll's listen-host self-join (NET_internal_C430) to work around the
 * broken local host discovery (dpnsvr binding IPv6-only on this OS build).
 * See selfjoin_hook.cpp for the actual fix.
 */

#include <windows.h>
#include <objbase.h>
#include <stdio.h>
#include <string.h>

#include "log.hpp"
#include "selfjoin_hook.hpp"
#include "version.h"

typedef HRESULT (WINAPI *DllGetClassObject_t)(REFCLSID, REFIID, LPVOID*);
typedef HRESULT (WINAPI *DllCanUnloadNow_t)(void);

static HMODULE   g_realDpnet = NULL;
static CRITICAL_SECTION g_lock;
static bool      g_initDone  = false;

/* Load the genuine system dpnet.dll (SysWOW64 for our 32-bit process) by full
 * path, so the COM registry override (which now points at us) is bypassed. */
static HMODULE LoadRealDpnet()
{
	if (g_realDpnet != NULL)
		return g_realDpnet;

	char path[MAX_PATH];
	UINT n = GetSystemWow64DirectoryA(path, MAX_PATH);   /* always SysWOW64 */
	if (n == 0 || n >= MAX_PATH)
		n = GetSystemDirectoryA(path, MAX_PATH);         /* 32-bit proc -> redirected to SysWOW64 */
	if (n == 0 || n >= MAX_PATH)
		return NULL;

	strcat(path, "\\dpnet.dll");
	g_realDpnet = LoadLibraryA(path);
	shim_log("LoadRealDpnet(%s) = %p", path, (void*)g_realDpnet);
	return g_realDpnet;
}

/* One-time deferred init, run outside DllMain (loader-lock safe). */
static void EnsureInit()
{
	EnterCriticalSection(&g_lock);
	if (!g_initDone)
	{
		g_initDone = true;
		LoadRealDpnet();
		SelfJoinHook_Install();
	}
	LeaveCriticalSection(&g_lock);
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
	(void)lpvReserved;
	if (fdwReason == DLL_PROCESS_ATTACH)
	{
		InitializeCriticalSection(&g_lock);
		DisableThreadLibraryCalls(hinstDLL);
		shim_log("dpnet host-fix shim v%s loaded into pid %lu",
			SHIM_VERSION_STR, (unsigned long)GetCurrentProcessId());
	}
	return TRUE;
}

extern "C" HRESULT WINAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID *ppv)
{
	EnsureInit();

	HMODULE real = LoadRealDpnet();
	if (real == NULL)
		return CLASS_E_CLASSNOTAVAILABLE;

	DllGetClassObject_t fn = (DllGetClassObject_t)GetProcAddress(real, "DllGetClassObject");
	if (fn == NULL)
		return CLASS_E_CLASSNOTAVAILABLE;

	return fn(rclsid, riid, ppv);
}

extern "C" HRESULT WINAPI DllCanUnloadNow()
{
	/* We keep our hook installed for the lifetime of the process, so never
	 * allow COM to unload us. */
	return S_FALSE;
}
