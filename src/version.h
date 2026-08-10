#ifndef VC_DPNET_HOSTFIX_VERSION_H
#define VC_DPNET_HOSTFIX_VERSION_H

/* Single source of truth for the shim version. Bump on every released change
 * and add a matching entry to CHANGES.md.
 *   - SHIM_VERSION_*  : numeric parts used by the VERSIONINFO resource.
 *   - SHIM_VERSION_STR: human string, logged at load and shown in CHANGES.md. */
#define SHIM_VERSION_MAJOR 1
#define SHIM_VERSION_MINOR 1
#define SHIM_VERSION_PATCH 0

#define SHIM_VERSION_STR   "1.1.0"

#endif
