#include "astonia.h"

// Human-readable client version, shown in-client (see cmd.c "Client version").
// Kept as a plain "MAJOR.MINOR.PATCH" semver; bump it per release. The Makefile
// may override it at build time via -DCLIENT_VERSION. (The standalone updater in
// updater/ syncs by per-file SHA-256 against the server manifest, so it does not
// depend on this string.)
#ifndef CLIENT_VERSION
#define CLIENT_VERSION "1.4.1"
#endif

// Build metadata (exact commit), stamped by the Makefile from `git describe`.
// Falls back to the compile timestamp for ad-hoc local builds. Display only.
#ifndef CLIENT_BUILD
#define CLIENT_BUILD "built " __DATE__ " " __TIME__
#endif

char *client_version(void)
{
	return CLIENT_VERSION;
}

char *client_build(void)
{
	return CLIENT_BUILD;
}
