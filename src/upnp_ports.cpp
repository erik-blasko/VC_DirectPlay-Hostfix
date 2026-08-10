#include <winsock2.h>
#include <windows.h>
#include <objbase.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "MinHook.h"
#include "log.hpp"
#include "upnp_ports.hpp"

/* --------------------------------------------------------------------------
 * UPnP: open the GameSpy query port for an internet host.
 *
 * Why this is needed
 * ------------------
 * A Vietcong server advertises itself to the GameSpy master with
 *   \heartbeat\<queryport>\gamename\vietcong
 * and the master then probes that port with \status\. Windows' DirectPlay NAT
 * helper punches a hole for DirectPlay's OWN socket (the game port) but knows
 * nothing about the GameSpy query socket, which logs.dll opens itself. So a
 * host behind a NAT registers with the master, is listed, and then never
 * answers - it looks dead to everyone.
 *
 * How
 * ---
 * We reuse the very same Windows component DirectPlay uses, dpnathlp.dll, and
 * simply register one more port with it. No SSDP or SOAP of our own, and the
 * router sees the same client it already deals with.
 *
 * The port number is never hardcoded: it is read out of the server's OWN
 * outgoing heartbeat, so whatever the operator configures is what gets mapped.
 * The game port is learned the same way, from the \hostport\ field of the
 * status reply, and mapped for TCP (the NAT helper only does UDP for it).
 *
 * Interface notes - all MEASURED, not from documentation
 * ------------------------------------------------------
 * No SDK ships headers for IDirectPlayNATHelp, and the two sources that
 * describe it are both wrong for current Windows:
 *   - Wine's dpnathlp.h IID {154940B6-..} is rejected with E_NOINTERFACE;
 *     the shipped DLL wants {3B743591-..} (what dpnet.dll itself passes).
 *   - RegisterPorts takes SIX parameters after `this`, and its order differs
 *     from both Wine and MSDN. Read off dpnet.dll's own call site:
 *         RegisterPorts(addrs, sizeofOneSockaddr, count, leaseMs, &h, flags)
 *     The second parameter is the SIZE of one sockaddr (16 for IPv4), not the
 *     count. Getting it wrong yields a permanent E_INVALIDARG.
 *   - GetCaps(caps, 0) only ever returns a cached, empty result. Discovery is
 *     started by GetCaps(caps, 1) (UPDATESERVERSTATUS) - without that flag the
 *     helper never even sends an M-SEARCH.
 * -------------------------------------------------------------------------- */

typedef HANDLE DPNHHANDLE;

typedef struct {
	DWORD dwSize;
	DWORD dwFlags;
	DWORD dwNumRegisteredPorts;
	DWORD dwMinLeaseTimeRemaining;
	DWORD dwRecommendedGetCapsInterval;
} DPNHCAPS;

#define DPNHCAPS_GATEWAYPRESENT   0x0002
#define DPNHCAPS_CANREGISTER      0x0010

#define DPNHREG_TCP               0x01
#define DPNHREG_FIXEDPORTS        0x02

#define DPNHGETCAPS_UPDATE        0x01   /* without this, no discovery happens */

/* dpnet.dll registers with a one hour lease; the helper renews it itself. It
 * is also our safety net: if the process is killed and we never get to
 * deregister, the router drops the mapping within the hour. */
#define LEASE_MS                  3600000

struct INatHelp;
typedef struct INatHelpVtbl {
	HRESULT (STDMETHODCALLTYPE *QueryInterface)(struct INatHelp*, REFIID, void**);
	ULONG   (STDMETHODCALLTYPE *AddRef)(struct INatHelp*);
	ULONG   (STDMETHODCALLTYPE *Release)(struct INatHelp*);
	HRESULT (STDMETHODCALLTYPE *Initialize)(struct INatHelp*, DWORD);
	HRESULT (STDMETHODCALLTYPE *Close)(struct INatHelp*, DWORD);
	HRESULT (STDMETHODCALLTYPE *GetCaps)(struct INatHelp*, DPNHCAPS*, DWORD);
	HRESULT (STDMETHODCALLTYPE *RegisterPorts)(struct INatHelp*, const SOCKADDR*,
	                                           DWORD, DWORD, DWORD, DPNHHANDLE*, DWORD);
	HRESULT (STDMETHODCALLTYPE *GetRegisteredAddresses)(struct INatHelp*, DPNHHANDLE,
	                                           SOCKADDR*, DWORD*, DWORD*, DWORD*, DWORD);
	HRESULT (STDMETHODCALLTYPE *DeregisterPorts)(struct INatHelp*, DPNHHANDLE, DWORD);
} INatHelpVtbl;
struct INatHelp { INatHelpVtbl* lpVtbl; };

typedef HRESULT (WINAPI *PFN_NATHELPCREATE)(const GUID*, void**);

/* The IID dpnet.dll itself passes to DirectPlayNATHelpCreate. */
static const GUID IID_NatHelp =
	{ 0x3B743591, 0x791B, 0x4864, { 0x9E,0xE3,0x55,0xE8,0x89,0x40,0x97,0x81 } };

/* ---------------- state ---------------- */

#define MAX_MAPPINGS 4

struct Mapping {
	DPNHHANDLE handle;
	u_short    port;
	bool       tcp;
};

static struct INatHelp *g_nat        = NULL;
static HMODULE          g_natLib     = NULL;
static Mapping          g_maps[MAX_MAPPINGS];
static int              g_mapCount   = 0;
static CRITICAL_SECTION g_lock;
static bool             g_lockReady  = false;
static bool             g_disabled   = false;

/* Ports discovered from outgoing traffic, handed to the worker thread. */
static volatile LONG    g_wantQuery  = 0;   /* UDP */
static volatile LONG    g_wantGame   = 0;   /* TCP */
static HANDLE           g_wakeup     = NULL;
static volatile LONG    g_stop       = 0;
static SOCKET           g_querySock  = INVALID_SOCKET;

/* Releasing has to be possible even while the worker is busy discovering a
 * gateway, which can take up to 20 s when there is none. Blocking the game's
 * own closesocket() for that long would look like a hang, so if the lock is
 * held we hand the job to the worker and wait only briefly. */
static volatile LONG    g_wantRelease = 0;
static HANDLE           g_released    = NULL;

/* hooked originals */
static int (WSAAPI *orig_sendto)(SOCKET, const char*, int, int, const struct sockaddr*, int) = NULL;
static int (WSAAPI *orig_closesocket)(SOCKET) = NULL;
static int (WSAAPI *orig_WSACleanup)(void) = NULL;

/* ---------------- helpers ---------------- */

/* Parse "\<key>\<number>\" out of a GameSpy datagram. Returns 0 if absent. */
static u_short gs_field(const char *buf, int len, const char *key)
{
	char pat[32];
	int  plen = _snprintf(pat, sizeof(pat), "\\%s\\", key);
	if (plen <= 0 || len <= plen)
		return 0;

	for (int i = 0; i + plen < len; ++i)
	{
		if (_strnicmp(buf + i, pat, plen) != 0)
			continue;
		int v = 0, j = i + plen;
		for (; j < len && buf[j] >= '0' && buf[j] <= '9'; ++j)
			v = v * 10 + (buf[j] - '0');
		if (j == i + plen || v <= 0 || v > 65535)
			return 0;
		return (u_short)v;
	}
	return 0;
}

static bool already_mapped(u_short port, bool tcp)
{
	for (int i = 0; i < g_mapCount; ++i)
		if (g_maps[i].port == port && g_maps[i].tcp == tcp)
			return true;
	return false;
}

/* ---------------- NAT helper ---------------- */

static bool nat_open()
{
	if (g_nat != NULL)
		return true;

	g_natLib = LoadLibraryA("dpnathlp.dll");
	if (g_natLib == NULL)
	{
		/* Expected on Wine and on Windows installs without DirectPlay. Not an
		 * error - we simply do nothing, which is the pre-existing behaviour. */
		shim_log("upnp: dpnathlp.dll not present - UPnP disabled");
		return false;
	}

	PFN_NATHELPCREATE create =
		(PFN_NATHELPCREATE)GetProcAddress(g_natLib, "DirectPlayNATHelpCreate");
	if (create == NULL)
	{
		shim_log("upnp: DirectPlayNATHelpCreate missing");
		return false;
	}

	void *p = NULL;
	HRESULT hr = create(&IID_NatHelp, &p);
	if (FAILED(hr) || p == NULL)
	{
		shim_log("upnp: create failed hr=0x%08lX", (unsigned long)hr);
		return false;
	}
	g_nat = (struct INatHelp*)p;

	hr = g_nat->lpVtbl->Initialize(g_nat, 0);
	if (FAILED(hr))
	{
		shim_log("upnp: Initialize failed hr=0x%08lX", (unsigned long)hr);
		g_nat->lpVtbl->Release(g_nat);
		g_nat = NULL;
		return false;
	}
	return true;
}

/* Poll until the helper reports a usable gateway. Measured: it answers on the
 * second call, roughly a second in. */
static bool nat_wait_gateway(int seconds)
{
	for (int i = 0; i < seconds && !g_stop; ++i)
	{
		DPNHCAPS caps;
		memset(&caps, 0, sizeof(caps));
		caps.dwSize = sizeof(caps);
		if (SUCCEEDED(g_nat->lpVtbl->GetCaps(g_nat, &caps, DPNHGETCAPS_UPDATE)))
		{
			if (caps.dwFlags & DPNHCAPS_GATEWAYPRESENT)
			{
				shim_log("upnp: gateway found, caps=0x%04lX%s",
					(unsigned long)caps.dwFlags,
					(caps.dwFlags & DPNHCAPS_CANREGISTER) ? " (can register)" : "");
				return true;
			}
		}
		Sleep(1000);
	}
	shim_log("upnp: no UPnP gateway on this network - nothing to do");
	return false;
}

static void nat_register(u_short port, bool tcp)
{
	if (g_mapCount >= MAX_MAPPINGS || already_mapped(port, tcp))
		return;

	struct sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family      = AF_INET;
	sa.sin_port        = htons(port);
	sa.sin_addr.s_addr = INADDR_ANY;      /* helper picks the outbound interface */

	DPNHHANDLE h = NULL;
	DWORD flags = DPNHREG_FIXEDPORTS | (tcp ? DPNHREG_TCP : 0);

	/* FIXEDPORTS asks for the same external port number; if the router cannot
	 * honour that, retry without it rather than give up. */
	HRESULT hr = g_nat->lpVtbl->RegisterPorts(g_nat, (const SOCKADDR*)&sa,
	                                          sizeof(sa), 1, LEASE_MS, &h, flags);
	if (FAILED(hr))
	{
		flags &= ~DPNHREG_FIXEDPORTS;
		hr = g_nat->lpVtbl->RegisterPorts(g_nat, (const SOCKADDR*)&sa,
		                                  sizeof(sa), 1, LEASE_MS, &h, flags);
	}

	if (FAILED(hr) || h == NULL)
	{
		shim_log("upnp: RegisterPorts %u/%s failed hr=0x%08lX",
			(unsigned)port, tcp ? "TCP" : "UDP", (unsigned long)hr);
		return;
	}

	g_maps[g_mapCount].handle = h;
	g_maps[g_mapCount].port   = port;
	g_maps[g_mapCount].tcp    = tcp;
	++g_mapCount;
	shim_log("upnp: mapped %u/%s (flags=0x%02lX)",
		(unsigned)port, tcp ? "TCP" : "UDP", (unsigned long)flags);
}

static void nat_release_all()
{
	if (g_nat == NULL)
		return;
	for (int i = 0; i < g_mapCount; ++i)
	{
		HRESULT hr = g_nat->lpVtbl->DeregisterPorts(g_nat, g_maps[i].handle, 0);
		shim_log("upnp: released %u/%s hr=0x%08lX", (unsigned)g_maps[i].port,
			g_maps[i].tcp ? "TCP" : "UDP", (unsigned long)hr);
	}
	g_mapCount = 0;
}

/* Release everything without ever blocking the caller for long. */
static void release_now()
{
	if (!g_lockReady || g_mapCount == 0)
		return;

	if (TryEnterCriticalSection(&g_lock))
	{
		nat_release_all();
		LeaveCriticalSection(&g_lock);
		return;
	}

	InterlockedExchange(&g_wantRelease, 1);
	if (g_wakeup != NULL)
		SetEvent(g_wakeup);
	if (g_released != NULL)
		WaitForSingleObject(g_released, 3000);
}

/* ---------------- worker ---------------- */

/* The registration has to happen off the caller's thread: discovering the
 * gateway takes on the order of a second, and it is triggered from inside
 * sendto() - blocking there would stall the server's network loop. */
static DWORD WINAPI worker(LPVOID)
{
	CoInitializeEx(NULL, COINIT_MULTITHREADED);

	bool ready = false;
	while (!g_stop)
	{
		WaitForSingleObject(g_wakeup, 5000);
		if (g_stop)
			break;

		if (InterlockedExchange(&g_wantRelease, 0))
		{
			EnterCriticalSection(&g_lock);
			nat_release_all();
			LeaveCriticalSection(&g_lock);
			if (g_released != NULL)
				SetEvent(g_released);
			continue;
		}

		LONG q = InterlockedExchange(&g_wantQuery, 0);
		LONG g = InterlockedExchange(&g_wantGame, 0);
		if (q == 0 && g == 0)
			continue;

		EnterCriticalSection(&g_lock);
		if (!ready)
		{
			if (nat_open())
				ready = nat_wait_gateway(20);
			if (!ready)
				g_disabled = true;          /* stop trying for this process */
		}
		if (ready)
		{
			if (q) nat_register((u_short)q, false);
			if (g) nat_register((u_short)g, true);
		}
		LeaveCriticalSection(&g_lock);

		if (g_disabled)
			break;
	}

	CoUninitialize();
	return 0;
}

static void want_port(volatile LONG *slot, u_short port)
{
	if (g_disabled || port == 0)
		return;
	if (InterlockedExchange(slot, port) != port && g_wakeup != NULL)
		SetEvent(g_wakeup);
}

/* ---------------- hooks ---------------- */

static int WSAAPI hook_sendto(SOCKET s, const char *buf, int len, int flags,
                              const struct sockaddr *to, int tolen)
{
	/* GameSpy messages are short ASCII and start with a backslash. Cheap test
	 * first so we add nothing measurable to the game's send path. */
	if (len > 12 && len < 1400 && buf != NULL && buf[0] == '\\' && !g_disabled)
	{
		u_short p = gs_field(buf, len, "heartbeat");
		if (p != 0)
		{
			g_querySock = s;              /* so we can notice it closing */
			want_port(&g_wantQuery, p);
		}
		else
		{
			/* Status replies carry the DirectPlay port. Windows' NAT helper
			 * maps it for UDP only, so we add the TCP side. */
			p = gs_field(buf, len, "hostport");
			if (p != 0)
				want_port(&g_wantGame, p);
		}
	}
	return orig_sendto(s, buf, len, flags, to, tolen);
}

/* Primary cleanup trigger: an orderly server shutdown closes its query socket
 * long before the DLL is torn down, so this is where we can still safely talk
 * to the router. */
static int WSAAPI hook_closesocket(SOCKET s)
{
	if (s != INVALID_SOCKET && s == g_querySock)
	{
		g_querySock = INVALID_SOCKET;
		release_now();
	}
	return orig_closesocket(s);
}

static int WSAAPI hook_WSACleanup(void)
{
	release_now();
	return orig_WSACleanup();
}

/* ---------------- entry points ---------------- */

void UPnPPorts_Install()
{
	const char *off = getenv("DPSHIM_NO_UPNP");
	if (off != NULL && *off != '0')
	{
		shim_log("upnp: disabled via DPSHIM_NO_UPNP");
		g_disabled = true;
		return;
	}

	InitializeCriticalSection(&g_lock);
	g_lockReady = true;

	HMODULE ws = GetModuleHandleA("ws2_32.dll");
	if (ws == NULL)
		ws = LoadLibraryA("ws2_32.dll");
	if (ws == NULL)
	{
		shim_log("upnp: ws2_32 unavailable");
		g_disabled = true;
		return;
	}

	/* MinHook is already initialised by the self-join hook; calling
	 * MH_Initialize twice returns MH_ERROR_ALREADY_INITIALIZED, which is fine. */
	MH_Initialize();

	struct { const char *name; LPVOID detour; LPVOID *orig; } hooks[] = {
		{ "sendto",       (LPVOID)&hook_sendto,       (LPVOID*)&orig_sendto },
		{ "closesocket",  (LPVOID)&hook_closesocket,  (LPVOID*)&orig_closesocket },
		{ "WSACleanup",   (LPVOID)&hook_WSACleanup,   (LPVOID*)&orig_WSACleanup },
	};

	for (int i = 0; i < 3; ++i)
	{
		LPVOID target = (LPVOID)GetProcAddress(ws, hooks[i].name);
		if (target == NULL ||
		    MH_CreateHook(target, hooks[i].detour, hooks[i].orig) != MH_OK ||
		    MH_EnableHook(target) != MH_OK)
		{
			shim_log("upnp: hook %s FAILED - UPnP disabled", hooks[i].name);
			g_disabled = true;
			return;
		}
	}

	g_wakeup   = CreateEventA(NULL, FALSE, FALSE, NULL);
	g_released = CreateEventA(NULL, FALSE, FALSE, NULL);
	HANDLE th = CreateThread(NULL, 0, worker, NULL, 0, NULL);
	if (th != NULL)
		CloseHandle(th);

	shim_log("upnp: watching for the server's heartbeat");
}

void UPnPPorts_Shutdown(bool processExiting)
{
	InterlockedExchange(&g_stop, 1);
	if (g_wakeup != NULL)
		SetEvent(g_wakeup);

	/* During process teardown other DLLs may already be gone and we hold the
	 * loader lock, so COM calls are not safe. The lease is the fallback there:
	 * the router drops the mapping within the hour. In the normal case the
	 * closesocket hook has already released everything long before this. */
	if (processExiting || !g_lockReady)
		return;

	EnterCriticalSection(&g_lock);
	nat_release_all();
	if (g_nat != NULL)
	{
		g_nat->lpVtbl->Close(g_nat, 0);
		g_nat->lpVtbl->Release(g_nat);
		g_nat = NULL;
	}
	LeaveCriticalSection(&g_lock);
}
