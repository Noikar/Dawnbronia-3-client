// WinHTTP-backed implementation of the updater's tiny HTTP client (http.h).
// Windows-only: TLS comes from SChannel, so there are no external dependencies.

#include "http.h"

#include <windows.h>
#include <winhttp.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HTTP_USER_AGENT L"Dawnbronia3-Updater"

struct http_req {
	HINTERNET session;
	HINTERNET connect;
	HINTERNET request;
};

// Convert a UTF-8 string to a freshly allocated wide string (caller frees).
static wchar_t *utf8_to_wide(const char *s)
{
	int len = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
	if (len <= 0) {
		return NULL;
	}
	wchar_t *w = malloc((size_t)len * sizeof(wchar_t));
	if (!w) {
		return NULL;
	}
	if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, len) <= 0) {
		free(w);
		return NULL;
	}
	return w;
}

static void http_req_close(struct http_req *r)
{
	if (r->request) {
		WinHttpCloseHandle(r->request);
	}
	if (r->connect) {
		WinHttpCloseHandle(r->connect);
	}
	if (r->session) {
		WinHttpCloseHandle(r->session);
	}
	r->request = NULL;
	r->connect = NULL;
	r->session = NULL;
}

// Open a GET request for url, send it, and receive the response headers.
// On success returns 1 and stores the HTTP status code in *status; the caller
// must http_req_close(r) either way.
static int http_req_open(const char *url, struct http_req *r, DWORD *status)
{
	memset(r, 0, sizeof(*r));
	*status = 0;

	wchar_t *wurl = utf8_to_wide(url);
	if (!wurl) {
		return 0;
	}

	// Crack the URL: copy the host into a local buffer (NUL-terminated) and get
	// pointers into wurl for the path and query.
	wchar_t host[256];
	URL_COMPONENTS uc;
	memset(&uc, 0, sizeof(uc));
	uc.dwStructSize = sizeof(uc);
	uc.lpszHostName = host;
	uc.dwHostNameLength = (DWORD)(sizeof(host) / sizeof(host[0]));
	uc.dwUrlPathLength = (DWORD)-1;
	uc.dwExtraInfoLength = (DWORD)-1;

	if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) {
		free(wurl);
		return 0;
	}

	// Build the object name (path + query). Fall back to "/" for empty paths.
	size_t pathlen = uc.dwUrlPathLength;
	size_t extralen = uc.dwExtraInfoLength;
	wchar_t *object = malloc((pathlen + extralen + 2) * sizeof(wchar_t));
	if (!object) {
		free(wurl);
		return 0;
	}
	if (pathlen == 0 && extralen == 0) {
		object[0] = L'/';
		object[1] = L'\0';
	} else {
		if (pathlen) {
			memcpy(object, uc.lpszUrlPath, pathlen * sizeof(wchar_t));
		}
		if (extralen) {
			memcpy(object + pathlen, uc.lpszExtraInfo, extralen * sizeof(wchar_t));
		}
		object[pathlen + extralen] = L'\0';
	}

	INTERNET_PORT port = uc.nPort;
	int secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);
	free(wurl);

	r->session = WinHttpOpen(HTTP_USER_AGENT, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
	                         WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!r->session) {
		free(object);
		return 0;
	}
	WinHttpSetTimeouts(r->session, 10000, 15000, 30000, 60000);

	r->connect = WinHttpConnect(r->session, host, port, 0);
	if (!r->connect) {
		free(object);
		return 0;
	}

	DWORD flags = secure ? (DWORD)WINHTTP_FLAG_SECURE : 0u;
	r->request = WinHttpOpenRequest(r->connect, L"GET", object, NULL, WINHTTP_NO_REFERER,
	                                WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
	free(object);
	if (!r->request) {
		return 0;
	}

	if (!WinHttpSendRequest(r->request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
	                        WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
		return 0;
	}
	if (!WinHttpReceiveResponse(r->request, NULL)) {
		return 0;
	}

	DWORD code = 0;
	DWORD sz = (DWORD)sizeof(code);
	if (WinHttpQueryHeaders(r->request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
	                        WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz, WINHTTP_NO_HEADER_INDEX)) {
		*status = code;
	}
	return 1;
}

char *http_get(const char *url, size_t *out_len)
{
	if (out_len) {
		*out_len = 0;
	}

	struct http_req r;
	DWORD status = 0;
	if (!http_req_open(url, &r, &status)) {
		http_req_close(&r);
		return NULL;
	}
	if (status < 200 || status >= 300) {
		http_req_close(&r);
		return NULL;
	}

	size_t cap = 65536;
	size_t len = 0;
	char *buf = malloc(cap);
	if (!buf) {
		http_req_close(&r);
		return NULL;
	}

	for (;;) {
		DWORD avail = 0;
		if (!WinHttpQueryDataAvailable(r.request, &avail)) {
			free(buf);
			http_req_close(&r);
			return NULL;
		}
		if (avail == 0) {
			break;
		}
		if (len + avail + 1 > cap) {
			while (len + avail + 1 > cap) {
				cap *= 2;
			}
			char *nb = realloc(buf, cap);
			if (!nb) {
				free(buf);
				http_req_close(&r);
				return NULL;
			}
			buf = nb;
		}
		DWORD got = 0;
		if (!WinHttpReadData(r.request, buf + len, avail, &got)) {
			free(buf);
			http_req_close(&r);
			return NULL;
		}
		if (got == 0) {
			break;
		}
		len += got;
	}

	buf[len] = '\0';
	if (out_len) {
		*out_len = len;
	}
	http_req_close(&r);
	return buf;
}

int http_download(const char *url, const char *dest_path, http_progress_cb progress_cb,
                  void *userdata)
{
	struct http_req r;
	DWORD status = 0;
	if (!http_req_open(url, &r, &status)) {
		http_req_close(&r);
		return -1;
	}
	if (status < 200 || status >= 300) {
		http_req_close(&r);
		return -1;
	}

	unsigned long long total = 0;
	DWORD clen = 0;
	DWORD csz = (DWORD)sizeof(clen);
	if (WinHttpQueryHeaders(r.request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
	                        WINHTTP_HEADER_NAME_BY_INDEX, &clen, &csz, WINHTTP_NO_HEADER_INDEX)) {
		total = clen;
	}

	FILE *f = fopen(dest_path, "wb");
	if (!f) {
		http_req_close(&r);
		return -1;
	}

	unsigned long long received = 0;
	char chunk[65536];
	int ok = 1;
	for (;;) {
		DWORD avail = 0;
		if (!WinHttpQueryDataAvailable(r.request, &avail)) {
			ok = 0;
			break;
		}
		if (avail == 0) {
			break;
		}
		DWORD toread = avail;
		if (toread > (DWORD)sizeof(chunk)) {
			toread = (DWORD)sizeof(chunk);
		}
		DWORD got = 0;
		if (!WinHttpReadData(r.request, chunk, toread, &got)) {
			ok = 0;
			break;
		}
		if (got == 0) {
			break;
		}
		if (fwrite(chunk, 1, got, f) != (size_t)got) {
			ok = 0;
			break;
		}
		received += got;
		if (progress_cb) {
			progress_cb(received, total, userdata);
		}
	}

	fclose(f);
	http_req_close(&r);
	if (!ok) {
		remove(dest_path);
		return -1;
	}
	return 0;
}
