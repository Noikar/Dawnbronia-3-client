// In-client auto-updater implementation (see updater.h). Talks to the GitHub
// Releases API, diffs a manifest.json by SHA-256, downloads only changed files,
// and applies them on exit through a detached batch helper. Windows-only:
// hashing uses BCrypt, HTTP uses the WinHTTP-backed http.c, apply uses cmd/robocopy.

#include <windows.h>
#include <bcrypt.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "updater.h"
#include "http.h"
#include "lib/cjson/cJSON.h"

// The client's own version, defined in version.c.
extern char *client_version(void);

// Owner/repo whose Releases feed the updater. Lives in one place so a rename is
// a one-line change.
#define UPDATE_REPO    "Noikar/Dawnbronia-3-client"
#define UPDATE_API_URL "https://api.github.com/repos/" UPDATE_REPO "/releases/latest"

#define UPD_MAXFILES 128
#define STAGING_DIR  "update_staging"
#define APPLY_BAT    "apply_update.bat"

struct upd_file {
	char path[260]; // install-root-relative target, e.g. "bin/moac.exe"
	char url[600]; // direct download URL for this file's release asset
	char sha[65]; // expected lowercase hex SHA-256
	unsigned long long size;
	int changed; // set during the diff pass
};

static volatile LONG g_state = UPD_IDLE;
static volatile LONG g_progress_milli; // 0..1000
static char g_latest[64];
static char g_manifest_url[600];
static struct upd_file g_files[UPD_MAXFILES];
static int g_nfiles;

// ---------------------------------------------------------------------------
// Small helpers
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

// Parse the leading "MAJOR.MINOR.PATCH" out of s, skipping any leading non-digit
// (e.g. a "v" prefix) and ignoring trailing suffixes.
static void semver_parse(const char *s, int v[3])
{
	v[0] = v[1] = v[2] = 0;
	while (*s && (*s < '0' || *s > '9')) {
		s++;
	}
	sscanf(s, "%d.%d.%d", &v[0], &v[1], &v[2]);
}

// -1 if a<b, 0 if equal, 1 if a>b (by semver).
static int semver_cmp(const char *a, const char *b)
{
	int va[3], vb[3];
	semver_parse(a, va);
	semver_parse(b, vb);
	for (int i = 0; i < 3; i++) {
		if (va[i] < vb[i]) {
			return -1;
		}
		if (va[i] > vb[i]) {
			return 1;
		}
	}
	return 0;
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

// ---------------------------------------------------------------------------
// Version check
// ---------------------------------------------------------------------------

static DWORD WINAPI check_worker(LPVOID arg)
{
	(void)arg;

	char *body = http_get(UPDATE_API_URL, NULL);
	if (!body) {
		InterlockedExchange(&g_state, UPD_ERROR);
		return 0;
	}

	cJSON *root = cJSON_Parse(body);
	free(body);
	if (!root) {
		InterlockedExchange(&g_state, UPD_ERROR);
		return 0;
	}

	cJSON *tag = cJSON_GetObjectItem(root, "tag_name");
	if (!tag || !tag->valuestring) {
		cJSON_Delete(root);
		InterlockedExchange(&g_state, UPD_ERROR);
		return 0;
	}
	snprintf(g_latest, sizeof(g_latest), "%s", tag->valuestring);

	// Find the manifest.json asset's download URL.
	g_manifest_url[0] = '\0';
	cJSON *assets = cJSON_GetObjectItem(root, "assets");
	if (assets && cJSON_IsArray(assets)) {
		cJSON *a;
		cJSON_ArrayForEach(a, assets)
		{
			cJSON *name = cJSON_GetObjectItem(a, "name");
			cJSON *url = cJSON_GetObjectItem(a, "browser_download_url");
			if (name && name->valuestring && url && url->valuestring &&
			    strcmp(name->valuestring, "manifest.json") == 0) {
				snprintf(g_manifest_url, sizeof(g_manifest_url), "%s", url->valuestring);
				break;
			}
		}
	}

	int newer = semver_cmp(client_version(), g_latest) < 0;
	cJSON_Delete(root);

	InterlockedExchange(&g_state, newer ? UPD_AVAILABLE : UPD_UP_TO_DATE);
	return 0;
}

// ---------------------------------------------------------------------------
// Download + stage
// ---------------------------------------------------------------------------

struct progress_ctx {
	unsigned long long base; // bytes fully downloaded before the current file
	unsigned long long grand; // total bytes to download across all changed files
};

static void download_progress(unsigned long long received, unsigned long long total, void *userdata)
{
	(void)total;
	struct progress_ctx *ctx = userdata;
	if (ctx->grand == 0) {
		return;
	}
	unsigned long long done = ctx->base + received;
	LONG milli = (LONG)((done * 1000ull) / ctx->grand);
	if (milli > 1000) {
		milli = 1000;
	}
	InterlockedExchange(&g_progress_milli, milli);
}

// Parse the manifest into g_files. Returns file count, or -1 on error.
static int parse_manifest(const char *json)
{
	cJSON *root = cJSON_Parse(json);
	if (!root) {
		return -1;
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
		cJSON *url = cJSON_GetObjectItem(item, "url");
		cJSON *size = cJSON_GetObjectItem(item, "size");
		if (!path || !path->valuestring || !sha || !sha->valuestring || !url || !url->valuestring) {
			continue;
		}
		struct upd_file *f = &g_files[n];
		snprintf(f->path, sizeof(f->path), "%s", path->valuestring);
		snprintf(f->sha, sizeof(f->sha), "%s", sha->valuestring);
		snprintf(f->url, sizeof(f->url), "%s", url->valuestring);
		f->size = (size && cJSON_IsNumber(size)) ? (unsigned long long)size->valuedouble : 0;
		f->changed = 0;
		n++;
	}
	cJSON_Delete(root);
	return n;
}

static DWORD WINAPI download_worker(LPVOID arg)
{
	(void)arg;

	if (g_manifest_url[0] == '\0') {
		InterlockedExchange(&g_state, UPD_ERROR);
		return 0;
	}

	char *json = http_get(g_manifest_url, NULL);
	if (!json) {
		InterlockedExchange(&g_state, UPD_ERROR);
		return 0;
	}
	g_nfiles = parse_manifest(json);
	free(json);
	if (g_nfiles < 0) {
		InterlockedExchange(&g_state, UPD_ERROR);
		return 0;
	}

	// Diff pass: mark files whose local copy is missing or hash-mismatched.
	unsigned long long grand = 0;
	int changed = 0;
	for (int i = 0; i < g_nfiles; i++) {
		struct upd_file *f = &g_files[i];
		char have[65];
		if (sha256_file(f->path, have) != 0 || strcmp(have, f->sha) != 0) {
			f->changed = 1;
			grand += f->size;
			changed++;
		}
	}

	if (changed == 0) {
		// Version bumped but no file actually differs: nothing to apply.
		InterlockedExchange(&g_state, UPD_UP_TO_DATE);
		return 0;
	}

	// Download pass: fetch each changed file into the staging tree and verify.
	struct progress_ctx ctx = {0, grand};
	for (int i = 0; i < g_nfiles; i++) {
		struct upd_file *f = &g_files[i];
		if (!f->changed) {
			continue;
		}
		char stage[400];
		snprintf(stage, sizeof(stage), "%s/%s", STAGING_DIR, f->path);
		make_parent_dirs(stage);

		if (http_download(f->url, stage, download_progress, &ctx) != 0) {
			InterlockedExchange(&g_state, UPD_ERROR);
			return 0;
		}
		char have[65];
		if (sha256_file(stage, have) != 0 || strcmp(have, f->sha) != 0) {
			remove(stage);
			InterlockedExchange(&g_state, UPD_ERROR);
			return 0;
		}
		ctx.base += f->size;
	}

	InterlockedExchange(&g_progress_milli, 1000);
	InterlockedExchange(&g_state, UPD_READY);
	return 0;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void updater_check_async(void)
{
	// Only start from a resting state.
	LONG prev = InterlockedCompareExchange(&g_state, UPD_CHECKING, UPD_IDLE);
	if (prev != UPD_IDLE) {
		return;
	}
	HANDLE t = CreateThread(NULL, 0, check_worker, NULL, 0, NULL);
	if (t) {
		CloseHandle(t);
	} else {
		InterlockedExchange(&g_state, UPD_ERROR);
	}
}

int updater_get_state(void)
{
	return (int)g_state;
}

const char *updater_latest_version(void)
{
	return g_latest;
}

float updater_get_progress(void)
{
	return (float)g_progress_milli / 1000.0f;
}

void updater_download_async(void)
{
	LONG prev = InterlockedCompareExchange(&g_state, UPD_DOWNLOADING, UPD_AVAILABLE);
	if (prev != UPD_AVAILABLE) {
		return;
	}
	InterlockedExchange(&g_progress_milli, 0);
	HANDLE t = CreateThread(NULL, 0, download_worker, NULL, 0, NULL);
	if (t) {
		CloseHandle(t);
	} else {
		InterlockedExchange(&g_state, UPD_ERROR);
	}
}

// Write the detached helper that waits for us to exit, copies the staged tree
// into place, relaunches the client, and deletes itself.
static int write_apply_bat(void)
{
	FILE *f = fopen(APPLY_BAT, "wb");
	if (!f) {
		return -1;
	}
	fputs("@echo off\r\n", f);
	fputs("setlocal\r\n", f);
	fprintf(f, "set PID=%lu\r\n", (unsigned long)GetCurrentProcessId());
	fputs(":wait\r\n", f);
	fputs("tasklist /FI \"PID eq %PID%\" 2>NUL | find \"%PID%\" >NUL\r\n", f);
	fputs("if %ERRORLEVEL%==0 (\r\n", f);
	fputs("  ping -n 2 127.0.0.1 >NUL\r\n", f);
	fputs("  goto wait\r\n", f);
	fputs(")\r\n", f);
	fputs("robocopy \"" STAGING_DIR "\" \".\" /E /IS /IT /NFL /NDL /NJH /NJS /NP >NUL\r\n", f);
	fputs("rmdir /S /Q \"" STAGING_DIR "\" >NUL 2>&1\r\n", f);
	fputs("start \"\" \"bin\\moac.exe\"\r\n", f);
	fputs("del \"%~f0\"\r\n", f);
	fclose(f);
	return 0;
}

void updater_apply_and_exit(void)
{
	if (g_state != UPD_READY) {
		return;
	}
	if (write_apply_bat() != 0) {
		InterlockedExchange(&g_state, UPD_ERROR);
		return;
	}

	STARTUPINFOA si;
	PROCESS_INFORMATION pi;
	memset(&si, 0, sizeof(si));
	memset(&pi, 0, sizeof(pi));
	si.cb = sizeof(si);

	char cmd[] = "cmd.exe /c \"" APPLY_BAT "\"";
	if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, DETACHED_PROCESS | CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP,
	        NULL, NULL, &si, &pi)) {
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
	}

	exit(0);
}
