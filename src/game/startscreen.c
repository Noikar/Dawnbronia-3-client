/*
 * Part of Astonia Client (c) Daniel Brockhaus. Please read license.txt.
 *
 * Start Screen (login + settings)
 *
 * Pre-game screen shown before connecting. The landing screen is an account
 * sign-in (account name + password); after signing in the player picks (or
 * creates) a character to play. There is no longer a character-name quick
 * login - the account is always the single source of truth. The account name
 * and, if "Remember login info" is ticked, the account password are remembered
 * between launches (the password lightly obfuscated, still locally
 * recoverable). Display options chosen here take effect for the session that is
 * about to start; the window resolution is read before this screen runs, so a
 * change to it applies on the next launch.
 *
 * (Passing both -u and -p on the command line bypasses this screen entirely and
 * connects directly - see main.c. That is the dev/testing shortcut.)
 *
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_keycode.h>

#include "astonia.h"
#include "game/game.h"
#include "game/account.h"
#include "sdl/sdl.h"
#include "client/client.h"

// localdata is set up in main.c (points at %appdata% dir when GO_APPDATA is on,
// otherwise NULL and we fall back to bin/data/ next to the executable).
extern char *localdata;

// Desired window size, defined in main.c. Zero means "use the desktop size"
// (borderless fullscreen). parse_args() fills these from -w / -h before
// load_settings() runs, so a command-line size always wins over the saved one.
extern int want_width, want_height;

// Server the account operations talk to (same one the game connects to). Set up
// in main.c before this screen runs.
extern char server_url[256];
extern int server_port;

// Character class flag bits (subset of the server's CF_*), used to label the
// characters returned by account_list().
#define CHR_GOD     (1u << 2)
#define CHR_WARRIOR (1u << 16)
#define CHR_MAGE    (1u << 17)
#define CHR_ARCH    (1u << 18)

// ---- persistence -----------------------------------------------------------

#define SETTINGS_MAGIC   0x41335332u // 'A3S2'
#define SETTINGS_VERSION 5

struct settings_data {
	uint32_t magic;
	uint32_t version;
	char username[40];
	int32_t remember_pw;
	int32_t pw_len;
	unsigned char pw_obf[16];
	int32_t has_options;
	uint64_t game_options;
	int32_t win_w; // desired window size; 0 = fullscreen (native desktop size)
	int32_t win_h;
	char account_user[40]; // last account name used (for prefill)
};

static struct settings_data g_set;
static int g_set_loaded = 0;

// Fixed-key obfuscation for the stored password. This is NOT encryption; it
// just keeps the password from sitting in the file as plain text. It is opt-in
// and locally recoverable by design.
static const unsigned char PW_KEY[8] = {0x5a, 0xa5, 0x3c, 0xc3, 0x69, 0x96, 0x0f, 0xf0};

static void pw_obfuscate(unsigned char *out, const char *in)
{
	size_t n = strlen(in), i;

	if (n > 16) {
		n = 16;
	}
	for (i = 0; i < 16; i++) {
		unsigned char c = (i < n) ? (unsigned char)in[i] : 0u;
		out[i] = (unsigned char)(c ^ PW_KEY[i % sizeof PW_KEY]);
	}
}

static void pw_deobfuscate(char *out, size_t out_len, const unsigned char *obf, int len)
{
	int i;

	if (len < 0) {
		len = 0;
	}
	if (len > 15) {
		len = 15;
	}
	if (out_len > 0 && (size_t)len >= out_len) {
		len = (int)out_len - 1;
	}
	for (i = 0; i < len; i++) {
		out[i] = (char)(obf[i] ^ PW_KEY[(size_t)i % sizeof PW_KEY]);
	}
	out[len] = 0;
}

static void settings_path(char *buf, size_t len)
{
	if (localdata) {
		snprintf(buf, len, "%s%s", localdata, "moac_settings.dat");
	} else {
		snprintf(buf, len, "%s", "bin/data/moac_settings.dat");
	}
}

// Called from main.c after load_options() and before sdl_init(). Loads the
// persisted settings and, if the user did not pass -o, applies their saved
// display options so they are active for this launch.
void load_settings(void)
{
	FILE *fp;
	char path[1024];
	struct settings_data sd;

	settings_path(path, sizeof(path));

	fp = fopen(path, "rb");
	if (!fp) {
		return;
	}
	// Accept the current settings version and the one just before it, so a new
	// release can migrate old files in place instead of discarding them (which
	// would wipe the remembered login and display prefs).
	if (fread(&sd, sizeof(sd), 1, fp) == 1 && sd.magic == SETTINGS_MAGIC &&
	    (sd.version == SETTINGS_VERSION || sd.version == SETTINGS_VERSION - 1)) {
		int migrated = 0;

		sd.username[sizeof(sd.username) - 1] = 0;

		// One-time migration to v5: turn the smooth camera on for players whose
		// settings predate it, so the new default reaches existing installs too.
		// Once re-saved at the current version the checkbox choice is respected.
		if (sd.has_options && sd.version < SETTINGS_VERSION) {
			sd.game_options |= GO_SMOOTHCAM;
			migrated = 1;
		}
		sd.version = SETTINGS_VERSION;

		g_set = sd;
		g_set_loaded = 1;
		if (g_set.has_options && (game_options & GO_NOTSET)) {
			game_options = g_set.game_options;
		}
		// Apply the saved windowed resolution unless -w / -h were given on the
		// command line (in which case want_* are already non-zero). A saved
		// win_w of 0 means "fullscreen (native)", which is also the default, so
		// there is nothing to do in that case.
		if (g_set.win_w > 0 && g_set.win_h > 0 && want_width == 0 && want_height == 0) {
			want_width = g_set.win_w;
			want_height = g_set.win_h;
		}
		fclose(fp);

		// Persist the upgraded settings so the migration runs only once. Writing
		// g_set directly keeps the remembered (obfuscated) password intact.
		if (migrated) {
			FILE *wf = fopen(path, "wb");
			if (wf) {
				fwrite(&g_set, sizeof(g_set), 1, wf);
				fclose(wf);
			}
		}
		return;
	}
	fclose(fp);
}

static void save_settings(
    const char *un, int remember, const char *pw, uint64_t opts, int win_w, int win_h, const char *account_user)
{
	FILE *fp;
	char path[1024];
	struct settings_data sd;

	memset(&sd, 0, sizeof(sd));
	sd.magic = SETTINGS_MAGIC;
	sd.version = SETTINGS_VERSION;
	snprintf(sd.username, sizeof(sd.username), "%s", un);
	sd.remember_pw = remember ? 1 : 0;
	if (remember) {
		size_t n = strlen(pw);
		if (n > 15) {
			n = 15;
		}
		sd.pw_len = (int32_t)n;
		pw_obfuscate(sd.pw_obf, pw);
	}
	sd.has_options = 1;
	sd.game_options = opts;
	sd.win_w = win_w;
	sd.win_h = win_h;
	if (account_user) {
		snprintf(sd.account_user, sizeof(sd.account_user), "%s", account_user);
	}

	settings_path(path, sizeof(path));

	fp = fopen(path, "wb");
	if (!fp) {
		return;
	}
	fwrite(&sd, sizeof(sd), 1, fp);
	fclose(fp);

	g_set = sd;
	g_set_loaded = 1;
}

// ---- options table ---------------------------------------------------------

static const struct {
	const char *label;
	uint64_t bit;
	int invert; // if set, the checkbox reflects/toggles the *cleared* state of bit
} opt_rows[] = {
    {"Sound", GO_SOUND, 0},
    {"Big health bar", GO_BIGBAR, 0},
    {"Dark UI", GO_DARK, 0},
    {"Large font", GO_LARGE, 0},
    {"Context menu", GO_CONTEXT, 0},
    {"Action bar", GO_ACTION, 0},
    // GO_SMALLTOP set = smaller, auto-hiding top bar; inverted so a checked box
    // means the gear bar is pinned / always visible.
    {"Pin gear bar", GO_SMALLTOP, 1},
    {"Auto-pocket items", GO_AUTOPOCKET, 0},
    {"Smooth camera", GO_SMOOTHCAM, 0},
};

#define NUM_OPTS ((int)(sizeof(opt_rows) / sizeof(opt_rows[0])))
#define CB_SIZE  11

// ---- resolution presets ----------------------------------------------------

// Index 0 is "fullscreen (native)" (want_* stay 0). The rest are windowed
// sizes. sdl_init clamps anything larger than the desktop, so oversized entries
// are harmless.
static const struct {
	int w, h;
	const char *label;
} res_presets[] = {
    {0, 0, "Fullscreen (native)"},
    {1920, 1080, "1920 x 1080"},
    {1600, 900, "1600 x 900"},
    {1366, 768, "1366 x 768"},
    {1280, 720, "1280 x 720"},
    {1024, 768, "1024 x 768"},
    {800, 600, "800 x 600"},
};

#define NUM_RES ((int)(sizeof(res_presets) / sizeof(res_presets[0])))

static int res_index_from_wh(int w, int h)
{
	int i;

	if (w <= 0 || h <= 0) {
		return 0; // fullscreen
	}
	for (i = 1; i < NUM_RES; i++) {
		if (res_presets[i].w == w && res_presets[i].h == h) {
			return i;
		}
	}
	return 0; // unknown custom size -> present as fullscreen
}

// ---- layout ----------------------------------------------------------------

// All coordinates are in the virtual 800 x YRES space; sdl_scale and the render
// offset (set in sdl_init) take care of upscaling and centering in the window.
struct ss_rects {
	int uf_x1, uf_y1, uf_x2, uf_y2; // character-name field
	int pf_x1, pf_y1, pf_x2, pf_y2; // password field
	int pb_x1, pb_y1, pb_x2, pb_y2; // play button
};

static void compute_rects(struct ss_rects *r)
{
	int lx = XRES / 2 - 190;
	int cy = YRES / 2;

	r->uf_x1 = lx;
	r->uf_x2 = lx + 170;
	r->uf_y1 = cy - 62;
	r->uf_y2 = cy - 42;

	r->pf_x1 = lx;
	r->pf_x2 = lx + 170;
	r->pf_y1 = cy - 20;
	r->pf_y2 = cy;

	r->pb_x1 = lx + 22;
	r->pb_x2 = lx + 148;
	r->pb_y1 = cy + 30;
	r->pb_y2 = cy + 58;
}

static int opt_col_x(void)
{
	return XRES / 2 + 22;
}

static int opt_row_y(int i)
{
	// Tightened row spacing so every option row plus the "Remember login info"
	// row stack above the display band (compute_disp, at cy+80) without
	// overlapping it. With N options the last row is at row index N, so keep
	// (N+1)*step below ~150 to stay inside the panel.
	return YRES / 2 - 73 + i * 15;
}

// The resolution cycler sits in a full-width band below both columns: a "<"
// arrow, the current preset label, and a ">" arrow.
struct disp_rects {
	int la_x1, la_y1, la_x2, la_y2; // left arrow
	int ra_x1, ra_y1, ra_x2, ra_y2; // right arrow
	int val_cx; // center x for the value label
	int y; // band top
};

static void compute_disp(struct disp_rects *d)
{
	int cx = XRES / 2;
	int dy = YRES / 2 + 80;

	d->y = dy;
	d->la_x1 = cx - 30;
	d->la_x2 = cx - 18;
	d->la_y1 = dy;
	d->la_y2 = dy + CB_SIZE;
	d->ra_x1 = cx + 150;
	d->ra_x2 = cx + 162;
	d->ra_y1 = dy;
	d->ra_y2 = dy + CB_SIZE;
	d->val_cx = (d->la_x2 + d->ra_x1) / 2;
}

// ---- drawing helpers -------------------------------------------------------

static void box(int sx, int sy, int ex, int ey, unsigned short color)
{
	sdl_rect(sx, sy, ex, ey, color, 0, 0, XRES, YRES, render_offset_x(), render_offset_y());
}

static void box_outline(int sx, int sy, int ex, int ey, unsigned short color)
{
	sdl_rect_outline_alpha(sx, sy, ex, ey, color, 255, 0, 0, XRES, YRES, render_offset_x(), render_offset_y());
}

static void text_centered(int cx, int y, unsigned short color, int flags, const char *txt)
{
	int w = render_text_length(flags, txt);
	render_text(cx - w / 2, y, color, flags, txt);
}

static void draw_field(int x1, int y1, int x2, int y2, int active, const char *text)
{
	unsigned short focus_col = IRGB(30, 25, 8);
	unsigned short dim_col = IRGB(9, 9, 12);

	box(x1, y1, x2, y2, IRGB(1, 1, 2));
	box_outline(x1, y1, x2, y2, active ? focus_col : dim_col);
	render_text(x1 + 6, y1 + 5, IRGB(31, 31, 31), RENDER_TEXT_LEFT, text);
	if (active) {
		int w = render_text_length(RENDER_TEXT_LEFT, text);
		box(x1 + 6 + w, y1 + 4, x1 + 7 + w, y2 - 4, focus_col);
	}
}

static void draw_checkbox(int x, int y, int on, const char *label)
{
	box(x, y, x + CB_SIZE, y + CB_SIZE, IRGB(1, 1, 2));
	box_outline(x, y, x + CB_SIZE, y + CB_SIZE, IRGB(12, 12, 16));
	if (on) {
		box(x + 3, y + 3, x + CB_SIZE - 3, y + CB_SIZE - 3, IRGB(16, 26, 10));
	}
	render_text(x + CB_SIZE + 6, y + 1, IRGB(21, 21, 23), RENDER_TEXT_LEFT, label);
}

// ---- click handling --------------------------------------------------------

static int hit(int x, int y, int x1, int y1, int x2, int y2)
{
	return (x >= x1 && x <= x2 && y >= y1 && y <= y2);
}

// ---- screen state machine --------------------------------------------------

enum ss_screen {
	SS_ACCOUNT, // sign in to an account (the landing screen)
	SS_REGISTER, // create a new account
	SS_SELECT, // pick / create a character on the signed-in account
	SS_CREATE // create a character
};

struct ss_state {
	enum ss_screen screen;
	int focus; // index of the focused text field on the current screen

	// scratch buffers handed to the game login once a character is chosen:
	// un = character name, pw = the account password (see play_char).
	char un[40], pw[16];
	int remember, res_idx;

	// register screen
	char r_user[40], r_pass[16], r_pass2[16];

	// account sign-in screen
	char a_user[40], a_pass[16];

	// create-character screen
	char c_name[40];
	int c_gender; // 0 = male, 1 = female
	int c_prof; // 0 = warrior, 1 = mage, 2 = seyan
	int c_arch; // arch variant (admin accounts only)
	int c_god; // god powers (admin accounts only)

	// character list (select screen)
	struct acc_char chars[ACC_MAXCHARS];
	int char_count;
	int is_admin; // signed-in account may create arch / god characters

	// status line
	char msg[80];
	int msg_err;

	int running, result;
};

static void setmsg(struct ss_state *st, const char *m, int err)
{
	snprintf(st->msg, sizeof(st->msg), "%s", m);
	st->msg_err = err;
}

static const char *acc_error_text(int r)
{
	switch (r) {
	case ACC_ST_BADCREDS:
		return "Wrong account name or password.";
	case ACC_ST_TAKEN:
		return "That name is already taken.";
	case ACC_ST_INVALID:
		return "Invalid entry (letters only, min 4-char password).";
	case ACC_ST_LIMIT:
		return "Character limit reached for this account.";
	case ACC_ST_SERVERERR:
		return "Server error. Please try again.";
	case ACC_NET_ERR:
		return "Could not reach the server.";
	default:
		return "Unknown error.";
	}
}

// how many text fields the current screen has (for Tab cycling)
static int field_count(enum ss_screen s)
{
	switch (s) {
	case SS_ACCOUNT:
		return 2;
	case SS_REGISTER:
		return 3;
	case SS_CREATE:
		return 1;
	default:
		return 0;
	}
}

// the text buffer for the focused field, or NULL if the screen has none
static char *active_field(struct ss_state *st, size_t *cap)
{
	switch (st->screen) {
	case SS_REGISTER:
		if (st->focus == 1) {
			*cap = sizeof(st->r_pass);
			return st->r_pass;
		}
		if (st->focus == 2) {
			*cap = sizeof(st->r_pass2);
			return st->r_pass2;
		}
		*cap = sizeof(st->r_user);
		return st->r_user;
	case SS_ACCOUNT:
		if (st->focus == 1) {
			*cap = sizeof(st->a_pass);
			return st->a_pass;
		}
		*cap = sizeof(st->a_user);
		return st->a_user;
	case SS_CREATE:
		*cap = sizeof(st->c_name);
		return st->c_name;
	default:
		*cap = 0;
		return NULL;
	}
}

// ---- account actions (blocking network calls) ------------------------------

// Persist the current display options and, when "Remember login info" is on,
// the account name + password so the sign-in screen can prefill them next time.
// When remember is off we deliberately store neither, so unchecking the box
// forgets the login. Display options and resolution are always saved.
static void persist(struct ss_state *st)
{
	const char *acct = st->remember ? st->a_user : "";

	save_settings(
	    acct, st->remember, st->a_pass, game_options, res_presets[st->res_idx].w, res_presets[st->res_idx].h, acct);
}

static void do_account_list(struct ss_state *st)
{
	int cnt = 0, r;

	if (!*st->a_user || !*st->a_pass) {
		setmsg(st, "Enter your account name and password.", 1);
		return;
	}
	r = account_list(server_url, server_port, st->a_user, st->a_pass, st->chars, ACC_MAXCHARS, &cnt, &st->is_admin);
	if (r == ACC_ST_OK) {
		st->char_count = cnt;
		st->screen = SS_SELECT;
		st->focus = 0;
		persist(st); // remember the (now validated) login for next launch
		setmsg(st, cnt ? "" : "No characters yet - create one.", 0);
	} else {
		setmsg(st, acc_error_text(r), 1);
	}
}

static void do_register(struct ss_state *st)
{
	int r;

	if (!*st->r_user || !*st->r_pass) {
		setmsg(st, "Enter an account name and password.", 1);
		return;
	}
	if (strcmp(st->r_pass, st->r_pass2) != 0) {
		setmsg(st, "Passwords do not match.", 1);
		return;
	}
	if (strlen(st->r_pass) < 4) {
		setmsg(st, "Password too short (min 4 characters).", 1);
		return;
	}
	r = account_register(server_url, server_port, st->r_user, st->r_pass);
	if (r == ACC_ST_OK) {
		// sign straight in with the new credentials and go to the character list
		snprintf(st->a_user, sizeof(st->a_user), "%s", st->r_user);
		snprintf(st->a_pass, sizeof(st->a_pass), "%s", st->r_pass);
		do_account_list(st);
		setmsg(st, "Account created - add a character.", 0);
	} else {
		setmsg(st, acc_error_text(r), 1);
	}
}

static void do_create(struct ss_state *st)
{
	int flags, r;

	if (!*st->c_name) {
		setmsg(st, "Enter a character name.", 1);
		return;
	}

	flags = (st->c_gender == 0) ? ACC_FLAG_MALE : 0;

	// profession -> warrior/mage bits (seyan sets both).
	if (st->c_prof == 1) {
		flags |= ACC_FLAG_MAGE;
	} else if (st->c_prof == 2) {
		flags |= ACC_FLAG_WARRIOR | ACC_FLAG_MAGE;
	} else {
		flags |= ACC_FLAG_WARRIOR;
	}

	// arch / god are admin-only; only offered when is_admin, and the server
	// re-checks admin regardless of what we send.
	if (st->is_admin) {
		if (st->c_arch) {
			flags |= ACC_FLAG_ARCH;
		}
		if (st->c_god) {
			flags |= ACC_FLAG_GOD;
		}
	}

	r = account_create(server_url, server_port, st->a_user, st->a_pass, st->c_name, flags);
	if (r == ACC_ST_OK) {
		st->c_name[0] = 0;
		do_account_list(st); // refresh the list and return to the select screen
		setmsg(st, "Character created.", 0);
	} else {
		setmsg(st, acc_error_text(r), 1);
	}
}

static void play_char(struct ss_state *st, const char *charname)
{
	// The game logs in with the character name + the ACCOUNT password.
	snprintf(st->un, sizeof(st->un), "%s", charname);
	snprintf(st->pw, sizeof(st->pw), "%s", st->a_pass);
	st->running = 0;
	st->result = 1;
}

// ---- immediate-mode widgets ------------------------------------------------

// Draws a button; returns 1 if it was clicked this frame.
static int ui_button(int x1, int y1, int x2, int y2, const char *label, int mx, int my, int click)
{
	int hover = hit(mx, my, x1, y1, x2, y2);

	box(x1, y1, x2, y2, hover ? IRGB(11, 17, 29) : IRGB(6, 10, 20));
	box_outline(x1, y1, x2, y2, IRGB(17, 21, 32));
	text_centered((x1 + x2) / 2, (y1 + y2) / 2 - 5, IRGB(31, 31, 31), RENDER_TEXT_LEFT, label);
	return (click && hover);
}

// Draws a horizontal segmented radio of n options, each seg_w wide, starting at
// (x, y). The selected segment is highlighted. Returns the (possibly updated)
// selection index for this frame.
static int ui_radio(
    int x, int y, int seg_w, int seg_h, const char *const *labels, int n, int sel, int mx, int my, int click)
{
	int i;

	for (i = 0; i < n; i++) {
		int x1 = x + i * seg_w, x2 = x1 + seg_w - 2;
		int hover = hit(mx, my, x1, y, x2, y + seg_h);
		int on = (i == sel);

		box(x1, y, x2, y + seg_h, on ? IRGB(16, 26, 10) : (hover ? IRGB(11, 17, 29) : IRGB(6, 10, 20)));
		box_outline(x1, y, x2, y + seg_h, on ? IRGB(20, 30, 14) : IRGB(17, 21, 32));
		text_centered(
		    (x1 + x2) / 2, y + seg_h / 2 - 5, on ? IRGB(31, 31, 31) : IRGB(23, 23, 25), RENDER_TEXT_LEFT, labels[i]);
		if (click && hover) {
			sel = i;
		}
	}
	return sel;
}

// Draws a labeled text field; clicking it focuses field 'idx'.
static void ui_field(struct ss_state *st, int idx, int x, int y, int w, const char *label, const char *text, int mask,
    int mx, int my, int click)
{
	if (click && hit(mx, my, x, y, x + w, y + 20)) {
		st->focus = idx;
	}
	render_text(x, y - 13, IRGB(17, 17, 19), RENDER_TEXT_LEFT, label);
	if (mask) {
		char m[24];
		size_t i, n = strlen(text);

		if (n > sizeof(m) - 1) {
			n = sizeof(m) - 1;
		}
		for (i = 0; i < n; i++) {
			m[i] = '*';
		}
		m[n] = 0;
		draw_field(x, y, x + w, y + 20, st->focus == idx, m);
	} else {
		draw_field(x, y, x + w, y + 20, st->focus == idx, text);
	}
}

static void draw_title(int cx, int cy)
{
	text_centered(cx, cy - 150, IRGB(31, 26, 9), RENDER_TEXT_BIG, "ASTONIA");
	text_centered(cx, cy - 134, IRGB(17, 17, 20), RENDER_TEXT_LEFT, "Community Server");
}

static void draw_msg(struct ss_state *st, int cx, int y)
{
	if (*st->msg) {
		text_centered(cx, y, st->msg_err ? IRGB(31, 12, 10) : IRGB(14, 26, 14), RENDER_TEXT_SMALL, st->msg);
	}
}

// ---- per-screen UI (draw + click in one pass) ------------------------------

static void ui_register(struct ss_state *st, int mx, int my, int click)
{
	int cx = XRES / 2, cy = YRES / 2, fx = XRES / 2 - 90, fw = 180;

	box(cx - 130, cy - 92, cx + 130, cy + 60, IRGB(3, 3, 5));
	box_outline(cx - 130, cy - 92, cx + 130, cy + 60, IRGB(11, 11, 15));
	text_centered(cx, cy - 84, IRGB(24, 24, 27), RENDER_TEXT_LEFT, "Create Account");

	ui_field(st, 0, fx, cy - 58, fw, "Account name", st->r_user, 0, mx, my, click);
	ui_field(st, 1, fx, cy - 20, fw, "Password", st->r_pass, 1, mx, my, click);
	ui_field(st, 2, fx, cy + 18, fw, "Confirm password", st->r_pass2, 1, mx, my, click);

	if (ui_button(cx - 90, cy + 66, cx - 4, cy + 86, "Back", mx, my, click)) {
		st->screen = SS_ACCOUNT;
		st->focus = 0;
		setmsg(st, "", 0);
	}
	if (ui_button(cx + 4, cy + 66, cx + 90, cy + 86, "Create", mx, my, click)) {
		do_register(st);
	}

	draw_msg(st, cx, cy + 98);
}

// The landing screen: sign in to an account on the left, display settings on
// the right. Signing in leads to the character-select screen.
static void ui_account(struct ss_state *st, int mx, int my, int click)
{
	struct ss_rects r;
	struct disp_rects d;
	int cx = XRES / 2, cy = YRES / 2, i;

	compute_rects(&r);

	// panel
	box(cx - 205, cy - 90, cx + 205, cy + 96, IRGB(3, 3, 5));
	box_outline(cx - 205, cy - 90, cx + 205, cy + 96, IRGB(11, 11, 15));

	// sign-in column
	ui_field(st, 0, r.uf_x1, r.uf_y1, r.uf_x2 - r.uf_x1, "Account name", st->a_user, 0, mx, my, click);
	ui_field(st, 1, r.pf_x1, r.pf_y1, r.pf_x2 - r.pf_x1, "Password", st->a_pass, 1, mx, my, click);

	if (ui_button(r.pb_x1, r.pb_y1, r.pb_x2, r.pb_y2, "Sign in", mx, my, click)) {
		do_account_list(st);
	}

	// options column
	render_text(opt_col_x(), cy - 80, IRGB(17, 17, 19), RENDER_TEXT_LEFT, "Options");
	for (i = 0; i < NUM_OPTS; i++) {
		int oy = opt_row_y(i);
		int checked = (game_options & opt_rows[i].bit) ? 1 : 0;
		if (opt_rows[i].invert) {
			checked = !checked;
		}
		draw_checkbox(opt_col_x(), oy, checked, opt_rows[i].label);
		if (click && hit(mx, my, opt_col_x(), oy, opt_col_x() + 150, oy + CB_SIZE)) {
			game_options ^= opt_rows[i].bit;
		}
	}
	{
		int oy = opt_row_y(NUM_OPTS);
		draw_checkbox(opt_col_x(), oy, st->remember, "Remember login info");
		if (click && hit(mx, my, opt_col_x(), oy, opt_col_x() + 150, oy + CB_SIZE)) {
			st->remember ^= 1;
		}
	}

	// display band: resolution cycler
	compute_disp(&d);
	render_text(cx - 195, d.y + 1, IRGB(17, 17, 19), RENDER_TEXT_LEFT, "Display");
	if (ui_button(d.la_x1, d.la_y1, d.la_x2, d.la_y2, "<", mx, my, click)) {
		st->res_idx = (st->res_idx + NUM_RES - 1) % NUM_RES;
	}
	if (ui_button(d.ra_x1, d.ra_y1, d.ra_x2, d.ra_y2, ">", mx, my, click)) {
		st->res_idx = (st->res_idx + 1) % NUM_RES;
	}
	text_centered(d.val_cx, d.y + 1, IRGB(28, 28, 30), RENDER_TEXT_LEFT, res_presets[st->res_idx].label);

	// new-account button (below the panel)
	if (ui_button(cx - 75, cy + 102, cx + 75, cy + 120, "New account", mx, my, click)) {
		st->screen = SS_REGISTER;
		st->focus = 0;
		setmsg(st, "", 0);
	}

	draw_msg(st, cx, cy + 128);
	text_centered(cx, cy + 140, IRGB(12, 12, 14), RENDER_TEXT_SMALL,
	    "Tab: switch field   Enter: sign in   Esc: quit   -   options apply now, resolution on restart");
}

static void ui_select(struct ss_state *st, int mx, int my, int click)
{
	int cx = XRES / 2, cy = YRES / 2, top = cy - 96, i;
	int rows = (st->char_count > 0) ? st->char_count : 1;
	int bottom = top + 44 + rows * 26;

	box(cx - 160, top, cx + 160, bottom, IRGB(3, 3, 5));
	box_outline(cx - 160, top, cx + 160, bottom, IRGB(11, 11, 15));

	{
		char hdr[96];
		snprintf(hdr, sizeof(hdr), "Characters - %s", st->a_user);
		text_centered(cx, top + 8, IRGB(24, 24, 27), RENDER_TEXT_LEFT, hdr);
	}

	if (st->char_count == 0) {
		text_centered(cx, top + 44, IRGB(15, 15, 17), RENDER_TEXT_LEFT, "No characters yet.");
	}
	for (i = 0; i < st->char_count; i++) {
		int ry = top + 40 + i * 26;
		unsigned int f = st->chars[i].flags;
		const char *prof;
		char cls[48], label[96];

		if ((f & CHR_WARRIOR) && (f & CHR_MAGE)) {
			prof = "Seyan";
		} else if (f & CHR_MAGE) {
			prof = "Mage";
		} else {
			prof = "Warrior";
		}
		snprintf(cls, sizeof(cls), "%s%s%s", (f & CHR_ARCH) ? "Arch" : "", prof, (f & CHR_GOD) ? ", God" : "");
		snprintf(label, sizeof(label), "%s  (%s)", st->chars[i].name, cls);
		render_text(cx - 150, ry + 3, IRGB(28, 28, 30), RENDER_TEXT_LEFT, label);
		if (ui_button(cx + 80, ry, cx + 150, ry + 20, "Play", mx, my, click)) {
			play_char(st, st->chars[i].name);
		}
	}

	if (ui_button(cx - 150, bottom + 6, cx - 4, bottom + 26, "Sign out", mx, my, click)) {
		st->screen = SS_ACCOUNT;
		st->focus = 0;
		setmsg(st, "", 0);
	}
	if (ui_button(cx + 4, bottom + 6, cx + 150, bottom + 26, "+ New character", mx, my, click)) {
		st->screen = SS_CREATE;
		st->focus = 0;
		st->c_name[0] = 0;
		st->c_gender = 0;
		st->c_prof = 0;
		st->c_arch = 0;
		st->c_god = 0;
		setmsg(st, "", 0);
	}

	draw_msg(st, cx, bottom + 38);
}

static void ui_create(struct ss_state *st, int mx, int my, int click)
{
	static const char *const gender_lbl[] = {"Male", "Female"};
	static const char *const prof_lbl[] = {"Warrior", "Mage", "Seyan"};
	int cx = XRES / 2, cy = YRES / 2, fx = cx - 90, fw = 180;
	int panel_top = cy - 110;
	int y_name = panel_top + 34;
	int y_gender = y_name + 40;
	int y_prof = y_gender + 40;
	int y_admin = y_prof + 34; // only used when admin
	int y_btn = st->is_admin ? y_admin + 26 : y_prof + 34;
	int panel_bot = y_btn + 28;

	box(cx - 130, panel_top, cx + 130, panel_bot, IRGB(3, 3, 5));
	box_outline(cx - 130, panel_top, cx + 130, panel_bot, IRGB(11, 11, 15));
	text_centered(cx, panel_top + 8, IRGB(24, 24, 27), RENDER_TEXT_LEFT, "New Character");

	ui_field(st, 0, fx, y_name, fw, "Character name", st->c_name, 0, mx, my, click);

	render_text(fx, y_gender - 13, IRGB(17, 17, 19), RENDER_TEXT_LEFT, "Gender");
	st->c_gender = ui_radio(fx, y_gender, fw / 2, 20, gender_lbl, 2, st->c_gender, mx, my, click);

	// Seyan is earned via an in-game quest, so it is not a normal creation
	// choice; only admin accounts may pick it directly.
	{
		int nprof = st->is_admin ? 3 : 2;
		render_text(fx, y_prof - 13, IRGB(17, 17, 19), RENDER_TEXT_LEFT, "Profession");
		st->c_prof = ui_radio(fx, y_prof, fw / nprof, 20, prof_lbl, nprof, st->c_prof, mx, my, click);
	}

	// Arch and god powers are only offered to admin accounts (the server is the
	// real gate; see do_create).
	if (st->is_admin) {
		draw_checkbox(fx, y_admin, st->c_arch, "Arch");
		if (click && hit(mx, my, fx, y_admin, fx + 80, y_admin + CB_SIZE)) {
			st->c_arch ^= 1;
		}
		draw_checkbox(fx + 90, y_admin, st->c_god, "God powers");
		if (click && hit(mx, my, fx + 90, y_admin, fx + 90 + 110, y_admin + CB_SIZE)) {
			st->c_god ^= 1;
		}
	}

	if (ui_button(cx - 90, y_btn, cx - 4, y_btn + 20, "Back", mx, my, click)) {
		st->screen = SS_SELECT;
		st->focus = 0;
		setmsg(st, "", 0);
	}
	if (ui_button(cx + 4, y_btn, cx + 90, y_btn + 20, "Create", mx, my, click)) {
		do_create(st);
	}

	draw_msg(st, cx, panel_bot + 16);
}

static void ss_draw(struct ss_state *st, int mx, int my, int click)
{
	int cx = XRES / 2, cy = YRES / 2;

	sdl_clear();
	draw_title(cx, cy);

	switch (st->screen) {
	case SS_ACCOUNT:
		ui_account(st, mx, my, click);
		break;
	case SS_REGISTER:
		ui_register(st, mx, my, click);
		break;
	case SS_SELECT:
		ui_select(st, mx, my, click);
		break;
	case SS_CREATE:
		ui_create(st, mx, my, click);
		break;
	}

	sdl_render();
}

// ---- input handling --------------------------------------------------------

static void ss_text(struct ss_state *st, const char *s)
{
	size_t cap = 0, cur;
	char *dst = active_field(st, &cap);

	if (!dst || cap == 0) {
		return;
	}
	cur = strlen(dst);
	while (*s && cur + 1 < cap) {
		if ((unsigned char)*s >= 32) {
			dst[cur++] = *s;
		}
		s++;
	}
	dst[cur] = 0;
}

static void ss_submit(struct ss_state *st)
{
	switch (st->screen) {
	case SS_ACCOUNT:
		do_account_list(st);
		break;
	case SS_REGISTER:
		do_register(st);
		break;
	case SS_CREATE:
		do_create(st);
		break;
	default:
		break;
	}
}

static void ss_back(struct ss_state *st)
{
	switch (st->screen) {
	case SS_ACCOUNT:
		// the landing screen: Esc quits the client
		st->running = 0;
		st->result = 0;
		break;
	case SS_CREATE:
		st->screen = SS_SELECT;
		break;
	default:
		st->screen = SS_ACCOUNT;
		break;
	}
	st->focus = 0;
	setmsg(st, "", 0);
}

static void ss_key(struct ss_state *st, SDL_Keycode key)
{
	switch (key) {
	case SDLK_BACKSPACE: {
		size_t cap = 0, cur;
		char *dst = active_field(st, &cap);

		if (dst && (cur = strlen(dst)) > 0) {
			dst[cur - 1] = 0;
		}
		break;
	}
	case SDLK_TAB: {
		int n = field_count(st->screen);
		if (n > 0) {
			st->focus = (st->focus + 1) % n;
		}
		break;
	}
	case SDLK_RETURN:
	case SDLK_KP_ENTER:
		ss_submit(st);
		break;
	case SDLK_ESCAPE:
		ss_back(st);
		break;
	default:
		break;
	}
}

// ---- entry point -----------------------------------------------------------

int start_screen(void)
{
	struct ss_state st;
	int mx = 0, my = 0;
	SDL_Event ev;

	memset(&st, 0, sizeof(st));
	st.screen = SS_ACCOUNT;
	st.running = 1;
	// create-character defaults (male / warrior / no arch / no god) are all zero,
	// so the memset above is enough.

	// Make sure the "no options set" sentinel is cleared so we never save it.
	game_options &= ~GO_NOTSET;

	// Prefill the sign-in from saved settings. (A direct -u / -p command-line
	// login never reaches this screen; see main.c.)
	if (g_set_loaded) {
		st.remember = g_set.remember_pw ? 1 : 0;
		st.res_idx = res_index_from_wh(g_set.win_w, g_set.win_h);
		if (*g_set.account_user) {
			snprintf(st.a_user, sizeof(st.a_user), "%s", g_set.account_user);
			snprintf(st.r_user, sizeof(st.r_user), "%s", g_set.account_user);
		}
		if (st.remember && g_set.pw_len > 0) {
			pw_deobfuscate(st.a_pass, sizeof(st.a_pass), g_set.pw_obf, g_set.pw_len);
		}
	}
	if (*st.a_user) {
		st.focus = 1; // have an account name, so start on the password
	}

	while (st.running) {
		int clicked = 0;

		while (SDL_PollEvent(&ev)) {
			switch (ev.type) {
			case SDL_EVENT_QUIT:
				st.running = 0;
				st.result = 0;
				break;
			case SDL_EVENT_TEXT_INPUT:
				ss_text(&st, ev.text.text);
				break;
			case SDL_EVENT_KEY_DOWN:
				ss_key(&st, ev.key.key);
				break;
			case SDL_EVENT_MOUSE_MOTION:
				mx = (int)ev.motion.x / sdl_scale - render_offset_x();
				my = (int)ev.motion.y / sdl_scale - render_offset_y();
				break;
			case SDL_EVENT_MOUSE_BUTTON_DOWN:
				if (ev.button.button == SDL_BUTTON_LEFT) {
					mx = (int)ev.button.x / sdl_scale - render_offset_x();
					my = (int)ev.button.y / sdl_scale - render_offset_y();
					clicked = 1;
				}
				break;
			default:
				break;
			}
		}

		ss_draw(&st, mx, my, clicked);
		SDL_Delay(16u);
	}

	if (st.result == 1) {
		// play_char put the chosen character name in st.un and the account
		// password in st.pw; hand those to the game login.
		snprintf(username, sizeof(username), "%s", st.un);
		snprintf(password, sizeof(password), "%s", st.pw);
		persist(&st);
	}

	return st.result;
}
