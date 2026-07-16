#include "astonia.h"

// Authoritative client version, used by the auto-updater to decide whether a
// newer release is available. Keep it a plain "MAJOR.MINOR.PATCH" semver so the
// updater's comparison can parse it, bump it for every release, and tag the
// GitHub release to match (e.g. version 1.2.0 -> tag v1.2.0). The Makefile may
// override it at build time via -DCLIENT_VERSION.
#ifndef CLIENT_VERSION
#define CLIENT_VERSION "1.2.0"
#endif

// Build metadata (exact commit), stamped by the Makefile from `git describe`.
// Falls back to the compile timestamp for ad-hoc local builds. For display and
// debugging only - never used for update comparisons.
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
