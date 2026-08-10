#ifndef UPNP_PORTS_HPP
#define UPNP_PORTS_HPP

/* Opens the server's GameSpy query port on a UPnP router, so a host behind an
 * ordinary NAT shows up in the internet server list. See upnp_ports.cpp. */
void UPnPPorts_Install();

/* Release every mapping we created. `processExiting` is true when the DLL is
 * being torn down as part of process exit, where doing real work is unsafe. */
void UPnPPorts_Shutdown(bool processExiting);

#endif
