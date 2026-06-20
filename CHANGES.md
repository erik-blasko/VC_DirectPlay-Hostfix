# Changelog

All notable changes to this project are documented here. The version is defined
in `src/version.h` (and embedded in the DLL's file properties); bump it together
with a new entry below.

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
