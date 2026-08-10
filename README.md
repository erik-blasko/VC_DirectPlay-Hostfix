# Vietcong DirectPlay host-fix

A drop-in `dpnet.dll` that fixes **multiplayer hosting in Vietcong (1.60)** on
modern Windows (10/11), where creating a server fails with:

```
GNET_GAM_CreateEx() failed   (host creation aborts / never starts)
```

Unlike a full DirectPlay reimplementation, this shim keeps using Windows' own
DirectPlay — so **only the person hosting needs to install it**, and ordinary
players connect with stock, unmodified DirectPlay. No system files are changed;
the override is a per-user COM registration you can undo at any time.

## The problem

Vietcong hosts a game through DirectPlay8. When the host is created, the game
joins its own session by **discovering itself** over the local network. That
discovery relies on the Windows helper `dpnsvr.exe` and on the host socket
binding the configured port. On recent Windows builds the host port is bound
only on an **IPv6 link-local address**, the IPv4 sockets get ephemeral ports,
and `dpnsvr` is IPv6-only — so the loopback self-discovery never answers, the
host self-join times out, and server creation fails. There is no in-game or
registry setting to work around it.

## The fix

This `dpnet.dll` is registered (per-user COM) in place of the system one, but it
does **not** reimplement DirectPlay: every COM object the game asks for is
delegated straight to the genuine system `dpnet.dll`. Being loaded into the game
process simply gives it a place to run a small in-process patch.

That patch intercepts the host self-join and, instead of waiting on the broken
discovery, asks the host's own `IDirectPlay8Server` for its real listening
address (`GetLocalHostAddresses`) and connects directly to it. Because
everything else stays stock DirectPlay, the host is reachable by ordinary
players exactly as before.

All of the game's functions and globals are located by byte-signature, so the
same binary works against both the retail `vietcong.exe` and the editor
(`dev_editor.exe`).

## Also: your server shows up in the internet list

A Vietcong server tells the GameSpy master which port to probe it on
(`\heartbeat\<queryport>\`). Windows' DirectPlay NAT helper opens a hole for
DirectPlay's own socket, but it knows nothing about that query socket — so a
host behind a home router gets listed and then never answers, and the server
looks dead to everyone browsing.

This shim asks the very same Windows component to open the query port as well,
reading the port number out of the server's own heartbeat (so a custom port
works without any configuration). The mapping is released when the server shuts
down, and expires by itself within an hour if the process is killed. Where
there is no UPnP router — or no DirectPlay, as on Wine — nothing happens at all.

Set `DPSHIM_NO_UPNP=1` before launching to turn this off.

If your router has UPnP disabled, or you are behind more than one NAT, forward
these to the machine hosting the game instead:

| port | protocol | what for |
|---|---|---|
| 15425 | UDP | GameSpy query — without it the server is listed but unreachable |
| 5425 | UDP + TCP | DirectPlay 8 — the game itself |

(Those are the defaults; if you changed the server's port, adjust accordingly.)

## Install

1. Download / extract this folder somewhere.
2. Double-click **`install.bat`**.

That's it — no admin rights needed, and **only the host installs it**. Host a
LAN or Internet game in Vietcong as usual.

To revert, run **`uninstall.bat`**.

## Building from source

Requires Visual Studio 2019/2022 with the C++ desktop workload.

```
MSBuild dpnet.sln /p:Configuration=Release /p:Platform=x86
```

Output: `Release\dpnet.dll` (32-bit). Copy it over `bin\dpnet.dll` if you want
`install.bat` to deploy your own build. The version is defined in
`src/version.h`.

## Optional logging

Set the environment variable `DPSHIM_LOG` to a file path before launching the
game to capture a trace of the self-join fix. Logging is off by default.

## License & credits

Licensed under the **GNU LGPL v2.1 or later** (see `LICENSE.txt`).

- DirectPlay8 interface headers (`include/dplay8.h`, `include/dpaddr.h`) are
  from the [Wine project](https://www.winehq.org/) (© Raphael Junqueira), LGPL-2.1.
- In-process hooking uses [MinHook](https://github.com/TsudaKageyu/minhook)
  by Tsuda Kageyu (BSD-2-Clause; see `minhook/LICENSE.txt`).

