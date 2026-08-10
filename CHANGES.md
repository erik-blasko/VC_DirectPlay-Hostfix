# Changelog

All notable changes to this project are documented here. The version is defined
in `src/version.h` (and embedded in the DLL's file properties); bump it together
with a new entry below.

## 1.1.0 - 2026-08-10

- **UPnP for the GameSpy query port.** A host behind an ordinary NAT would
  register with the GameSpy master and then never answer its `\status\` probes,
  because Windows' DirectPlay NAT helper only opens DirectPlay's own socket and
  knows nothing about the query socket logs.dll opens itself. The shim now
  registers that port too, through the very same Windows component
  (`dpnathlp.dll`), so the router sees the client it already deals with.
  - The port is **never hardcoded**: it is read out of the server's own
    outgoing `\heartbeat\<port>\` datagram, so whatever the operator configures
    is what gets mapped. The DirectPlay port is learned the same way from
    `\hostport\` in status replies and mapped for **TCP**, which the Windows
    helper does not request.
  - Mappings are released when the server closes its query socket, on
    `WSACleanup`, and on an orderly unload. A one hour lease (renewed by the
    helper) is the backstop if the process is killed outright.
  - Silently does nothing when there is no UPnP gateway, or where
    `dpnathlp.dll` is absent (Wine, Windows without DirectPlay).
  - Set `DPSHIM_NO_UPNP=1` to turn it off.
- Interface details for `IDirectPlayNATHelp` were measured, not taken from
  documentation - both Wine's header and MSDN are wrong for current Windows.
  See the comment block at the top of `src/upnp_ports.cpp`.

## 1.0.0 - 2026-06-20

First working release.

- Listen-host self-join fix: hooks logs.dll `NET_internal_C430` and, instead of
  relying on the broken local host discovery (dpnsvr is IPv6-only on modern
  Windows), connects the host to its **own** authoritative listening address
  obtained from `IDirectPlay8Server::GetLocalHostAddresses`.
- Pass-through `dpnet.dll`: every COM class request is delegated to the real
  system dpnet.dll, so the game uses 100% stock DirectPlay8 and remains
  interoperable with players running unmodified Windows DirectPlay.
- Build-independent symbol resolution: all logs.dll functions and the
  connection-list head global are located by byte-signature (and the global's
  relocated address is read from the instruction stream), so the same binary
  works against both the retail and dev (editor) game builds.
- Per-user install/uninstall via COM registration under HKCU (and the
  Wow6432Node path that 32-bit games read); no admin rights, no system files
  changed.
- Opt-in logging via the `DPSHIM_LOG` environment variable.

Verified working as a LAN listen-host in both `vietcong.exe` (retail) and
`dev_editor.exe`, in both hosting directions, with the map loading and the
session playable.
