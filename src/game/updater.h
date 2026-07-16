#ifndef ASTONIA_UPDATER_H
#define ASTONIA_UPDATER_H

// In-client auto-updater. Checks the GitHub Releases API for a newer client,
// downloads only the files whose SHA-256 changed (so a code fix is a small
// download, not the whole 170+ MB package), then applies them on exit via a
// detached helper. All network/disk work happens on a worker thread; the UI
// polls updater_get_state() each frame and never blocks.

enum updater_state {
	UPD_IDLE = 0,    // nothing started yet
	UPD_CHECKING,    // version check in flight
	UPD_UP_TO_DATE,  // running the latest release
	UPD_AVAILABLE,   // a newer release exists (see updater_latest_version)
	UPD_DOWNLOADING, // fetching + verifying changed files
	UPD_READY,       // staged and verified; call updater_apply_and_exit()
	UPD_ERROR        // check or download failed (offline, bad data, ...)
};

// Start the version check on a worker thread. No-op if a check/download is
// already running. Safe to call once from the start screen.
void updater_check_async(void);

// Current state (one of enum updater_state). Cheap; poll it each frame.
int updater_get_state(void);

// Latest release version string, valid once state is AVAILABLE or later.
const char *updater_latest_version(void);

// Download progress in [0,1] while state is UPD_DOWNLOADING.
float updater_get_progress(void);

// Begin downloading + staging the changed files on a worker thread. Only valid
// when state is UPD_AVAILABLE; no-op otherwise.
void updater_download_async(void);

// Apply the staged update: write and launch a detached helper that waits for
// this process to exit, copies the staged files into place, and relaunches the
// client. Only valid when state is UPD_READY. Does not return (exits the
// process); no-op if not READY.
void updater_apply_and_exit(void);

#endif
