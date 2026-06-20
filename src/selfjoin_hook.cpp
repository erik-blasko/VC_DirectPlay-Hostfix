#include <windows.h>
#include <objbase.h>
#include <psapi.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include <dplay8.h>
#include <dpaddr.h>

#include "MinHook.h"
#include "log.hpp"
#include "selfjoin_hook.hpp"

/* --------------------------------------------------------------------------
 * Listen-host self-join fix (interop-preserving).
 *
 * On host creation logs.dll joins its own session: NET_internal_C430 runs DP8
 * EnumHosts (host discovery) and waits ~7s for a response. On this OS build the
 * configured port (5425) is bound only on the IPv6 link-local address, the IPv4
 * sockets get ephemeral ports, and dpnsvr is IPv6-only - so the loopback
 * discovery never answers and the host fails (ret 2 -> "GNET_GAM_CreateEx()
 * failed").
 *
 * Fix: skip discovery and connect straight to the host's OWN authoritative
 * listening address. The host's IDirectPlay8Server lives in a separate
 * connection in logs.dll's connection list; we find it, ask it for its real
 * address via GetLocalHostAddresses (= the IPv6 fe80::..:5425 it actually
 * binds), inject that as a discovered host (NET_internal_2E10) and join it
 * (NET_iJoinGame -> stock DP8 Client::Connect). Everything stays stock
 * DirectPlay, so ordinary players remain fully interoperable.
 * -------------------------------------------------------------------------- */

/* NET_internal_C430: the self-join entry point (enum + 7s wait). */
static const unsigned char SIG_C430[]  = {
	0x55, 0x56, 0x8B, 0x74, 0x24, 0x0C, 0x8B, 0x46, 0x18, 0x33, 0xED, 0x3B, 0xC5, 0x57, 0x74, 0x09
};
static const char          MASK_C430[] = "xxxxxxxxxxxxxxxx";

/* NET_internal_2E10: build/refresh a discovered-host list entry. */
static const unsigned char SIG_2E10[]  = {
	0x51, 0x53, 0x55, 0x8D, 0x44, 0x24, 0x08, 0x85, 0xC0, 0x56, 0x57, 0x75, 0x1A
};
static const char          MASK_2E10[] = "xxxxxxxxxxxxx";

/* NET_iJoinGame: stop enum + DP8 Client::Connect to a discovered host. */
static const unsigned char SIG_IJOIN[]  = {
	0x56, 0x8B, 0x74, 0x24, 0x08, 0x8B, 0x46, 0x10, 0x83, 0xE8, 0x03, 0x57, 0x74, 0x22
};
static const char          MASK_IJOIN[] = "xxxxxxxxxxxxxx";

typedef int (__cdecl *NET_internal_C430_t)(int a1, int a2);
typedef int (__cdecl *NET_internal_2E10_t)(int conn, void *info, DWORD *outHandle);
typedef int (__cdecl *NET_iJoinGame_t)(int wrapper, int hostHandle, int gameinit);

static NET_internal_C430_t orig_C430  = NULL;
static NET_internal_2E10_t fn_2E10    = NULL;   /* called directly, not hooked */
static NET_iJoinGame_t     fn_iJoin   = NULL;

/* Head of the logs.dll DP8 connection list (NET_DP8_InitConnection pushes a
 * 0x1C-byte node: [+4]=next, [+8]=conn(0x1A8)). IDB name dword_10EFCDF8.
 * Its RVA differs between game builds, so we DON'T hardcode it: we scan for the
 * node-push code and read the relocated absolute address out of the
 * `mov eax, ds:head` (opcode A1) imm32 - correct for whichever build is loaded.
 *
 *   C7 03 1C 00 00 00  mov [ebx],1Ch
 *   89 6B 08           mov [ebx+8],ebp
 *   A1 <imm32>         mov eax, dword_10EFCDF8   <- imm32 at offset 10 = head addr */
static void* g_connListHead = NULL;   /* address of the head pointer */

static const unsigned char SIG_HEADREF[] = {
	0xC7, 0x03, 0x1C, 0x00, 0x00, 0x00, 0x89, 0x6B, 0x08, 0xA1
};
static const char          MASK_HEADREF[] = "xxxxxxxxxx";
#define HEADREF_IMM_OFFSET 10

/* conn struct dword indices (NET_DP8_InitConnection, 424 bytes):
 *   [77]=host/server flag (1=server)  [79]=IDirectPlay8Client
 *   [80]=IDirectPlay8Server           [81]=host addr(client)/0(server)
 *   [83]=device addr */
#define CONN_HOSTFLAG 77
#define CONN_SERVER   80

static bool is_ok(int r) { return r == 0 || r == 267 || r == 10; }

/* Walk the connection list and return the host's IDirectPlay8Server, or NULL.
 * The self-join client conn and the host server conn are SEPARATE entries; we
 * need the server's authoritative listening address, not the client's. */
static IDirectPlay8Server* FindServerInterface()
{
	if (!g_connListHead) return NULL;
	void* node = *(void**)g_connListHead;
	int guard = 0;
	while (node && guard++ < 256)
	{
		int* conn = *(int**)((char*)node + 8);   /* node[2] = conn */
		if (conn && conn[CONN_HOSTFLAG] == 1 && conn[CONN_SERVER])
			return (IDirectPlay8Server*)conn[CONN_SERVER];
		node = *(void**)((char*)node + 4);        /* node[1] = next */
	}
	return NULL;
}

/* Inject a discovered-host entry for `addr` and join it. Returns the
 * NET_iJoinGame result, or -1 if the entry could not be built. */
static int JoinHostAddress(int a1, int a2, IDirectPlay8Address* addr)
{
	int conn = *(int*)(a1 + 4);

	/* NET_internal_2E10 info struct (10 dwords): [3..6]=session guidInstance
	 * (left 0 - DP8 resolves it via the directed enum/handshake),
	 * [7]=host IDirectPlay8Address (AddRef'd inside), [8]/[9]=response data. */
	DWORD info[10];
	memset(info, 0, sizeof(info));
	info[7] = (DWORD)(uintptr_t)addr;

	DWORD handle = 0;
	int r2 = fn_2E10(conn, info, &handle);
	if (!is_ok(r2) || handle == 0)
	{
		shim_log("self-join: NET_internal_2E10 failed (%d)", r2);
		return -1;
	}

	*(DWORD*)(a1 + 132) = handle;                  /* found-host handle */
	return fn_iJoin(a1, (int)handle, a2);
}

/* Connect the self-join to the host's own listening address(es). */
static int TryServerAddressJoin(int a1, int a2)
{
	IDirectPlay8Server* srv = FindServerInterface();
	if (!srv) { shim_log("self-join: host server interface not found"); return -1; }

	DWORD cnt = 0;
	srv->GetLocalHostAddresses(NULL, &cnt, 0);
	if (cnt == 0) { shim_log("self-join: no local host addresses"); return -1; }

	IDirectPlay8Address** arr = (IDirectPlay8Address**)calloc(cnt, sizeof(void*));
	if (!arr) return -1;
	if (FAILED(srv->GetLocalHostAddresses(arr, &cnt, 0))) { free(arr); return -1; }

	int result = -1;
	for (DWORD i = 0; i < cnt && !is_ok(result); ++i)
	{
		if (!arr[i]) continue;
		int r = JoinHostAddress(a1, a2, arr[i]);
		if (r != -1)
		{
			result = r;
			wchar_t url[512]; DWORD n = 512;
			if (SUCCEEDED(arr[i]->GetURLW(url, &n)))
				shim_log("self-join: NET_iJoinGame -> %d (host %ls)", r, url);
			else
				shim_log("self-join: NET_iJoinGame -> %d", r);
		}
	}

	for (DWORD i = 0; i < cnt; ++i)
		if (arr[i]) arr[i]->Release();
	free(arr);
	return result;
}

static int __cdecl hook_C430(int a1, int a2)
{
	int type = a1 ? *(int*)(a1 + 16) : 0;   /* a1+16: 3=LAN, 4=Internet (DP8) */

	if ((type == 3 || type == 4) && fn_2E10 && fn_iJoin)
	{
		int r = TryServerAddressJoin(a1, a2);
		if (r != -1 && is_ok(r))
			return r;
		shim_log("self-join: direct join unavailable (r=%d), using stock enum", r);
	}

	return orig_C430(a1, a2);   /* not a host self-join, or fix unavailable */
}

static uint8_t* scan_module(HMODULE mod, const unsigned char* sig, const char* mask, size_t siglen)
{
	MODULEINFO mi;
	if (!GetModuleInformation(GetCurrentProcess(), mod, &mi, sizeof(mi)))
		return NULL;
	uint8_t* base = (uint8_t*)mi.lpBaseOfDll;
	size_t   size = mi.SizeOfImage;
	for (size_t i = 0; i + siglen <= size; ++i)
	{
		bool ok = true;
		for (size_t j = 0; j < siglen; ++j)
			if (mask[j] == 'x' && base[i + j] != sig[j]) { ok = false; break; }
		if (ok) return base + i;
	}
	return NULL;
}

void SelfJoinHook_Install()
{
	HMODULE logs = GetModuleHandleA("logs.dll");
	if (logs == NULL) { shim_log("hook: logs.dll not loaded yet - skipping"); return; }
	if (MH_Initialize() != MH_OK) { shim_log("hook: MH_Initialize failed"); return; }

	/* Resolve the helpers we call directly. */
	fn_2E10  = (NET_internal_2E10_t)scan_module(logs, SIG_2E10,  MASK_2E10,  sizeof(SIG_2E10));
	fn_iJoin = (NET_iJoinGame_t)    scan_module(logs, SIG_IJOIN, MASK_IJOIN, sizeof(SIG_IJOIN));

	/* Resolve the connection-list head from the relocated imm32 in the
	 * node-push code (build-independent). */
	uint8_t* hr = scan_module(logs, SIG_HEADREF, MASK_HEADREF, sizeof(SIG_HEADREF));
	if (hr)
		g_connListHead = (void*)(*(uintptr_t*)(hr + HEADREF_IMM_OFFSET));

	uint8_t* c430 = scan_module(logs, SIG_C430, MASK_C430, sizeof(SIG_C430));
	if (c430 == NULL) { shim_log("hook: NET_internal_C430 signature NOT found"); return; }
	if (MH_CreateHook((LPVOID)c430, (LPVOID)&hook_C430, (LPVOID*)&orig_C430) != MH_OK
		|| MH_EnableHook((LPVOID)c430) != MH_OK)
	{
		shim_log("hook: NET_internal_C430 hook failed");
		return;
	}

	shim_log("hook: installed (2E10=%p iJoin=%p head=%p C430=%p)",
		(void*)fn_2E10, (void*)fn_iJoin, g_connListHead, (void*)c430);
	if (!fn_2E10 || !fn_iJoin || !g_connListHead)
		shim_log("hook: WARNING - a signature did not resolve; self-join fix may be inactive");
}
