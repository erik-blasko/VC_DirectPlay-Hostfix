#ifndef VC_DPNET_HOSTFIX_SELFJOIN_HOOK_HPP
#define VC_DPNET_HOSTFIX_SELFJOIN_HOOK_HPP

/* Installs the in-process hook on logs.dll's listen-host self-join
 * (NET_internal_C430) that works around the broken local host discovery by
 * connecting straight to the host's own listening address. See
 * selfjoin_hook.cpp for details. No-op if logs.dll is not loaded. */
void SelfJoinHook_Install();

#endif
