// Standalone one-click installer / updater / launcher for the Astonia client.
//
// Drop updater.exe into any folder and run it: with no prompts it downloads the
// game into that folder (or, on later runs, only the files that changed), then
// launches bin/moac.exe. It talks to the server's own nginx file host over plain
// HTTP and verifies every file by SHA-256, so a corrupt or interrupted download
// is never left in place.
//
// Windows-only. Uses the local WinHTTP client (http.h / http_windows.c in this
// folder) and the client's vendored cJSON. Build with the Makefile here (links
// -lwinhttp -lbcrypt).

#include <windows.h>
#include <bcrypt.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http.h"
#include "lib/cjson/cJSON.h"

// The server hosting manifest.json + the /files/ tree. Point this at your public
// server address (IP or DNS) and the nginx port from docker-compose.updates.yml.
// This is the ONLY value that must change per server. Override it without editing
// this file by building with -DUPDATE_BASE_URL='"http://your-server:8080"'
// (the Makefile forwards CFLAGS), mirroring version.c's -DCLIENT_VERSION.
#ifndef UPDATE_BASE_URL
#define UPDATE_BASE_URL "http://108.64.255.138:8080"
#endif

// The game to launch once files are in sync. Overridable at build time too.
#ifndef GAME_EXE
#define GAME_EXE "bin\\moac.exe"
#endif

#define MANIFEST_URL UPDATE_BASE_URL "/manifest.json"
#define UPD_MAXFILES 256

struct upd_file {
	char path[260]; // install-root-relative, forward-slashed (e.g. "bin/moac.exe")
	char sha[65]; // expected lowercase hex SHA-256
	unsigned long long size;
	int changed;
};

static struct upd_file g_files[UPD_MAXFILES];
static int g_nfiles;

// Name and last-drawn percent of the file currently downloading (for the bar).
static char g_cur_name[260];
static int g_last_pct;

// ---------------------------------------------------------------------------
// Helpers (sha256_file and make_parent_dirs mirror src/game/updater.c)
// ---------------------------------------------------------------------------

// SHA-256 a file into a 64-char lowercase hex string (out_hex must hold 65
// bytes). Returns 0 on success, -1 on any error (including a missing file).
static int sha256_file(const char *path, char *out_hex)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		return -1;
	}

	BCRYPT_ALG_HANDLE alg = NULL;
	BCRYPT_HASH_HANDLE hash = NULL;
	unsigned char digest[32];
	unsigned char buf[65536];
	int rc = -1;

	if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0) {
		goto done;
	}
	if (BCryptCreateHash(alg, &hash, NULL, 0, NULL, 0, 0) < 0) {
		goto done;
	}

	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
		if (BCryptHashData(hash, buf, (ULONG)n, 0) < 0) {
			goto done;
		}
	}
	if (BCryptFinishHash(hash, digest, (ULONG)sizeof(digest), 0) < 0) {
		goto done;
	}
	for (int i = 0; i < 32; i++) {
		snprintf(out_hex + i * 2, 3, "%02x", (unsigned)digest[i]);
	}
	out_hex[64] = '\0';
	rc = 0;

done:
	if (hash) {
		BCryptDestroyHash(hash);
	}
	if (alg) {
		BCryptCloseAlgorithmProvider(alg, 0);
	}
	fclose(f);
	return rc;
}

// Create the parent directories of a (forward-slashed) path under the CWD.
static void make_parent_dirs(const char *path)
{
	char tmp[400];
	snprintf(tmp, sizeof(tmp), "%s", path);
	for (char *p = tmp + 1; *p; p++) {
		if (*p == '/' || *p == '\\') {
			char c = *p;
			*p = '\0';
			CreateDirectoryA(tmp, NULL);
			*p = c;
		}
	}
}

// Make the folder holding updater.exe the working directory, so files install
// next to the updater however it was launched (double-click, shortcut, etc.).
static void set_cwd_to_exe_dir(void)
{
	char exe[MAX_PATH];
	DWORD n = GetModuleFileNameA(NULL, exe, (DWORD)sizeof(exe));
	if (n == 0 || n >= sizeof(exe)) {
		return;
	}
	for (char *p = exe + n; p > exe; p--) {
		if (*p == '\\' || *p == '/') {
			*p = '\0';
			SetCurrentDirectoryA(exe);
			return;
		}
	}
}

// ---------------------------------------------------------------------------
// Progress bar
// ---------------------------------------------------------------------------

static void draw_bar(const char *name, int pct)
{
	if (pct < 0) {
		pct = 0;
	}
	if (pct > 100) {
		pct = 100;
	}
	char bar[21];
	int filled = pct / 5; // 20 cells, 5% each
	for (int i = 0; i < 20; i++) {
		bar[i] = (i < filled) ? '#' : '-';
	}
	bar[20] = '\0';
	printf("\r  %-28s [%s] %3d%%", name, bar, pct);
	fflush(stdout);
}

static void on_progress(unsigned long long received, unsigned long long total, void *userdata)
{
	(void)userdata;
	if (total == 0) {
		return;
	}
	int pct = (int)((received * 100ull) / total);
	if (pct != g_last_pct) {
		g_last_pct = pct;
		draw_bar(g_cur_name, pct);
	}
}

// ---------------------------------------------------------------------------
// Manifest
// ---------------------------------------------------------------------------

// Parse the manifest JSON into g_files. Copies "version" into out_version when
// present. Returns the file count, or -1 on error.
static int parse_manifest(const char *json, char *out_version, size_t vlen)
{
	cJSON *root = cJSON_Parse(json);
	if (!root) {
		return -1;
	}
	cJSON *ver = cJSON_GetObjectItem(root, "version");
	if (out_version && ver && ver->valuestring) {
		snprintf(out_version, vlen, "%s", ver->valuestring);
	}

	cJSON *files = cJSON_GetObjectItem(root, "files");
	if (!files || !cJSON_IsArray(files)) {
		cJSON_Delete(root);
		return -1;
	}

	int n = 0;
	cJSON *item;
	cJSON_ArrayForEach(item, files)
	{
		if (n >= UPD_MAXFILES) {
			break;
		}
		cJSON *path = cJSON_GetObjectItem(item, "path");
		cJSON *sha = cJSON_GetObjectItem(item, "sha256");
		cJSON *size = cJSON_GetObjectItem(item, "size");
		if (!path || !path->valuestring || !sha || !sha->valuestring) {
			continue;
		}
		struct upd_file *f = &g_files[n];
		snprintf(f->path, sizeof(f->path), "%s", path->valuestring);
		snprintf(f->sha, sizeof(f->sha), "%s", sha->valuestring);
		f->size = (size && cJSON_IsNumber(size)) ? (unsigned long long)size->valuedouble : 0;
		f->changed = 0;
		n++;
	}
	cJSON_Delete(root);
	return n;
}

// Download UPDATE_BASE_URL/files/<path> to <path>.part, verify its SHA-256, then
// move it into place (replacing any old copy). Retries once. Returns 0 on
// success, -1 on failure. A verify/transport failure never leaves a corrupt file
// at the target path.
static int fetch_file(struct upd_file *f)
{
	char url[800];
	snprintf(url, sizeof(url), "%s/files/%s", UPDATE_BASE_URL, f->path);

	char part[300];
	snprintf(part, sizeof(part), "%s.part", f->path);
	make_parent_dirs(part);

	snprintf(g_cur_name, sizeof(g_cur_name), "%s", f->path);

	for (int attempt = 0; attempt < 2; attempt++) {
		g_last_pct = -1;
		if (http_download(url, part, on_progress, NULL) != 0) {
			continue;
		}
		char have[65];
		if (sha256_file(part, have) == 0 && strcmp(have, f->sha) == 0) {
			draw_bar(g_cur_name, 100);
			printf("\n");
			if (MoveFileExA(part, f->path, MOVEFILE_REPLACE_EXISTING)) {
				return 0;
			}
		}
		remove(part);
	}
	printf("\n  ERROR: failed to fetch %s\n", f->path);
	return -1;
}

int main(void)
{
	set_cwd_to_exe_dir();

	printf("========================================\n");
	printf(" Astonia Updater\n");
	printf("========================================\n");
	printf("Server: %s\n", UPDATE_BASE_URL);
	printf("Checking for updates...\n");

	char *json = http_get(MANIFEST_URL, NULL);
	if (!json) {
		printf("\nERROR: could not reach the update server (%s).\n", MANIFEST_URL);
		printf("Check your connection (and that the server is up), then run the updater again.\n");
		Sleep(8000);
		return 1;
	}

	char version[64] = "";
	g_nfiles = parse_manifest(json, version, sizeof(version));
	free(json);
	if (g_nfiles < 0) {
		printf("\nERROR: the update manifest was invalid.\n");
		Sleep(8000);
		return 1;
	}
	if (version[0]) {
		printf("Latest version: %s\n", version);
	}

	// Diff pass: mark files whose local copy is missing or hash-mismatched.
	unsigned long long need = 0;
	int changed = 0;
	for (int i = 0; i < g_nfiles; i++) {
		struct upd_file *f = &g_files[i];
		char have[65];
		if (sha256_file(f->path, have) != 0 || strcmp(have, f->sha) != 0) {
			f->changed = 1;
			need += f->size;
			changed++;
		}
	}

	if (changed == 0) {
		printf("Already up to date (%d files).\n", g_nfiles);
	} else {
		printf("Downloading %d file(s), %.1f MB...\n\n", changed, (double)need / (1024.0 * 1024.0));
		int done = 0;
		for (int i = 0; i < g_nfiles; i++) {
			if (!g_files[i].changed) {
				continue;
			}
			if (fetch_file(&g_files[i]) != 0) {
				printf("\nUpdate failed. Please run the updater again.\n");
				Sleep(8000);
				return 1;
			}
			done++;
		}
		printf("\nUpdate complete (%d file(s)).\n", done);
	}

	// Launch the game (CWD is the install root, so moac.exe finds res/) and exit.
	printf("Launching game...\n");
	STARTUPINFOA si;
	PROCESS_INFORMATION pi;
	memset(&si, 0, sizeof(si));
	memset(&pi, 0, sizeof(pi));
	si.cb = sizeof(si);

	char cmd[] = GAME_EXE;
	if (!CreateProcessA(GAME_EXE, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
		printf("ERROR: could not launch %s.\n", GAME_EXE);
		Sleep(8000);
		return 1;
	}
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return 0;
}
