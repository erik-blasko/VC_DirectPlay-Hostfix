#ifndef VC_DPNET_SHIM_LOG_HPP
#define VC_DPNET_SHIM_LOG_HPP

/* Opt-in logging: set env var DPSHIM_LOG to a file path to enable. No output
 * by default. */
void shim_log(const char *fmt, ...);

#endif
