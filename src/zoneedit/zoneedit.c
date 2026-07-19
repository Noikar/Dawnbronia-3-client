/*
 * Part of Astonia content tooling (Dawnbronia). Please read license.txt.
 *
 * zoneedit - standalone zone content editor.
 *
 * It reuses the client's sprite/render code by linking the same source files
 * (the src/sdl layer + src/game/render.c) plus the zoneio parser; the client
 * itself is not modified.
 *
 * Loads a zone with zoneio and renders its terrain (gsprite/fsprite) and
 * char/item placements in the client's isometric projection, with a pannable
 * camera. Tiles can be edited (paint/erase) and saved back through zoneio.
 * Screenshots (--shot) give headless visual verification.
 *
 * Must be run with the client root as the working directory so the reused
 * loader finds res/gxN.zip.
 *
 * Interactive: WASD/arrows pan; left-click paints the brush; right-click erases;
 * middle-click eyedrops the brush; F5 saves.
 *
 * Usage:
 *   zoneedit [<area>] [--zones=<dir>] [--mapfile=<f>] [--frames=<n>]
 *            [--shot=<file.bmp>] [--set=<x,y,gsprite>]... [--out=<file.map>]
 *            [--highlight=<x,y>] [--warp=<sx,sy,area,dx,dy[,sprite]>]...
 *     <area>       numeric zone id (default 1)
 *     --zones      zones root dir (default ../astonia_community_server3/zones)
 *     --mapfile    load this .map directly instead of the zone's
 *     --set        scripted paint of a tile's gsprite (repeatable)
 *     --out        write the (edited) map here
 *     --warp       place a teleport door on (sx,sy) that warps to (dx,dy) in
 *                  `area` (area 0 = same area); generates the driver-10 item
 *                  template in the zone's warps.itm. Pure data, no server code.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <math.h>

#include <SDL3/SDL.h>

#include "../zoneio/zoneio.h"

/* Reused client entry points (declared here to avoid pulling the large client
 * headers; signatures match src/sdl/sdl.h and src/game/game.h). */
extern int sdl_init(int width, int height, char *title, int monitor);
extern void sdl_exit(void);
extern int sdl_clear(void);
extern int sdl_render(void);
extern void render_clear_clip(void);
extern void render_get_clip(int *sx, int *sy, int *ex, int *ey);
extern void render_sprite(unsigned int sprite, int scrx, int scry, char light, char align);
extern void render_line(int fx, int fy, int tx, int ty, unsigned short col);
extern void render_create_font(void); /* builds the shaded/framed fonts from font.c */
extern int render_text(int sx, int sy, unsigned short color, int flags, const char *text);
extern void render_rect_alpha(int sx, int sy, int ex, int ey, unsigned short color, unsigned char alpha);
extern void render_set_clip(int sx, int sy, int ex, int ey);
extern int sdl_multi;

/* 5-5-5 color, components 0..31 (matches the client's game.h IRGB). */
#define IRGB(r, g, b) (((r) << 10) | ((g) << 5) | ((b) << 0))
extern int sdl_cache_size;
extern int x_offset, y_offset; /* logical->window centering offset (render.c) */
extern SDL_Renderer *sdlren;

#define RENDER_ALIGN_OFFSET 0
#define RENDER_ALIGN_CENTER 1
#define NORMAL_LIGHT        15
#define FDX                 40 /* map tile width  (client astonia.h) */
#define FDY                 20 /* map tile height (client astonia.h) */
#define MAXMAP              ZIO_MAXMAP

/* ------------------------------------------------ template name -> sprite -- */
/* Placements reference char/item templates by name; we resolve those to sprite
 * ids by loading the zone's (and the generic) .chr/.itm templates. */
typedef struct {
	char name[ZIO_NAMELEN];
	unsigned int sprite;
} tmpl_entry;

static tmpl_entry *g_tmpl = NULL;
static int g_tmpl_count = 0, g_tmpl_cap = 0;

static void tmpl_add(const char *name, unsigned int sprite)
{
	if (!name[0] || !sprite) {
		return;
	}
	if (g_tmpl_count >= g_tmpl_cap) {
		int nc = g_tmpl_cap ? g_tmpl_cap * 2 : 256;
		tmpl_entry *ne = realloc(g_tmpl, sizeof(tmpl_entry) * (size_t)nc);
		if (!ne) {
			return;
		}
		g_tmpl = ne;
		g_tmpl_cap = nc;
	}
	strncpy(g_tmpl[g_tmpl_count].name, name, ZIO_NAMELEN - 1);
	g_tmpl[g_tmpl_count].name[ZIO_NAMELEN - 1] = 0;
	g_tmpl[g_tmpl_count].sprite = sprite;
	g_tmpl_count++;
}

static unsigned int tmpl_lookup(const char *name)
{
	int i;
	for (i = 0; i < g_tmpl_count; i++) {
		if (!strcmp(g_tmpl[i].name, name)) {
			return g_tmpl[i].sprite;
		}
	}
	return 0;
}

/* Load every .chr/.itm in a directory and record its name->sprite mapping. */
static void load_templates_dir(const char *dir)
{
	DIR *d = opendir(dir);
	if (!d) {
		return;
	}
	struct dirent *de;
	char path[1024];
	char err[256];
	while ((de = readdir(d))) {
		size_t n = strlen(de->d_name);
		int is_itm = (n >= 4 && !strcasecmp(de->d_name + n - 4, ".itm"));
		int is_chr = (n >= 4 && !strcasecmp(de->d_name + n - 4, ".chr"));
		if (!is_itm && !is_chr) {
			continue;
		}
		snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
		zio_file *zf = zio_load(path, is_itm ? ZIO_ITM : ZIO_CHR, err, sizeof(err));
		if (!zf) {
			fprintf(stderr, "zoneedit: skipping %s (%s)\n", path, err);
			continue;
		}
		int rc = zio_record_count(zf), i;
		for (i = 0; i < rc; i++) {
			if (is_itm) {
				zio_item *it = zio_item_at(zf, i);
				tmpl_add(it->name, (unsigned int)it->sprite);
			} else {
				zio_char *ch = zio_char_at(zf, i);
				tmpl_add(ch->name, (unsigned int)ch->sprite);
			}
		}
		zio_free(zf);
	}
	closedir(d);
}

/* Find the first *.map file in a zone directory. */
static int find_map_file(const char *zonedir, char *out, size_t outlen)
{
	DIR *d = opendir(zonedir);
	if (!d) {
		return 0;
	}
	struct dirent *de;
	int found = 0;
	while ((de = readdir(d))) {
		size_t n = strlen(de->d_name);
		if (n >= 4 && !strcasecmp(de->d_name + n - 4, ".map")) {
			snprintf(out, outlen, "%s/%s", zonedir, de->d_name);
			found = 1;
			break;
		}
	}
	closedir(d);
	return found;
}

/* --------------------------------------------------------------- rendering - */

static zio_map g_map;
static unsigned int *g_place_sprite = NULL; /* per-cell resolved placement sprite */
static int g_show_flags = 0; /* overlay move-blocked tiles         */

static inline size_t cellof(int x, int y)
{
	return (size_t)x + (size_t)y * MAXMAP;
}

/* Draw the visible portion of the zone with the camera centered on (camx,camy).
 * Tiles are emitted back-to-front (increasing mx+my) so taller foreground
 * objects correctly overdraw the ground behind them. */
static void draw_zone(double camx, double camy, int w, int h)
{
	int cx = w / 2, cy = h / 2;
	int icamx = (int)camx, icamy = (int)camy;

	/* Window of tiles that can fall on screen (generous margin for tall walls). */
	int reach = 40;
	int x0 = icamx - reach, x1 = icamx + reach;
	int y0 = icamy - reach, y1 = icamy + reach;
	if (x0 < 0) {
		x0 = 0;
	}
	if (y0 < 0) {
		y0 = 0;
	}
	if (x1 > MAXMAP - 1) {
		x1 = MAXMAP - 1;
	}
	if (y1 > MAXMAP - 1) {
		y1 = MAXMAP - 1;
	}

	int s, smin = x0 + y0, smax = x1 + y1;
	for (s = smin; s <= smax; s++) {
		int mx, mxlo = (s - y1 > x0) ? s - y1 : x0, mxhi = (s - y0 < x1) ? s - y0 : x1;
		for (mx = mxlo; mx <= mxhi; mx++) {
			int my = s - mx;
			size_t c = cellof(mx, my);

			int dx = mx - icamx, dy = my - icamy;
			int sx = (dx - dy) * (FDX / 2) + cx;
			int sy = (dx + dy) * (FDY / 2) + cy - FDY / 2;

			if (sx < -FDX * 2 || sx > w + FDX * 2 || sy < -200 || sy > h + FDY * 2) {
				continue;
			}

			/* The stored 32-bit gsprite/fsprite packs two 16-bit sprite layers
			 * (low = primary, high = secondary overlay); the client renders both
			 * (protocol.c: gsprite/gsprite2). Mirror that here. */
			unsigned int g = g_map.gsprite[c], f = g_map.fsprite[c];
			if (g & 0xFFFF) {
				render_sprite(g & 0xFFFF, sx, sy, NORMAL_LIGHT, RENDER_ALIGN_OFFSET);
			}
			if (g >> 16) {
				render_sprite(g >> 16, sx, sy, NORMAL_LIGHT, RENDER_ALIGN_OFFSET);
			}
			if (f & 0xFFFF) {
				render_sprite(f & 0xFFFF, sx, sy, NORMAL_LIGHT, RENDER_ALIGN_OFFSET);
			}
			if (f >> 16) {
				render_sprite(f >> 16, sx, sy, NORMAL_LIGHT, RENDER_ALIGN_OFFSET);
			}
			if (g_place_sprite[c]) {
				render_sprite(g_place_sprite[c], sx, sy, NORMAL_LIGHT, RENDER_ALIGN_OFFSET);
			}

			/* Flag overlay: outline move-blocked tiles in red. */
			if (g_show_flags && (g_map.flags[c] & ((1u << 0) | (1u << 2)))) { /* MF_MOVEBLOCK | MF_TMOVEBLOCK */
				unsigned short red = 0xF800;
				render_line(sx - FDX / 2, sy, sx, sy - FDY / 2, red);
				render_line(sx, sy - FDY / 2, sx + FDX / 2, sy, red);
				render_line(sx + FDX / 2, sy, sx, sy + FDY / 2, red);
				render_line(sx, sy + FDY / 2, sx - FDX / 2, sy, red);
			}
		}
	}
}

/* Center the initial camera on the populated part of the map. */
static void content_center(double *camx, double *camy)
{
	int minx = MAXMAP, miny = MAXMAP, maxx = 0, maxy = 0, any = 0;
	int x, y;
	for (y = 0; y < MAXMAP; y++) {
		for (x = 0; x < MAXMAP; x++) {
			size_t c = cellof(x, y);
			if (g_map.gsprite[c] || g_map.fsprite[c] || g_place_sprite[c]) {
				any = 1;
				if (x < minx) {
					minx = x;
				}
				if (x > maxx) {
					maxx = x;
				}
				if (y < miny) {
					miny = y;
				}
				if (y > maxy) {
					maxy = y;
				}
			}
		}
	}
	if (any) {
		*camx = (minx + maxx) / 2.0;
		*camy = (miny + maxy) / 2.0;
	} else {
		*camx = *camy = MAXMAP / 2.0;
	}
}

/* Append a char/item placement to the decoded map (grows the zio_map's list). */
static void map_add_placement(int x, int y, int is_char, const char *name)
{
	if (g_map.place_count >= g_map.place_cap) {
		int nc = g_map.place_cap ? g_map.place_cap * 2 : 64;
		zio_placement *np = realloc(g_map.place, sizeof(zio_placement) * (size_t)nc);
		if (!np) {
			return;
		}
		g_map.place = np;
		g_map.place_cap = nc;
	}
	g_map.place[g_map.place_count].x = x;
	g_map.place[g_map.place_count].y = y;
	g_map.place[g_map.place_count].is_char = is_char;
	strncpy(g_map.place[g_map.place_count].name, name, ZIO_NAMELEN - 1);
	g_map.place[g_map.place_count].name[ZIO_NAMELEN - 1] = 0;
	g_map.place_count++;
}

/* Generate an idiomatic teleport door (driver 10 = IDR_TELEPORT), append it to
 * the zone's warps.itm, and place it on tile (srcx,srcy). The server's
 * teleport_driver reads the destination straight out of the item's arg bytes
 * (little-endian u16 x, u16 y, u16 area, then a quiet flag), and warps a player
 * with change_area() when area != 0 - so a cross-area link is pure content, no
 * server code. area 0 means "same area". Returns 0 on success.
 *
 * Note: re-running --warp on a tile appends another record; duplicate it=
 * placements collapse on save (first-per-cell wins), the extra .itm record is
 * harmless but should be pruned by a future dedupe pass. */
static int add_warp(const char *zonedir, int srcx, int srcy, int area, int dx, int dy, unsigned int sprite)
{
	/* Bounds match the server: teleport_driver rejects a destination outside
	 * 1..MAXMAP-2, and change_area needs area >= 0 (0 = stay in this area). A
	 * dead warp is worse than a rejected one, so refuse to generate it. */
	if (srcx < 0 || srcy < 0 || srcx >= MAXMAP || srcy >= MAXMAP) {
		fprintf(stderr, "zoneedit: warp source (%d,%d) out of range 0..%d\n", srcx, srcy, MAXMAP - 1);
		return 1;
	}
	if (dx < 1 || dx > MAXMAP - 2 || dy < 1 || dy > MAXMAP - 2) {
		fprintf(
		    stderr, "zoneedit: warp destination (%d,%d) out of range 1..%d - would BUG in-game\n", dx, dy, MAXMAP - 2);
		return 1;
	}
	if (area < 0 || area > 0xFFFF) {
		fprintf(stderr, "zoneedit: warp area %d out of range 0..65535\n", area);
		return 1;
	}

	zio_item it;
	memset(&it, 0, sizeof(it));

	snprintf(it.name, sizeof(it.name), "warp_%d_%d", srcx, srcy);
	snprintf(it.item_name, sizeof(it.item_name), "Teleporter");
	snprintf(it.description, sizeof(it.description), "A teleporter door.");
	it.sprite = (int)sprite;
	it.driver = 10; /* IDR_TELEPORT */

	/* Give the door a light radius so it is visible (mirrors the shipped
	 * teleporter templates: mod_index=V_LIGHT, mod_value=50). */
	int vlight = zio_lookup_V("V_LIGHT");
	if (vlight >= 0) {
		it.mod_index[0] = vlight;
		it.mod_value[0] = 50;
		it.mod_count = 1;
	}

	int b;
	b = zio_lookup_IF("IF_USE");
	if (b >= 0) {
		it.flags |= (1ull << b);
	}
	b = zio_lookup_IF("IF_MOVEBLOCK");
	if (b >= 0) {
		it.flags |= (1ull << b);
	}

	/* arg = dest x (u16 LE), dest y (u16 LE), dest area (u16 LE), quiet=1 */
	it.arg[0] = (unsigned char)(dx & 0xFF);
	it.arg[1] = (unsigned char)((dx >> 8) & 0xFF);
	it.arg[2] = (unsigned char)(dy & 0xFF);
	it.arg[3] = (unsigned char)((dy >> 8) & 0xFF);
	it.arg[4] = (unsigned char)(area & 0xFF);
	it.arg[5] = (unsigned char)((area >> 8) & 0xFF);
	it.arg[6] = 1;
	it.arg_len = 7;
	it.has_arg = 1;

	char *buf = NULL;
	size_t len = 0, cap = 0;
	if (zio_item_render(&it, &buf, &len, &cap) != 0) {
		free(buf);
		fprintf(stderr, "zoneedit: warp render failed\n");
		return 1;
	}

	char itmp[1024];
	snprintf(itmp, sizeof(itmp), "%s/warps.itm", zonedir);
	FILE *f = fopen(itmp, "ab");
	if (!f) {
		fprintf(stderr, "zoneedit: cannot open %s\n", itmp);
		free(buf);
		return 1;
	}
	fputc('\n', f);
	fwrite(buf, 1, len, f);
	fputc('\n', f);
	fclose(f);
	free(buf);

	/* Place it on the map and register its sprite so it renders immediately. */
	map_add_placement(srcx, srcy, 0, it.name);
	tmpl_add(it.name, sprite);
	if (srcx >= 0 && srcy >= 0 && srcx < MAXMAP && srcy < MAXMAP) {
		g_place_sprite[cellof(srcx, srcy)] = sprite;
	}

	fprintf(stderr, "zoneedit: warp (%d,%d) -> area %d (%d,%d), template %s written to %s\n", srcx, srcy, area, dx, dy,
	    it.name, itmp);
	return 0;
}

static int file_exists(const char *path); /* defined below */

/* Scaffold a brand-new area: create zones/<N>/, write a small grass-field map,
 * and emit the mandatory `area` table INSERT (the row the server only ever
 * UPDATEs, never inserts). Returns 0 on success. */
static int create_new_area(const char *zones_root, int n, const char *name)
{
	char dir[1024], mapp[1024], sqlp[1024];
	snprintf(dir, sizeof(dir), "%s/%d", zones_root, n);

	/* Never scaffold over an existing area — that would clobber live content. */
	snprintf(mapp, sizeof(mapp), "%s/area%d.map", dir, n);
	if (file_exists(mapp)) {
		fprintf(stderr, "zoneedit: area %d already exists (%s) - refusing to overwrite.\n", n, mapp);
		return 1;
	}

	if (!SDL_CreateDirectory(dir)) {
		fprintf(stderr, "zoneedit: cannot create %s: %s\n", dir, SDL_GetError());
		return 1;
	}

	zio_map m;
	memset(&m, 0, sizeof(m));
	size_t cells = (size_t)MAXMAP * MAXMAP;
	m.gsprite = calloc(cells, sizeof(unsigned int));
	m.fsprite = calloc(cells, sizeof(unsigned int));
	m.flags = calloc(cells, sizeof(unsigned int));
	if (!m.gsprite || !m.fsprite || !m.flags) {
		zio_map_free(&m);
		return 1;
	}
	/* a 16x16 grass field near the map center */
	int x0 = 120, y0 = 120, sz = 16, x, y;
	for (y = y0; y < y0 + sz; y++) {
		for (x = x0; x < x0 + sz; x++) {
			m.gsprite[(size_t)x + (size_t)y * MAXMAP] = 12000; /* grass ground */
		}
	}
	snprintf(mapp, sizeof(mapp), "%s/area%d.map", dir, n);
	int rc = zio_map_write(&m, mapp);
	zio_map_free(&m);
	if (rc != 0) {
		fprintf(stderr, "zoneedit: cannot write %s\n", mapp);
		return 1;
	}

	const char *nm = name ? name : "New Area";
	snprintf(sqlp, sizeof(sqlp), "%s/area.sql", dir);
	FILE *f = fopen(sqlp, "wb");
	if (f) {
		fprintf(f,
		    "-- Register area %d so it is a valid teleport destination.\n"
		    "-- The server UPDATEs this row but never INSERTs it, so it must exist.\n"
		    "INSERT INTO area (ID,mirror,name,players,alive_time,idle,server,port,bps,mem_usage,last_error)\n"
		    "  VALUES (%d,1,'%s',0,UNIX_TIMESTAMP(),0,0,%d,0,0,0);\n",
		    n, n, nm, 5555 + n);
		fclose(f);
	}

	printf("Created area %d:\n", n);
	printf("  map: %s (16x16 grass field at 120,120)\n", mapp);
	printf("  sql: %s\n", sqlp);
	printf("Steps to bring it online:\n");
	printf("  1. run the SQL:  docker exec astonia3-db mysql -uroot -pastonia merc < %s\n", sqlp);
	printf("  2. ensure zones/%d has no OFFLINE marker; restart the container.\n", n);
	printf("  3. it binds port %d (=5555+%d) and self-heartbeats.\n", 5555 + n, n);
	return 0;
}

/* Convert a window-space mouse position to a map tile (inverse of the draw
 * projection). Returns 1 and fills *tx,*ty when the tile is in range. */
static int pick_tile(int mouse_x, int mouse_y, double camx, double camy, int w, int h, int *tx, int *ty)
{
	/* window -> logical draw space */
	double lx = (mouse_x - x_offset), ly = (mouse_y - y_offset);
	double cx = w / 2.0, cy = h / 2.0 - FDY / 2.0;

	double u = (lx - cx) / (FDX / 2.0); /* = dx - dy */
	double v = (ly - cy) / (FDY / 2.0); /* = dx + dy */
	double dx = (u + v) / 2.0, dy = (v - u) / 2.0;

	int mx = (int)floor(camx + dx + 0.5);
	int my = (int)floor(camy + dy + 0.5);
	if (mx < 0 || my < 0 || mx >= MAXMAP || my >= MAXMAP) {
		return 0;
	}
	*tx = mx;
	*ty = my;
	return 1;
}

/* Draw a diamond outline around tile (mx,my). */
static void draw_highlight(int mx, int my, double camx, double camy, int w, int h)
{
	int cx = w / 2, cy = h / 2;
	int dx = mx - (int)camx, dy = my - (int)camy;
	int sx = (dx - dy) * (FDX / 2) + cx;
	int sy = (dx + dy) * (FDY / 2) + cy - FDY / 2;
	unsigned short col = 0xFFE0; /* yellow (RGB565) */
	/* tile diamond: half-width FDX/2, half-height FDY/2, centered on the tile */
	render_line(sx - FDX / 2, sy, sx, sy - FDY / 2, col);
	render_line(sx, sy - FDY / 2, sx + FDX / 2, sy, col);
	render_line(sx + FDX / 2, sy, sx, sy + FDY / 2, col);
	render_line(sx, sy + FDY / 2, sx - FDX / 2, sy, col);
}

/* True if a file already exists at path. Used to protect live content: the
 * editor refuses to save over anything that existed when it launched, so an
 * existing zone can never be clobbered — only a fresh --out file is writable. */
static int file_exists(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (f) {
		fclose(f);
		return 1;
	}
	return 0;
}

/* ------------------------------------------------------- tile palette ------ *
 * A visual brush picker (RPG-Maker style): the distinct ground sprites already
 * present in the loaded map, shown as clickable thumbnails on a right-side
 * strip. Click a cell to make it the paint brush; the mouse wheel scrolls.
 * This replaces having to know/pass raw sprite numbers on the command line. */
#define PAL_CELL 44 /* cell edge, logical px          */
#define PAL_COLS 3 /* thumbnails per row             */
#define PAL_PAD  5 /* inner padding                  */
#define PAL_TOP  22 /* below the top title bar        */
#define PAL_HDR  14 /* header-label height            */

static unsigned int g_pal[8192]; /* distinct gsprite values (the brush set) */
static int g_pal_count = 0;
static int g_pal_scroll = 0; /* first visible row */

/* Collect the distinct non-empty gsprite values used across the map. */
static void palette_build(void)
{
	size_t n = (size_t)MAXMAP * MAXMAP;
	g_pal_count = 0;
	for (size_t i = 0; i < n; i++) {
		unsigned int v = g_map.gsprite[i];
		if (!(v & 0xFFFF)) {
			continue;
		}
		int seen = 0;
		for (int j = 0; j < g_pal_count; j++) {
			if (g_pal[j] == v) {
				seen = 1;
				break;
			}
		}
		if (!seen && g_pal_count < (int)(sizeof g_pal / sizeof g_pal[0])) {
			g_pal[g_pal_count++] = v;
		}
	}
}

/* Panel rect, derived from the logical draw size (right edge, between the bars). */
static void palette_rect(int w, int h, int *x0, int *y0, int *x1, int *y1)
{
	int panel_w = PAL_COLS * PAL_CELL + PAL_PAD * 2;
	*x0 = w - panel_w;
	*y0 = PAL_TOP;
	*x1 = w;
	*y1 = h - PAL_TOP;
}

static int palette_rows_visible(int w, int h)
{
	int x0, y0, x1, y1;
	palette_rect(w, h, &x0, &y0, &x1, &y1);
	int rows = (y1 - (y0 + PAL_HDR) - PAL_PAD) / PAL_CELL;
	return rows < 0 ? 0 : rows;
}

/* On-screen cell rect for palette entry idx at the current scroll; 0 if hidden. */
static int palette_cell_rect(int idx, int w, int h, int *cx0, int *cy0)
{
	int x0, y0, x1, y1;
	palette_rect(w, h, &x0, &y0, &x1, &y1);
	int row = idx / PAL_COLS - g_pal_scroll;
	int col = idx % PAL_COLS;
	if (row < 0 || row >= palette_rows_visible(w, h)) {
		return 0;
	}
	*cx0 = x0 + PAL_PAD + col * PAL_CELL;
	*cy0 = y0 + PAL_HDR + PAL_PAD + row * PAL_CELL;
	return 1;
}

/* Window-pixel click -> palette index, or -1 = panel chrome, -2 = outside panel. */
static int palette_pick(int mouse_x, int mouse_y, int w, int h)
{
	double lx = mouse_x - x_offset, ly = mouse_y - y_offset;
	int x0, y0, x1, y1;
	palette_rect(w, h, &x0, &y0, &x1, &y1);
	if (lx < x0 || lx >= x1 || ly < y0 || ly >= y1) {
		return -2;
	}
	for (int idx = 0; idx < g_pal_count; idx++) {
		int cx0, cy0;
		if (!palette_cell_rect(idx, w, h, &cx0, &cy0)) {
			continue;
		}
		if (lx >= cx0 && lx < cx0 + PAL_CELL - 2 && ly >= cy0 && ly < cy0 + PAL_CELL - 2) {
			return idx;
		}
	}
	return -1;
}

static void palette_scroll(int delta, int w, int h)
{
	int total_rows = (g_pal_count + PAL_COLS - 1) / PAL_COLS;
	int max_row = total_rows - palette_rows_visible(w, h);
	if (max_row < 0) {
		max_row = 0;
	}
	g_pal_scroll += delta;
	if (g_pal_scroll < 0) {
		g_pal_scroll = 0;
	}
	if (g_pal_scroll > max_row) {
		g_pal_scroll = max_row;
	}
}

static void draw_palette(int w, int h, unsigned int brush)
{
	int x0, y0, x1, y1;
	char hdr[64];
	palette_rect(w, h, &x0, &y0, &x1, &y1);
	render_rect_alpha(x0, y0, x1, y1, IRGB(2, 2, 5), 245);
	snprintf(hdr, sizeof hdr, "TILES %d", g_pal_count);
	render_text(x0 + PAL_PAD, y0 + 3, IRGB(31, 31, 16), 0, hdr);

	for (int idx = 0; idx < g_pal_count; idx++) {
		int cx0, cy0;
		if (!palette_cell_rect(idx, w, h, &cx0, &cy0)) {
			continue;
		}
		int cx1 = cx0 + PAL_CELL - 2, cy1 = cy0 + PAL_CELL - 2;
		render_rect_alpha(cx0, cy0, cx1, cy1, IRGB(6, 6, 11), 255);

		/* thumbnail centered + clipped to the cell (sprites draw at native size) */
		render_set_clip(cx0, cy0, cx1, cy1);
		unsigned int g = g_pal[idx];
		int mx = (cx0 + cx1) / 2, my = (cy0 + cy1) / 2;
		if (g & 0xFFFF) {
			render_sprite(g & 0xFFFF, mx, my, NORMAL_LIGHT, RENDER_ALIGN_CENTER);
		}
		if (g >> 16) {
			render_sprite(g >> 16, mx, my, NORMAL_LIGHT, RENDER_ALIGN_CENTER);
		}
		render_set_clip(0, 0, w, h);

		if (g == brush) { /* selected outline */
			unsigned short sel = IRGB(31, 31, 0);
			render_line(cx0, cy0, cx1, cy0, sel);
			render_line(cx1, cy0, cx1, cy1, sel);
			render_line(cx1, cy1, cx0, cy1, sel);
			render_line(cx0, cy1, cx0, cy0, sel);
		}
	}
	render_clear_clip();
}

/* Overlay bars: a title strip up top and a status/help strip along the bottom.
 * This is the first on-screen text (real client fonts via render_create_font),
 * the foundation the tile palette + quest forms will build on. */
static void draw_hud(
    int w, int h, int have_hover, int hx, int hy, unsigned int brush, const char *save_path, int locked)
{
	char line[256];

	/* top title bar */
	render_rect_alpha(0, 0, w, 22, IRGB(0, 0, 0), 160);
	snprintf(line, sizeof line, "zoneedit   %s", save_path ? save_path : "(no file)");
	render_text(8, 6, IRGB(31, 31, 31), 0, line);
	if (locked) {
		render_text(w - 340, 6, IRGB(31, 20, 8), 0, "READ-ONLY (relaunch with --out=NEW.map to save)");
	}

	/* bottom status + help bar */
	render_rect_alpha(0, h - 22, w, h, IRGB(0, 0, 0), 160);
	if (have_hover) {
		snprintf(line, sizeof line, "tile %d,%d   brush %u", hx, hy, brush);
	} else {
		snprintf(line, sizeof line, "brush %u   (hover a tile)", brush);
	}
	render_text(8, h - 16, IRGB(24, 31, 24), 0, line);
	render_text(w - 380, h - 16, IRGB(24, 24, 31), 0, "LMB paint  RMB erase  MMB pick  F5 save  WASD pan  ESC quit");
}

int main(int argc, char *argv[])
{
	int area = 1, max_frames = 0, i;
	const char *zones_root = "../astonia_community_server3/zones";
	const char *shot_path = NULL;
	const char *out_path = NULL; /* --out: write the (edited) map here     */
	const char *mapfile_override = NULL; /* --mapfile: load this .map directly  */
	int hl_x = -1, hl_y = -1; /* --highlight tile                        */

	/* Scripted edits (also drivable interactively): set gsprite at a tile. */
	int set_x[64], set_y[64];
	unsigned int set_g[64];
	int nset = 0;

	/* Scripted flag toggles and placements. */
	int flag_x[64], flag_y[64];
	char flag_n[64][40];
	int nflag = 0;
	int place_x[64], place_y[64], place_ischar[64];
	char place_n[64][ZIO_NAMELEN];
	int nplace = 0;
	int new_area = 0;
	const char *area_name = NULL;

	/* Scripted warps: place a teleport door at (sx,sy) -> area (dx,dy). */
	int warp_sx[64], warp_sy[64], warp_a[64], warp_dx[64], warp_dy[64];
	unsigned int warp_sp[64];
	int nwarp = 0;

	for (i = 1; i < argc; i++) {
		if (!strncmp(argv[i], "--zones=", 8)) {
			zones_root = argv[i] + 8;
		} else if (!strncmp(argv[i], "--mapfile=", 10)) {
			mapfile_override = argv[i] + 10;
		} else if (!strncmp(argv[i], "--frames=", 9)) {
			max_frames = atoi(argv[i] + 9);
		} else if (!strncmp(argv[i], "--shot=", 7)) {
			shot_path = argv[i] + 7;
		} else if (!strncmp(argv[i], "--out=", 6)) {
			out_path = argv[i] + 6;
		} else if (!strncmp(argv[i], "--set=", 6)) {
			int x, y;
			unsigned int g;
			if (nset < 64 && sscanf(argv[i] + 6, "%d,%d,%u", &x, &y, &g) == 3) {
				set_x[nset] = x;
				set_y[nset] = y;
				set_g[nset] = g;
				nset++;
			}
		} else if (!strncmp(argv[i], "--highlight=", 12)) {
			sscanf(argv[i] + 12, "%d,%d", &hl_x, &hl_y);
		} else if (!strncmp(argv[i], "--flag=", 7)) {
			if (nflag < 64 && sscanf(argv[i] + 7, "%d,%d,%39s", &flag_x[nflag], &flag_y[nflag], flag_n[nflag]) == 3) {
				nflag++;
			}
		} else if (!strncmp(argv[i], "--place=", 8)) {
			char ty[16];
			if (nplace < 64 && sscanf(argv[i] + 8, "%d,%d,%15[^,],%79s", &place_x[nplace], &place_y[nplace], ty,
			                       place_n[nplace]) == 4) {
				place_ischar[nplace] = !strcasecmp(ty, "ch");
				nplace++;
			}
		} else if (!strncmp(argv[i], "--warp=", 7)) {
			unsigned int sp = 11061; /* default teleporter-door sprite */
			int got = sscanf(argv[i] + 7, "%d,%d,%d,%d,%d,%u", &warp_sx[nwarp], &warp_sy[nwarp], &warp_a[nwarp],
			    &warp_dx[nwarp], &warp_dy[nwarp], &sp);
			if (nwarp < 64 && got >= 5) {
				warp_sp[nwarp] = sp;
				nwarp++;
			}
		} else if (!strncmp(argv[i], "--new-area=", 11)) {
			new_area = atoi(argv[i] + 11);
		} else if (!strncmp(argv[i], "--area-name=", 12)) {
			area_name = argv[i] + 12;
		} else if (!strcmp(argv[i], "--show-flags")) {
			g_show_flags = 1;
		} else if (argv[i][0] != '-') {
			area = atoi(argv[i]);
		}
	}

	/* New-area wizard: scaffold zones/<N>/ and emit the mandatory area-row SQL,
	 * then exit (no rendering needed). */
	if (new_area > 0) {
		return create_new_area(zones_root, new_area, area_name);
	}

	char zonedir[1024], mapfile[1024], genericdir[1024], err[256];
	snprintf(zonedir, sizeof(zonedir), "%s/%d", zones_root, area);
	snprintf(genericdir, sizeof(genericdir), "%s/generic", zones_root);

	if (mapfile_override) {
		snprintf(mapfile, sizeof(mapfile), "%s", mapfile_override);
	} else if (!find_map_file(zonedir, mapfile, sizeof(mapfile))) {
		fprintf(stderr, "zoneedit: no .map found in %s\n", zonedir);
		return 1;
	}

	/* Templates first (needed to resolve ch=/it= placements to sprites). */
	load_templates_dir(genericdir);
	load_templates_dir(zonedir);
	fprintf(stderr, "zoneedit: %d templates loaded\n", g_tmpl_count);

	zio_file *zf = zio_load(mapfile, ZIO_MAP, err, sizeof(err));
	if (!zf) {
		fprintf(stderr, "zoneedit: cannot load %s: %s\n", mapfile, err);
		return 1;
	}
	if (zio_map_parse(zf, &g_map, err, sizeof(err)) != 0) {
		fprintf(stderr, "zoneedit: map parse failed: %s\n", err);
		return 1;
	}
	zio_free(zf);

	g_place_sprite = calloc((size_t)MAXMAP * MAXMAP, sizeof(unsigned int));
	int resolved = 0;
	for (i = 0; i < g_map.place_count; i++) {
		unsigned int sp = tmpl_lookup(g_map.place[i].name);
		if (sp) {
			g_place_sprite[cellof(g_map.place[i].x, g_map.place[i].y)] = sp;
			resolved++;
		}
	}
	fprintf(
	    stderr, "zoneedit: zone %d: map %s, %d placements (%d resolved)\n", area, mapfile, g_map.place_count, resolved);

	/* Apply scripted edits (paint a tile's gsprite). */
	for (i = 0; i < nset; i++) {
		if (set_x[i] >= 0 && set_y[i] >= 0 && set_x[i] < MAXMAP && set_y[i] < MAXMAP) {
			g_map.gsprite[cellof(set_x[i], set_y[i])] = set_g[i];
			fprintf(stderr, "zoneedit: set gsprite(%d,%d) = %u\n", set_x[i], set_y[i], set_g[i]);
		}
	}

	/* Toggle scripted MF_ flags. */
	for (i = 0; i < nflag; i++) {
		int bit = zio_lookup_MF(flag_n[i]);
		if (bit >= 0 && flag_x[i] >= 0 && flag_y[i] >= 0 && flag_x[i] < MAXMAP && flag_y[i] < MAXMAP) {
			g_map.flags[cellof(flag_x[i], flag_y[i])] ^= (1u << bit);
			fprintf(stderr, "zoneedit: toggled %s at (%d,%d)\n", flag_n[i], flag_x[i], flag_y[i]);
		} else if (bit < 0) {
			fprintf(stderr, "zoneedit: unknown flag \"%s\"\n", flag_n[i]);
		}
	}

	/* Drop scripted char/item placements. */
	for (i = 0; i < nplace; i++) {
		if (place_x[i] < 0 || place_y[i] < 0 || place_x[i] >= MAXMAP || place_y[i] >= MAXMAP) {
			continue;
		}
		map_add_placement(place_x[i], place_y[i], place_ischar[i], place_n[i]);
		unsigned int sp = tmpl_lookup(place_n[i]);
		if (sp) {
			g_place_sprite[cellof(place_x[i], place_y[i])] = sp;
		}
		fprintf(stderr, "zoneedit: placed %s \"%s\" at (%d,%d)%s\n", place_ischar[i] ? "ch" : "it", place_n[i],
		    place_x[i], place_y[i], sp ? "" : " (unresolved sprite)");
	}

	/* Generate + place scripted warp doors (writes the zone's warps.itm). */
	for (i = 0; i < nwarp; i++) {
		add_warp(zonedir, warp_sx[i], warp_sy[i], warp_a[i], warp_dx[i], warp_dy[i], warp_sp[i]);
	}

	/* Where F5 / batch edits save, and whether that target is write-protected.
	 * Anything that already exists (every live zone) is locked so we can never
	 * overwrite it; only a brand-new --out path this session is writable. */
	const char *save_path = out_path ? out_path : mapfile;
	int save_locked = file_exists(save_path);
	if (save_locked) {
		fprintf(stderr,
		    "zoneedit: %s already exists -> READ-ONLY (saving disabled). Pass --out=NEW.map to enable saving.\n",
		    save_path);
	}

	/* Save the (edited) map if requested (blocked when the target pre-exists). */
	if (out_path && !save_locked) {
		if (zio_map_write(&g_map, out_path) == 0) {
			fprintf(stderr, "zoneedit: wrote map %s\n", out_path);
		} else {
			fprintf(stderr, "zoneedit: FAILED to write map %s\n", out_path);
		}
	}

	sdl_multi = 0;
	sdl_cache_size = 8000;
	if (!sdl_init(1280, 720, "Astonia zoneedit", 0)) {
		fprintf(stderr, "zoneedit: sdl_init failed\n");
		return 1;
	}
	render_clear_clip();
	render_create_font(); /* real client fonts (font.c) for the HUD/panels */

	/* Use the client's logical draw area (what render_clear_clip just set) for
	 * projection centering, not the physical window size. */
	int csx, csy, w, h;
	render_get_clip(&csx, &csy, &w, &h);
	if (w <= 0 || h <= 0) {
		w = 800;
		h = 650;
	}

	double camx, camy;
	content_center(&camx, &camy);
	fprintf(stderr, "zoneedit: camera at %.0f,%.0f, view %dx%d\n", camx, camy, w, h);

	unsigned int brush = g_map.gsprite[cellof((int)camx, (int)camy)]; /* eyedropper default */
	palette_build();
	int running = 1, frame = 0;
	const double pan = 1.0; /* tiles per frame while a pan key is held */
	while (running) {
		float mfx = 0, mfy = 0;
		SDL_GetMouseState(&mfx, &mfy);
		int hx, hy, have_hover = pick_tile((int)mfx, (int)mfy, camx, camy, w, h, &hx, &hy);

		SDL_Event e;
		while (SDL_PollEvent(&e)) {
			if (e.type == SDL_EVENT_QUIT) {
				running = 0;
			} else if (e.type == SDL_EVENT_KEY_DOWN) {
				if (e.key.key == SDLK_ESCAPE) {
					running = 0;
				} else if (e.key.key == SDLK_F5) {
					if (save_locked) {
						fprintf(stderr,
						    "zoneedit: save blocked - %s is a pre-existing file. Relaunch with --out=NEW.map to "
						    "save.\n",
						    save_path);
					} else if (zio_map_write(&g_map, save_path) == 0) {
						fprintf(stderr, "zoneedit: saved %s\n", save_path);
					}
				}
			} else if (e.type == SDL_EVENT_MOUSE_WHEEL) {
				palette_scroll(-(int)e.wheel.y, w, h);
			} else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
				int pk = palette_pick((int)e.button.x, (int)e.button.y, w, h);
				if (pk >= 0) {
					if (e.button.button == SDL_BUTTON_LEFT) {
						brush = g_pal[pk]; /* pick brush from palette */
					}
				} else if (pk == -2 && have_hover) { /* over the map, not the panel */
					size_t c = cellof(hx, hy);
					if (e.button.button == SDL_BUTTON_LEFT) {
						g_map.gsprite[c] = brush; /* paint */
					} else if (e.button.button == SDL_BUTTON_RIGHT) {
						g_map.gsprite[c] = g_map.fsprite[c] = g_map.flags[c] = 0; /* erase */
						g_place_sprite[c] = 0;
					} else if (e.button.button == SDL_BUTTON_MIDDLE) {
						brush = g_map.gsprite[c]; /* eyedropper */
					}
				}
			}
		}
		const bool *ks = SDL_GetKeyboardState(NULL);
		if (ks) {
			if (ks[SDL_SCANCODE_UP] || ks[SDL_SCANCODE_W]) {
				camx -= pan;
				camy -= pan;
			}
			if (ks[SDL_SCANCODE_DOWN] || ks[SDL_SCANCODE_S]) {
				camx += pan;
				camy += pan;
			}
			if (ks[SDL_SCANCODE_LEFT] || ks[SDL_SCANCODE_A]) {
				camx -= pan;
				camy += pan;
			}
			if (ks[SDL_SCANCODE_RIGHT] || ks[SDL_SCANCODE_D]) {
				camx += pan;
				camy -= pan;
			}
		}

		sdl_clear();
		render_clear_clip();
		draw_zone(camx, camy, w, h);
		if (have_hover) {
			draw_highlight(hx, hy, camx, camy, w, h);
		}
		if (hl_x >= 0 && hl_y >= 0) {
			draw_highlight(hl_x, hl_y, camx, camy, w, h);
		}
		draw_hud(w, h, have_hover, hx, hy, brush, save_path, save_locked);
		draw_palette(w, h, brush);

		if (shot_path) {
			SDL_Surface *surf = SDL_RenderReadPixels(sdlren, NULL);
			if (surf) {
				if (!SDL_SaveBMP(surf, shot_path)) {
					fprintf(stderr, "zoneedit: SaveBMP failed: %s\n", SDL_GetError());
				} else {
					fprintf(stderr, "zoneedit: wrote screenshot %s\n", shot_path);
				}
				SDL_DestroySurface(surf);
			} else {
				fprintf(stderr, "zoneedit: ReadPixels failed: %s\n", SDL_GetError());
			}
			sdl_render();
			break; /* one frame, then exit */
		}

		sdl_render();
		SDL_Delay(16);
		if (max_frames && ++frame >= max_frames) {
			running = 0;
		}
	}

	fprintf(stderr, "zoneedit: clean exit\n");
	zio_map_free(&g_map);
	free(g_place_sprite);
	free(g_tmpl);
	sdl_exit();
	return 0;
}
