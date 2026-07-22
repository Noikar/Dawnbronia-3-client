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
 * middle-click eyedrops the brush; the wheel zooms over the map (and scrolls the
 * palette over the palette); F5 saves; F11 toggles borderless/windowed.
 *
 * It opens borderless over the whole display and uses all of it as canvas -- see
 * apply_layout(), which departs from the client's fixed centered 800x650.
 *
 * Usage:
 *   zoneedit [<area>] [--zones=<dir>] [--mapfile=<f>] [--frames=<n>]
 *            [--shot=<file.bmp>] [--set=<x,y,gsprite>]... [--out=<file.map>]
 *            [--highlight=<x,y>] [--warp=<sx,sy,area,dx,dy[,sprite]>]...
 *            [--link=<sx,sy,destarea,dx,dy[,sprite]>]... [--allow-live]
 *            [--windowed | --borderless]
 *     <area>       numeric zone id (default 1)
 *     --zones      zones root dir (default ../astonia_community_server3/zones)
 *     --mapfile    load this .map directly instead of the zone's
 *     --set        scripted paint of a tile's gsprite (repeatable)
 *     --out        write the (edited) map here
 *     --warp       place a one-way teleport door on (sx,sy) that warps to
 *                  (dx,dy) in `area` (area 0 = same area); generates the
 *                  driver-10 item template in the zone's warps.itm.
 *     --link       connect two maps in BOTH directions: an outbound door at
 *                  (sx,sy) here and the matching return door at (dx,dy) in
 *                  destarea. This is how you hang a new test area off an
 *                  existing one (e.g. a door in Aston to a fresh zone).
 *     --claim      mark a zone as yours to edit: `zoneedit <area>` then saves
 *     --unclaim    it in place with F5, no flags. Zones you did not claim stay
 *                  read-only, so unlocking your test map cannot unlock a
 *                  shipped one. --new-area claims the area it creates.
 *     --allow-live one-off in-place edit of an UNCLAIMED (shipped) zone; every
 *                  overwrite takes a timestamped backup first.
 *     --windowed   start in a bordered window instead of borderless fullscreen
 *                  (implied by --shot, so screenshots stay a fixed size).
 *     --borderless force borderless even for a --shot run.
 *     --zoom       start at this zoom percentage: 50, 100, 150 or 200.
 *     --palette-from
 *                  fill the brush palette from THIS zone instead of the loaded
 *                  one (edits still apply to the loaded map). The palette can
 *                  only offer sprites a map already uses, so a brand-new area
 *                  starts with one brush and needs to borrow to be paintable.
 *                  Same as the "from zone" dropdown in the palette panel.
 *     --selftest   round-trip the mouse->tile inverse against the forward
 *                  projection at every zoom, print the result and exit. A
 *                  --shot screenshot proves rendering but never input, so this
 *                  is the only headless guard on the pointer math.
 *
 * Note --out= writes a second .map. Never aim it inside a live zone folder: the
 * server loads EVERY .map in a zone dir, in unspecified readdir order, and a
 * `field=` block clears the tile first -- so two maps covering the same tile
 * clobber each other nondeterministically. Claim the zone instead.
 *
 * Warps are pure content: the server's teleport_driver already crosses areas,
 * so linking maps needs no server code change.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <math.h>
#include <time.h>

#include <SDL3/SDL.h>

#include "../zoneio/zoneio.h"

/* The sprite-variant table (res/config/animated_variants.json). Map files store
 * *virtual* sprite ids that must be resolved to a real archive sprite plus color
 * and light modifiers before drawing -- see resolve_sprite() below. */
#include "../game/sprite_config.h"

/* Mirrors src/game/game.h's struct renderfx. Declared locally (rather than
 * including game.h) to stay consistent with the hand-declared externs below;
 * keep the field order in sync with game.h if the client's struct ever changes. */
struct renderfx {
	unsigned int sprite;

	signed char sink;
	unsigned char scale;
	char cr, cg, cb;
	char clight, sat;
	unsigned short c1, c2, c3, shine;

	char light;
	unsigned char freeze;

	char ml, ll, rl, ul, dl;

	char align;
	short int clipsx, clipex;
	short int clipsy, clipey;

	unsigned char alpha;
};
typedef struct renderfx RenderFX;

extern int render_sprite_fx(RenderFX *fx, int scrx, int scry);

/* Reused client entry points (declared here to avoid pulling the large client
 * headers; signatures match src/sdl/sdl.h and src/game/game.h). */
extern int sdl_init(int width, int height, char *title, int monitor);
extern void sdl_exit(void);
extern int sdl_clear(void);
extern int sdl_render(void);
extern void render_get_clip(int *sx, int *sy, int *ex, int *ey);
extern void render_sprite(unsigned int sprite, int scrx, int scry, char light, char align);
extern void render_line(int fx, int fy, int tx, int ty, unsigned short col);
extern void render_create_font(void); /* builds the shaded/framed fonts from font.c */
extern int render_text(int sx, int sy, unsigned short color, int flags, const char *text);
extern int render_text_length(int flags, const char *text);
extern void render_rect_alpha(int sx, int sy, int ex, int ey, unsigned short color, unsigned char alpha);
extern void render_set_clip(int sx, int sy, int ex, int ey);
extern void render_set_offset(int x, int y);
extern int sdl_multi;
extern int sdl_scale; /* window pixels per logical pixel          */
extern int sdl_scale_pin; /* pins the above; see sdl_core.c          */
extern int __yres; /* logical canvas height (client YRES)     */

/* 5-5-5 color, components 0..31 (matches the client's game.h IRGB). */
#define IRGB(r, g, b) (((r) << 10) | ((g) << 5) | ((b) << 0))
extern int sdl_cache_size;
extern int x_offset, y_offset; /* logical->window centering offset (render.c) */
extern SDL_Renderer *sdlren;
extern SDL_Window *sdlwnd;

/* Logical canvas size, kept in sync by apply_layout(). Anything that wants the
 * clip rect back to "the whole canvas" must use reset_clip(), NOT the client's
 * render_clear_clip(): that one restores a fixed 800-wide XRES canvas, which is
 * the game's layout and not ours. */
static int g_view_w = 800, g_view_h = 650;

static void reset_clip(void)
{
	render_set_clip(0, 0, g_view_w, g_view_h);
}

#define RENDER_ALIGN_OFFSET 0
#define RENDER_ALIGN_CENTER 1
#define NORMAL_LIGHT        15
#define FDX                 40 /* map tile width  (client astonia.h) */
#define FDY                 20 /* map tile height (client astonia.h) */

/* ------------------------------------------------------------------- zoom --
 * Zoom is a percentage applied to both the projection (tile size) and every
 * sprite (RenderFX.scale, which the renderer also applies to the sprite's
 * anchor offsets -- sdl_image.c:804 -- so placements stay aligned).
 *
 * The steps are deliberately discrete and coarse. RenderFX.scale is part of the
 * texture cache key (sdl_texture.c:389), so each distinct zoom re-renders every
 * visible sprite into the cache; a continuous zoom would thrash it. They are
 * also all multiples of 50%, which is what keeps the half-tile dimensions whole
 * numbers (20,10 at 100%) -- a fractional half-tile would put the drawn grid and
 * the pick_tile inverse slightly out of step. */
static const int zoom_steps[] = {50, 100, 150, 200};
static int g_zoom = 100; /* percent; always one of zoom_steps */

/* Half-tile dimensions at the current zoom: the projection's basic unit. */
static int hdx(void)
{
	return FDX / 2 * g_zoom / 100;
}

static int hdy(void)
{
	return FDY / 2 * g_zoom / 100;
}

/* Move `dir` steps along zoom_steps, clamping at both ends. */
static void zoom_step(int dir)
{
	int n = (int)(sizeof zoom_steps / sizeof zoom_steps[0]), i;

	for (i = 0; i < n; i++) {
		if (zoom_steps[i] == g_zoom) {
			break;
		}
	}
	i += dir;
	if (i < 0) {
		i = 0;
	}
	if (i >= n) {
		i = n - 1;
	}
	g_zoom = zoom_steps[i];
}

#define MAXMAP ZIO_MAXMAP

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
				/* A .chr's "sprite" is a character number, not an archive sprite
				 * id -- the client expands it to 100000 + charno*1000 + frame
				 * (see _get_player_sprite). Store the idle frame so placements
				 * resolve to something drawable. */
				zio_char *ch = zio_char_at(zf, i);
				tmpl_add(ch->name, ch->sprite ? 100000 + (unsigned int)ch->sprite * 1000 : 0);
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

/* Animation clock handed to the sprite-variant system, so animated tiles (water,
 * fire, ...) cycle in the editor exactly as they do in game. Advanced once per
 * frame at the client's 24Hz world rate. */
static tick_t g_anim_tick = 0;

/* Draw one map sprite the way the client does.
 *
 * Map files store *virtual* sprite ids: anything listed in animated_variants.json
 * (e.g. 59072) is not an archive sprite at all, it is a recipe -- "sprite 21306,
 * tinted red, darkened". The client resolves these through trans_asprite()
 * before drawing; drawing the raw id instead misses every archive and lands on
 * the "unknown sprite" placeholder (the red question mark).
 *
 * This mirrors _trans_asprite() (src/game/sprite.c), including its >=100000
 * character-format branch -- terrain never uses that, but NPC placements do.
 *
 * mn is the map cell index, used only to desync position-cycle animations
 * between neighboring tiles.
 */
/* `zoom` is a percentage folded into the sprite's own scale. The map passes the
 * current zoom; the palette passes 100, so thumbnails keep a fixed size. */
static void draw_map_sprite(unsigned int sprite, size_t mn, int scrx, int scry, char align, int zoom)
{
	unsigned char scale, cr, cg, cb, light, sat;
	unsigned short c1, c2, c3, shine;
	RenderFX fx;

	if (!sprite) {
		return;
	}

	const AnimatedVariant *v = sprite_config_lookup_animated(sprite);
	unsigned int real = sprite_config_apply_animated(
	    v, mn, sprite, g_anim_tick, &scale, &cr, &cg, &cb, &light, &sat, &c1, &c2, &c3, &shine);

	/* Character-format id (100000 + charno*1000 + frame): the character number
	 * gets its own variant pass, which supplies the colors instead. */
	if (real >= 100000) {
		int charno = (int)((real - 100000) / 1000), frame = (int)(real % 1000);
		int c_scale, c_cr, c_cg, c_cb, c_light, c_sat, c_c1, c_c2, c_c3, c_shine;
		const CharacterVariant *cv = sprite_config_lookup_character(charno);
		int base = sprite_config_apply_character(cv, charno, &c_scale, &c_cr, &c_cg, &c_cb, &c_light, &c_sat, &c_c1,
		    &c_c2, &c_c3, &c_shine, (int)g_anim_tick);

		real = (unsigned int)(100000 + base * 1000 + frame);
		scale = (unsigned char)c_scale;
		cr = (unsigned char)c_cr;
		cg = (unsigned char)c_cg;
		cb = (unsigned char)c_cb;
		light = (unsigned char)c_light;
		sat = (unsigned char)c_sat;
		c1 = (unsigned short)c_c1;
		c2 = (unsigned short)c_c2;
		c3 = (unsigned short)c_c3;
		shine = (unsigned short)c_shine;
	}

	memset(&fx, 0, sizeof fx);
	fx.sprite = real;
	fx.align = align;
	fx.light = NORMAL_LIGHT;
	fx.ml = fx.ll = fx.rl = fx.ul = fx.dl = NORMAL_LIGHT;
	/* Some sprites already arrive with a variant scale of their own, so zoom
	 * multiplies into it rather than replacing it. RenderFX.scale is a byte. */
	int s = (scale ? scale : 100) * zoom / 100;
	fx.scale = (unsigned char)(s < 1 ? 1 : (s > 255 ? 255 : s));
	fx.cr = (char)cr;
	fx.cg = (char)cg;
	fx.cb = (char)cb;
	fx.clight = (char)light;
	fx.sat = (char)sat;
	fx.c1 = c1;
	fx.c2 = c2;
	fx.c3 = c3;
	fx.shine = shine;

	render_sprite_fx(&fx, scrx, scry);
}

static zio_map g_map;
static unsigned int *g_place_sprite = NULL; /* per-cell resolved placement sprite */
static unsigned char *g_char_here = NULL; /* per-cell: an NPC placement sits here */
static int g_show_flags = 0; /* overlay move-blocked tiles         */
static int g_live_edit = 0; /* --allow-live: edit zones in place  */
static int g_zone_claimed = 0; /* zone has an EDITABLE marker        */

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

	/* Window of tiles that can fall on screen. Inverting the projection, a tile
	 * is on screen while |dx-dy| <= (w/2)/hdx and |dx+dy| <= (h/2)/hdy, so neither
	 * coordinate can stray further than half their sum; the margin on top covers
	 * walls tall enough to reach in from off screen. This has to scale with both
	 * the canvas and the zoom -- a fixed reach silently stops filling the window
	 * as either grows. (A wall's height in *tiles* is zoom-independent, since the
	 * sprite and hdy scale together, so the margin stays constant.) */
	int reach = (w / hdx() + h / hdy()) / 4 + 12;
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
			int sx = (dx - dy) * hdx() + cx;
			int sy = (dx + dy) * hdy() + cy - hdy();

			/* Off-screen cull. The generous top margin lets a tall wall drawn from
			 * an off-screen tile still reach down into view, so it scales with the
			 * zoom the same way the sprite does. */
			if (sx < -hdx() * 4 || sx > w + hdx() * 4 || sy < -200 * g_zoom / 100 || sy > h + hdy() * 4) {
				continue;
			}

			/* The stored 32-bit gsprite/fsprite packs two 16-bit sprite layers
			 * (low = primary, high = secondary overlay); the client renders both
			 * (protocol.c: gsprite/gsprite2). Mirror that here. */
			unsigned int g = g_map.gsprite[c], f = g_map.fsprite[c];
			draw_map_sprite(g & 0xFFFF, c, sx, sy, RENDER_ALIGN_OFFSET, g_zoom);
			draw_map_sprite(g >> 16, c, sx, sy, RENDER_ALIGN_OFFSET, g_zoom);
			draw_map_sprite(f & 0xFFFF, c, sx, sy, RENDER_ALIGN_OFFSET, g_zoom);
			draw_map_sprite(f >> 16, c, sx, sy, RENDER_ALIGN_OFFSET, g_zoom);
			draw_map_sprite(g_place_sprite[c], c, sx, sy, RENDER_ALIGN_OFFSET, g_zoom);

			/* Flag overlay: outline move-blocked tiles in red. A tile an NPC
			 * stands on is blocked in-game too (the server raises MF_TMOVEBLOCK
			 * for it at runtime), so show those as well -- derived from the
			 * placement list rather than from a stored flag, which would bake a
			 * runtime-only flag into the file on save. */
			if (g_show_flags && ((g_map.flags[c] & (1u << 0)) || g_char_here[c])) { /* MF_MOVEBLOCK */
				unsigned short red = 0xF800;
				render_line(sx - hdx(), sy, sx, sy - hdy(), red);
				render_line(sx, sy - hdy(), sx + hdx(), sy, red);
				render_line(sx + hdx(), sy, sx, sy + hdy(), red);
				render_line(sx, sy + hdy(), sx - hdx(), sy, red);
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

/* Append a char/item placement to a decoded map (grows the zio_map's list). */
static void map_add_placement_to(zio_map *m, int x, int y, int is_char, const char *name)
{
	if (m->place_count >= m->place_cap) {
		int nc = m->place_cap ? m->place_cap * 2 : 64;
		zio_placement *np = realloc(m->place, sizeof(zio_placement) * (size_t)nc);
		if (!np) {
			return;
		}
		m->place = np;
		m->place_cap = nc;
	}
	m->place[m->place_count].x = x;
	m->place[m->place_count].y = y;
	m->place[m->place_count].is_char = is_char;
	strncpy(m->place[m->place_count].name, name, ZIO_NAMELEN - 1);
	m->place[m->place_count].name[ZIO_NAMELEN - 1] = 0;
	m->place_count++;
}

/* Same, on the map currently loaded in the editor. */
static void map_add_placement(int x, int y, int is_char, const char *name)
{
	map_add_placement_to(&g_map, x, y, is_char, name);
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
static int warp_item_build(zio_item *it, int srcx, int srcy, int area, int dx, int dy, unsigned int sprite)
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

	memset(it, 0, sizeof(*it));

	snprintf(it->name, sizeof(it->name), "warp_%d_%d", srcx, srcy);
	snprintf(it->item_name, sizeof(it->item_name), "Teleporter");
	snprintf(it->description, sizeof(it->description), "A teleporter door.");
	it->sprite = (int)sprite;
	it->driver = 10; /* IDR_TELEPORT */

	/* Give the door a light radius so it is visible (mirrors the shipped
	 * teleporter templates: mod_index=V_LIGHT, mod_value=50). */
	int vlight = zio_lookup_V("V_LIGHT");
	if (vlight >= 0) {
		it->mod_index[0] = vlight;
		it->mod_value[0] = 50;
		it->mod_count = 1;
	}

	int b;
	b = zio_lookup_IF("IF_USE");
	if (b >= 0) {
		it->flags |= (1ull << b);
	}
	b = zio_lookup_IF("IF_MOVEBLOCK");
	if (b >= 0) {
		it->flags |= (1ull << b);
	}

	/* arg = dest x (u16 LE), dest y (u16 LE), dest area (u16 LE), quiet=1 */
	it->arg[0] = (unsigned char)(dx & 0xFF);
	it->arg[1] = (unsigned char)((dx >> 8) & 0xFF);
	it->arg[2] = (unsigned char)(dy & 0xFF);
	it->arg[3] = (unsigned char)((dy >> 8) & 0xFF);
	it->arg[4] = (unsigned char)(area & 0xFF);
	it->arg[5] = (unsigned char)((area >> 8) & 0xFF);
	it->arg[6] = 1;
	it->arg_len = 7;
	it->has_arg = 1;
	return 0;
}

/* Append a generated door template to <zonedir>/warps.itm. The server loads
 * every .itm in a zone dir, so a separate file keeps generated content out of
 * the hand-authored ones. Returns 0 on success. */
static int warp_template_append(const char *zonedir, const zio_item *it)
{
	char *buf = NULL;
	size_t len = 0, cap = 0;
	if (zio_item_render(it, &buf, &len, &cap) != 0) {
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
	return 0;
}

/* Place a teleport door on the loaded map. When may_write is false the door is
 * still added in memory so it renders as a preview, but nothing is written to
 * disk -- otherwise a read-only session would leave a stray template behind in
 * a live zone with no matching placement (a half-applied edit). */
static int add_warp(
    const char *zonedir, int srcx, int srcy, int area, int dx, int dy, unsigned int sprite, int may_write)
{
	zio_item it;
	if (warp_item_build(&it, srcx, srcy, area, dx, dy, sprite) != 0) {
		return 1;
	}

	if (may_write) {
		if (warp_template_append(zonedir, &it) != 0) {
			return 1;
		}
	}

	/* Place it on the map and register its sprite so it renders immediately. */
	map_add_placement(srcx, srcy, 0, it.name);
	tmpl_add(it.name, sprite);
	if (srcx >= 0 && srcy >= 0 && srcx < MAXMAP && srcy < MAXMAP) {
		g_place_sprite[cellof(srcx, srcy)] = sprite;
	}

	if (may_write) {
		fprintf(stderr, "zoneedit: warp (%d,%d) -> area %d (%d,%d), template %s written to %s/warps.itm\n", srcx, srcy,
		    area, dx, dy, it.name, zonedir);
	} else {
		fprintf(stderr, "zoneedit: warp (%d,%d) -> area %d (%d,%d) PREVIEW ONLY (read-only session, nothing written)\n",
		    srcx, srcy, area, dx, dy);
	}
	return 0;
}

static int file_exists(const char *path); /* defined below */
static int backup_once(const char *path); /* defined below */

/* ---------------------------------------------------------- zone ownership --
 * A zone folder holding an EDITABLE marker is one you created for your own use,
 * so the editor saves it in place with no extra flag. Shipped zones have no
 * marker and stay read-only unless --allow-live is passed deliberately. This
 * makes "safe to edit" a property of the zone rather than a global switch, so
 * unlocking your test map cannot also unlock a shipped one.
 *
 * The marker has no file extension, and the server's zone loader matches on a
 * .map/.chr/.itm suffix (create.c endcmp), so it is ignored at load time. */
#define ZONE_MARKER "EDITABLE"

static void zone_marker_path(char *out, size_t outlen, const char *zones_root, int area)
{
	snprintf(out, outlen, "%s/%d/%s", zones_root, area, ZONE_MARKER);
}

static int zone_is_editable(const char *zones_root, int area)
{
	char p[1024];
	zone_marker_path(p, sizeof(p), zones_root, area);
	return file_exists(p);
}

/* Count content files in a zone, so claiming a zone full of shipped content
 * looks obviously different from claiming a fresh test area. */
static int zone_content_files(const char *zones_root, int area)
{
	char dir[1024];
	snprintf(dir, sizeof(dir), "%s/%d", zones_root, area);
	DIR *d = opendir(dir);
	if (!d) {
		return -1;
	}
	struct dirent *de;
	int n = 0;
	while ((de = readdir(d))) {
		const char *e = strrchr(de->d_name, '.');
		if (e && (!strcasecmp(e, ".map") || !strcasecmp(e, ".chr") || !strcasecmp(e, ".itm"))) {
			n++;
		}
	}
	closedir(d);
	return n;
}

/* --claim / --unclaim: mark a zone as yours to edit, or hand it back. Done
 * through the tool so the marker's name and location stay an implementation
 * detail. Returns 0 on success. */
static int zone_claim(const char *zones_root, int area, int claim)
{
	char p[1024];
	zone_marker_path(p, sizeof(p), zones_root, area);

	if (!claim) {
		remove(p);
		if (file_exists(p)) {
			fprintf(stderr, "zoneedit: could not remove %s\n", p);
			return 1;
		}
		printf("Area %d is READ-ONLY again (claim released).\n", area);
		return 0;
	}

	int files = zone_content_files(zones_root, area);
	if (files < 0) {
		fprintf(stderr, "zoneedit: no zone folder for area %d under %s\n", area, zones_root);
		return 1;
	}

	FILE *f = fopen(p, "wb");
	if (!f) {
		fprintf(stderr, "zoneedit: cannot create %s\n", p);
		return 1;
	}
	fprintf(f,
	    "This zone is claimed for editing: zoneedit may save it in place.\n"
	    "Delete this file (or run: zoneedit --unclaim=%d) to make it read-only again.\n",
	    area);
	fclose(f);

	printf("Area %d is now EDITABLE - `zoneedit %d` saves with F5, no flags needed.\n", area, area);
	if (files > 3) {
		printf("\n  NOTE: this zone holds %d content files, so it looks like authored\n"
		       "  content rather than a fresh test area. If you did not mean to claim a\n"
		       "  shipped zone, run: zoneedit --unclaim=%d\n",
		    files, area);
	}
	return 0;
}

/* Write the return half of a link: a door at (dx,dy) in `destarea` that warps
 * back to (srcx,srcy) in `srcarea`. The destination zone is not the one loaded
 * in the editor, so it is edited headlessly through zoneio -- load its .map,
 * append the door template + placement, write it back (after a backup, since
 * the destination may well be live content). Returns 0 on success. */
static int add_return_warp(
    const char *zones_root, int destarea, int dx, int dy, int srcarea, int srcx, int srcy, unsigned int sprite)
{
	char destdir[1024], destmap[1024], err[256];
	snprintf(destdir, sizeof(destdir), "%s/%d", zones_root, destarea);
	if (!find_map_file(destdir, destmap, sizeof(destmap))) {
		fprintf(stderr, "zoneedit: no .map found in %s - cannot write the return door\n", destdir);
		return 1;
	}

	zio_item it;
	if (warp_item_build(&it, dx, dy, srcarea, srcx, srcy, sprite) != 0) {
		return 1;
	}

	zio_file *zf = zio_load(destmap, ZIO_MAP, err, sizeof(err));
	if (!zf) {
		fprintf(stderr, "zoneedit: cannot load %s: %s\n", destmap, err);
		return 1;
	}
	zio_map dm;
	memset(&dm, 0, sizeof(dm));
	if (zio_map_parse(zf, &dm, err, sizeof(err)) != 0) {
		fprintf(stderr, "zoneedit: cannot parse %s: %s\n", destmap, err);
		zio_free(zf);
		return 1;
	}
	zio_free(zf);

	/* A door on a tile the map never defines would be dropped by the server's
	 * loader, so refuse rather than write a link that silently does nothing. */
	size_t cell = (size_t)dx + (size_t)dy * MAXMAP;
	if (!dm.gsprite[cell] && !dm.fsprite[cell]) {
		fprintf(stderr, "zoneedit: area %d tile (%d,%d) is empty - place ground there before linking to it\n", destarea,
		    dx, dy);
		zio_map_free(&dm);
		return 1;
	}

	if (warp_template_append(destdir, &it) != 0) {
		zio_map_free(&dm);
		return 1;
	}
	map_add_placement_to(&dm, dx, dy, 0, it.name);

	if (backup_once(destmap) != 0) {
		zio_map_free(&dm);
		return 1;
	}
	int rc = zio_map_write(&dm, destmap);
	zio_map_free(&dm);
	if (rc != 0) {
		fprintf(stderr, "zoneedit: FAILED to write %s\n", destmap);
		return 1;
	}

	fprintf(stderr, "zoneedit: return door area %d (%d,%d) -> area %d (%d,%d), saved %s\n", destarea, dx, dy, srcarea,
	    srcx, srcy, destmap);
	return 0;
}

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

	/* A zone you just scaffolded is yours by definition, so claim it now -- the
	 * whole point is to have somewhere you can paint and save freely. */
	zone_claim(zones_root, n, 1);

	printf("Created area %d:\n", n);
	printf("  map: %s (16x16 grass field at 120,120)\n", mapp);
	printf("  sql: %s\n", sqlp);
	printf("  editable: yes (claimed - `zoneedit %d` saves with F5)\n", n);
	printf("Steps to bring it online:\n");
	printf("  1. run the SQL:  docker exec astonia3-db mysql -uroot -pastonia merc < %s\n", sqlp);
	printf("  2. ensure zones/%d has no OFFLINE marker; restart the container.\n", n);
	printf("  3. it binds port %d (=5555+%d) and self-heartbeats.\n", 5555 + n, n);
	return 0;
}

/* Window pixel -> logical draw coordinate.
 *
 * The renderer draws straight into the window and scales as it goes:
 *   window = (logical + offset) * sdl_scale        (sdl_draw.c:261)
 * so the inverse needs BOTH terms. Every mouse position must come through here.
 *
 * The division is easy to lose: the tool ran at sdl_scale 1 until it went
 * borderless, and at scale 1 window and logical pixels are the same number, so
 * an offset-only inverse looks perfectly correct right up until it isn't. */
static void win_to_logical(int wx, int wy, double *lx, double *ly)
{
	*lx = (double)wx / sdl_scale - x_offset;
	*ly = (double)wy / sdl_scale - y_offset;
}

/* Convert a window-space mouse position to fractional map coordinates (the exact
 * inverse of the draw projection). Kept separate from pick_tile because zooming
 * about the cursor needs the un-rounded position. */
static void pick_tile_frac(int mouse_x, int mouse_y, double camx, double camy, int w, int h, double *fx, double *fy)
{
	double lx, ly;
	win_to_logical(mouse_x, mouse_y, &lx, &ly);

	double cx = w / 2.0, cy = h / 2.0 - hdy();

	double u = (lx - cx) / hdx(); /* = dx - dy */
	double v = (ly - cy) / hdy(); /* = dx + dy */

	/* Truncate the camera exactly as draw_zone/draw_highlight do. The renderer
	 * can only place the grid on a whole tile, so a fractional camera -- which
	 * content_center produces whenever the painted extent spans an even number
	 * of tiles -- is simply not representable on screen. Inverting against the
	 * fractional value instead puts the pick half a tile out of step with the
	 * grid that was actually drawn, which rounds to a whole tile of error. */
	*fx = (int)camx + (u + v) / 2.0;
	*fy = (int)camy + (v - u) / 2.0;
}

/* Convert a window-space mouse position to a map tile. Returns 1 and fills
 * *tx,*ty when the tile is in range. */
static int pick_tile(int mouse_x, int mouse_y, double camx, double camy, int w, int h, int *tx, int *ty)
{
	double fx, fy;

	pick_tile_frac(mouse_x, mouse_y, camx, camy, w, h, &fx, &fy);

	int mx = (int)floor(fx + 0.5);
	int my = (int)floor(fy + 0.5);
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
	int sx = (dx - dy) * hdx() + cx;
	int sy = (dx + dy) * hdy() + cy - hdy();
	unsigned short col = 0xFFE0; /* yellow (RGB565) */
	/* tile diamond: half-width hdx(), half-height hdy(), centered on the tile */
	render_line(sx - hdx(), sy, sx, sy - hdy(), col);
	render_line(sx, sy - hdy(), sx + hdx(), sy, col);
	render_line(sx + hdx(), sy, sx, sy + hdy(), col);
	render_line(sx, sy + hdy(), sx - hdx(), sy, col);
}

/* Round-trip check on the mouse -> tile inverse.
 *
 * A --shot screenshot verifies rendering but never input, which is how an
 * offset-only window->logical conversion survived the move to sdl_scale 2: the
 * hover highlight tracked a cursor twice as far from the window origin as the
 * real one, and no headless run could notice. This walks tiles through the
 * exact forward projection draw_highlight uses, converts to a window pixel the
 * way the renderer does (sdl_draw.c:261), and asserts pick_tile hands the same
 * tile back -- at every zoom, since hdx()/hdy() scale the projection too.
 *
 * Returns the number of failures. */
static int selftest_pick(double camx, double camy, int w, int h)
{
	int saved_zoom = g_zoom, fails = 0, checks = 0;
	int icamx = (int)camx, icamy = (int)camy;

	for (size_t z = 0; z < sizeof zoom_steps / sizeof zoom_steps[0]; z++) {
		g_zoom = zoom_steps[z];

		for (int dy = -6; dy <= 6; dy += 3) {
			for (int dx = -6; dx <= 6; dx += 3) {
				/* forward: tile -> logical screen -> window pixel */
				int lsx = (dx - dy) * hdx() + w / 2;
				int lsy = (dx + dy) * hdy() + h / 2 - hdy();
				int wx = (lsx + x_offset) * sdl_scale;
				int wy = (lsy + y_offset) * sdl_scale;

				int gx, gy;
				checks++;
				if (!pick_tile(wx, wy, camx, camy, w, h, &gx, &gy) || gx != icamx + dx || gy != icamy + dy) {
					fprintf(stderr, "  FAIL zoom %3d%%: tile %d,%d -> px %d,%d -> got %d,%d\n", g_zoom, icamx + dx,
					    icamy + dy, wx, wy, gx, gy);
					fails++;
				}
			}
		}
	}

	g_zoom = saved_zoom;
	fprintf(stderr, "zoneedit: selftest pick: %d/%d round-trips ok (scale %d, canvas %dx%d, offset %d,%d)\n",
	    checks - fails, checks, sdl_scale, w, h, x_offset, y_offset);
	return fails;
}

/* -------------------------------------------------------------- paint tools --
 * Every tool (click, drag, rectangle, flood) goes through paint_cell/erase_cell
 * so a tile painted by the bucket is identical to one painted by hand. */

/* Which of the two sprite layers an edit applies to. A brush remembers the
 * layer it was picked from, so paint and erase always hit the right one. */
#define LAYER_FLOOR  0
#define LAYER_OBJECT 1

typedef struct {
	unsigned int sprite;
	unsigned int flags;
	int layer; /* LAYER_FLOOR / LAYER_OBJECT */
} brush_t;

/* Apply the brush to one cell. An object carries its flags along, so a painted
 * wall blocks sight/movement the way that same wall does elsewhere in the zone. */
static void paint_cell(size_t c, brush_t b)
{
	if (b.layer == LAYER_OBJECT) {
		g_map.fsprite[c] = b.sprite;
		g_map.flags[c] = b.flags;
	} else {
		g_map.gsprite[c] = b.sprite;
	}
}

/* Clear the brush's own layer; clearing the object layer drops the flags that
 * object brought with it. */
static void erase_cell(size_t c, brush_t b)
{
	if (b.layer == LAYER_OBJECT) {
		g_map.fsprite[c] = 0;
		g_map.flags[c] = 0;
		g_place_sprite[c] = 0;
	} else {
		g_map.gsprite[c] = 0;
	}
}

static unsigned int layer_value(size_t c, int layer)
{
	return (layer == LAYER_OBJECT) ? g_map.fsprite[c] : g_map.gsprite[c];
}

/* Fill the tile rectangle spanned by two corners (either order). */
static void fill_rect(int x0, int y0, int x1, int y1, brush_t b, int erase)
{
	int t, x, y;
	if (x0 > x1) {
		t = x0;
		x0 = x1;
		x1 = t;
	}
	if (y0 > y1) {
		t = y0;
		y0 = y1;
		y1 = t;
	}
	for (y = y0; y <= y1; y++) {
		for (x = x0; x <= x1; x++) {
			if (x < 0 || y < 0 || x >= MAXMAP || y >= MAXMAP) {
				continue;
			}
			if (erase) {
				erase_cell(cellof(x, y), b);
			} else {
				paint_cell(cellof(x, y), b);
			}
		}
	}
}

/* Replace the 4-connected region of like-valued tiles under (sx,sy).
 * Iterative with an explicit queue rather than recursive: a region can span the
 * whole 256x256 grid, which would overflow the stack. */
static void flood_fill(int sx, int sy, brush_t b, int erase)
{
	static int queue[MAXMAP * MAXMAP];
	static unsigned char seen[MAXMAP * MAXMAP];
	static const int ndx[4] = {1, -1, 0, 0};
	static const int ndy[4] = {0, 0, 1, -1};

	if (sx < 0 || sy < 0 || sx >= MAXMAP || sy >= MAXMAP) {
		return;
	}
	unsigned int target = layer_value(cellof(sx, sy), b.layer);
	unsigned int repl = erase ? 0u : b.sprite;
	if (target == repl) {
		return; /* no-op, and guards against spreading forever */
	}

	memset(seen, 0, sizeof(seen));
	int head = 0, tail = 0, k;
	queue[tail++] = (int)cellof(sx, sy);
	seen[cellof(sx, sy)] = 1;

	while (head < tail) {
		int ci = queue[head++];
		int x = ci % MAXMAP, y = ci / MAXMAP;
		if (erase) {
			erase_cell((size_t)ci, b);
		} else {
			paint_cell((size_t)ci, b);
		}
		for (k = 0; k < 4; k++) {
			int nx = x + ndx[k], ny = y + ndy[k];
			if (nx < 0 || ny < 0 || nx >= MAXMAP || ny >= MAXMAP) {
				continue;
			}
			size_t nc = cellof(nx, ny);
			if (seen[nc] || layer_value(nc, b.layer) != target) {
				continue;
			}
			seen[nc] = 1;
			queue[tail++] = (int)nc;
		}
	}
}

/* Outline the tile rectangle being dragged, so the fill area is visible before
 * you commit to it. Connects the four corner tiles in screen space. */
static void draw_rect_outline(int x0, int y0, int x1, int y1, double camx, double camy, int w, int h)
{
	int cx = w / 2, cy = h / 2, t;
	if (x0 > x1) {
		t = x0;
		x0 = x1;
		x1 = t;
	}
	if (y0 > y1) {
		t = y0;
		y0 = y1;
		y1 = t;
	}

	int cor[4][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
	int px[4], py[4], i;
	for (i = 0; i < 4; i++) {
		int dx = cor[i][0] - (int)camx, dy = cor[i][1] - (int)camy;
		px[i] = (dx - dy) * hdx() + cx;
		py[i] = (dx + dy) * hdy() + cy - hdy();
	}
	unsigned short col = 0x07FF; /* cyan (RGB565) */
	for (i = 0; i < 4; i++) {
		int j = (i + 1) & 3;
		render_line(px[i], py[i], px[j], py[j], col);
	}
}

/* True if a file already exists at path. Drives the live-content guard: by
 * default the editor refuses to save over anything that existed when it
 * launched, so a live zone can never be clobbered by accident. --allow-live
 * lifts that, and every in-place overwrite is preceded by a backup. */
static int file_exists(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (f) {
		fclose(f);
		return 1;
	}
	return 0;
}

/* Copy `path` to a timestamped sibling before it is overwritten in place. Live
 * zones are real game content, so an in-place edit must always leave the
 * original recoverable. Returns 0 on success (or when there is nothing to back
 * up, i.e. the file does not exist yet). */
static int backup_file(const char *path)
{
	FILE *in = fopen(path, "rb");
	if (!in) {
		return 0; /* nothing to preserve */
	}

	time_t now = time(NULL);
	struct tm *lt = localtime(&now);
	char bak[1200];
	if (lt) {
		snprintf(bak, sizeof(bak), "%s.bak-%04d%02d%02d-%02d%02d%02d", path, lt->tm_year + 1900, lt->tm_mon + 1,
		    lt->tm_mday, lt->tm_hour, lt->tm_min, lt->tm_sec);
	} else {
		snprintf(bak, sizeof(bak), "%s.bak", path);
	}

	FILE *out = fopen(bak, "wb");
	if (!out) {
		fclose(in);
		fprintf(stderr, "zoneedit: cannot create backup %s\n", bak);
		return 1;
	}

	char buf[65536];
	size_t n;
	int rc = 0;
	while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
		if (fwrite(buf, 1, n, out) != n) {
			rc = 1;
			break;
		}
	}
	fclose(in);
	if (fclose(out) != 0) {
		rc = 1;
	}
	if (rc == 0) {
		fprintf(stderr, "zoneedit: backed up %s -> %s\n", path, bak);
	} else {
		fprintf(stderr, "zoneedit: FAILED to write backup %s\n", bak);
	}
	return rc;
}

/* Back a path up the first time it is written this session. Repeat saves (F5 in
 * a long editing session) then keep the pristine original rather than burying
 * it under a pile of near-identical snapshots. Returns 0 when it is safe to
 * proceed with the write. */
#define MAX_BACKED_UP 8
static char g_backed_up[MAX_BACKED_UP][1024];
static int g_backed_up_count = 0;

static int backup_once(const char *path)
{
	int i;
	for (i = 0; i < g_backed_up_count; i++) {
		if (!strcmp(g_backed_up[i], path)) {
			return 0;
		}
	}
	if (backup_file(path) != 0) {
		fprintf(stderr, "zoneedit: refusing to overwrite %s without a backup\n", path);
		return 1;
	}
	if (g_backed_up_count < MAX_BACKED_UP) {
		snprintf(g_backed_up[g_backed_up_count], sizeof(g_backed_up[0]), "%s", path);
		g_backed_up_count++;
	}
	return 0;
}

/* Save the loaded map, taking a one-time backup if the target already exists. */
static int save_map(const zio_map *m, const char *path)
{
	if (backup_once(path) != 0) {
		return 1;
	}
	return zio_map_write(m, path);
}

/* --------------------------------------------------------- zone list ------- *
 * The zones a palette can be borrowed from: every numeric folder under the
 * zones root that actually holds a .map. Each is labeled with its number plus
 * the base name of that map ("3 above3"), which says a good deal more about
 * what a zone is than the bare number does. */
#define ZL_MAX 256

typedef struct {
	int area;
	char label[24];
} zone_entry;

static zone_entry g_zones[ZL_MAX];
static int g_zone_count = 0;

static int zone_entry_cmp(const void *a, const void *b)
{
	const zone_entry *za = a, *zb = b;
	return za->area - zb->area;
}

static void zone_list_build(const char *zones_root)
{
	DIR *d = opendir(zones_root);
	if (!d) {
		fprintf(stderr, "zoneedit: cannot scan %s for zones\n", zones_root);
		return;
	}

	struct dirent *de;
	g_zone_count = 0;
	while ((de = readdir(d)) && g_zone_count < ZL_MAX) {
		const char *n = de->d_name;
		if (!*n || strspn(n, "0123456789") != strlen(n)) {
			continue; /* ".", "..", "generic" */
		}

		char dir[1024], mapfile[1024];
		snprintf(dir, sizeof dir, "%s/%s", zones_root, n);
		if (!find_map_file(dir, mapfile, sizeof mapfile)) {
			continue; /* no map -> no brushes to harvest */
		}

		const char *base = strrchr(mapfile, '/');
		base = base ? base + 1 : mapfile;

		char stem[24];
		snprintf(stem, sizeof stem, "%s", base);
		char *dot = strrchr(stem, '.');
		if (dot) {
			*dot = 0;
		}

		g_zones[g_zone_count].area = atoi(n);
		snprintf(g_zones[g_zone_count].label, sizeof g_zones[0].label, "%s %s", n, stem);
		g_zone_count++;
	}
	closedir(d);

	qsort(g_zones, (size_t)g_zone_count, sizeof g_zones[0], zone_entry_cmp);
}

static const char *zone_list_label(int idx)
{
	return (idx >= 0 && idx < g_zone_count) ? g_zones[idx].label : "";
}

/* ---------------------------------------------------------- dropdown ------- *
 * The tool's first real widget: a labeled button that opens a scrolling popup
 * list. It is written against the pattern the tile palette proved out -- an
 * alpha-filled backdrop, a clip rect per row, and a *_pick() that converts
 * window pixels to logical ones through x_offset/y_offset -- and deliberately
 * knows nothing about zones: items come in through a label callback. That is
 * what lets the reusable widget layer (G2) lift it out unchanged when buttons,
 * checkboxes and text fields join it.
 *
 * The popup is drawn last and hit-tested first, so it sits above the palette
 * and can overhang the map without clicks falling through to the paint tools. */
#define DD_ROWH    12 /* row height, logical px          */
#define DD_MAXROWS 20 /* rows before the list scrolls    */
#define DD_WIDTH   152 /* popup width (wider than the palette panel) */

typedef struct {
	int open;
	int scroll; /* first visible row */
} dropdown_t;

typedef const char *(*dd_label_fn)(int idx);

/* Pick results that are not an item index. */
#define DD_NONE   (-3) /* nowhere near the widget -> not ours          */
#define DD_BUTTON (-2) /* on the button -> toggle the popup            */
#define DD_CHROME (-1) /* on popup chrome -> swallow, keep it open     */

static int dropdown_rows_visible(int count, int by1, int h)
{
	int rows = count < DD_MAXROWS ? count : DD_MAXROWS;
	int room = (h - 22 - by1) / DD_ROWH; /* stop short of the status bar */

	if (rows > room) {
		rows = room;
	}
	return rows < 1 ? 1 : rows;
}

/* Popup rect: hangs below the button, right edges aligned so a popup wider than
 * its button grows leftward over the map rather than off the canvas. */
static void dropdown_popup_rect(int count, int bx1, int by1, int h, int *px0, int *py0, int *px1, int *py1)
{
	int rows = dropdown_rows_visible(count, by1, h);

	*px1 = bx1;
	*px0 = bx1 - DD_WIDTH;
	*py0 = by1;
	*py1 = by1 + rows * DD_ROWH + 2;
}

static int dropdown_pick(const dropdown_t *dd, int mouse_x, int mouse_y, int count, int bx0, int by0, int bx1, int by1,
    int h, int *out_hover)
{
	double lx, ly;
	win_to_logical(mouse_x, mouse_y, &lx, &ly);

	if (out_hover) {
		*out_hover = -1;
	}

	if (dd->open) {
		int px0, py0, px1, py1;
		dropdown_popup_rect(count, bx1, by1, h, &px0, &py0, &px1, &py1);
		if (lx >= px0 && lx < px1 && ly >= py0 && ly < py1) {
			int row = (int)((ly - py0 - 1) / DD_ROWH);
			int idx = dd->scroll + row;
			if (row >= 0 && row < dropdown_rows_visible(count, by1, h) && idx >= 0 && idx < count) {
				if (out_hover) {
					*out_hover = idx;
				}
				return idx;
			}
			return DD_CHROME;
		}
	}

	if (lx >= bx0 && lx < bx1 && ly >= by0 && ly < by1) {
		return DD_BUTTON;
	}
	return DD_NONE;
}

static void dropdown_scroll(dropdown_t *dd, int delta, int count, int by1, int h)
{
	int max_row = count - dropdown_rows_visible(count, by1, h);

	if (max_row < 0) {
		max_row = 0;
	}
	dd->scroll += delta;
	if (dd->scroll < 0) {
		dd->scroll = 0;
	}
	if (dd->scroll > max_row) {
		dd->scroll = max_row;
	}
}

/* The button. Drawn in place in the panel; `value` is the current selection. */
static void dropdown_draw_button(const dropdown_t *dd, const char *caption, const char *value, int bx0, int by0,
    int bx1, int by1, int w, int h, unsigned short value_color)
{
	char line[64];

	render_rect_alpha(bx0 + 1, by0, bx1 - 1, by1 - 1, dd->open ? IRGB(10, 10, 18) : IRGB(5, 5, 10), 255);
	render_set_clip(bx0 + 1, by0, bx1 - 11, by1 - 1);
	snprintf(line, sizeof line, "%s %s", caption, value);
	render_text(bx0 + 4, by0 + 3, value_color, 0, line);
	render_set_clip(0, 0, w, h);
	render_text(bx1 - 9, by0 + 3, IRGB(20, 20, 24), 0, dd->open ? "^" : "v");
}

/* The popup list. Kept separate from the button so the caller can draw it LAST,
 * over everything else -- a popup that overhangs the panel must not end up
 * underneath the palette cells that are painted after it. `sel` is the current
 * item, `hover` the one under the cursor; both may be -1. */
static void dropdown_draw_popup(
    const dropdown_t *dd, dd_label_fn label, int count, int sel, int hover, int bx1, int by1, int w, int h)
{
	if (!dd->open) {
		return;
	}

	int px0, py0, px1, py1;
	dropdown_popup_rect(count, bx1, by1, h, &px0, &py0, &px1, &py1);
	render_rect_alpha(px0, py0, px1, py1, IRGB(2, 2, 6), 250);

	unsigned short edge = IRGB(14, 14, 20);
	render_line(px0, py0, px1, py0, edge);
	render_line(px1, py0, px1, py1, edge);
	render_line(px1, py1, px0, py1, edge);
	render_line(px0, py1, px0, py0, edge);

	int rows = dropdown_rows_visible(count, by1, h);
	for (int r = 0; r < rows; r++) {
		int idx = dd->scroll + r;
		if (idx >= count) {
			break;
		}

		int ry = py0 + 1 + r * DD_ROWH;
		if (idx == hover) {
			render_rect_alpha(px0 + 1, ry, px1 - 1, ry + DD_ROWH, IRGB(8, 8, 16), 255);
		}

		render_set_clip(px0 + 2, ry, px1 - 2, ry + DD_ROWH);
		render_text(px0 + 5, ry + 2, (idx == sel) ? IRGB(31, 31, 16) : IRGB(22, 22, 26), 0, label(idx));
		render_set_clip(0, 0, w, h);
	}

	/* "there is more below/above" marks, since the list has no scrollbar yet */
	if (dd->scroll > 0) {
		render_text(px1 - 10, py0 + 1, IRGB(16, 16, 20), 0, "^");
	}
	if (dd->scroll + rows < count) {
		render_text(px1 - 10, py1 - DD_ROWH, IRGB(16, 16, 20), 0, "v");
	}
}

/* ------------------------------------------------------- tile palette ------ *
 * A visual brush picker (RPG-Maker style): the distinct sprites already present
 * in the loaded map, shown as clickable thumbnails on a right-side strip. Click
 * a cell to make it the paint brush; the mouse wheel scrolls. This replaces
 * having to know/pass raw sprite numbers on the command line.
 *
 * Maps have two sprite layers and the palette carries both, on switchable tabs:
 *   FLOORS  = gsprite, the ground (grass, dirt, flagstone)
 *   OBJECTS = fsprite, everything standing on it (walls, doors, furniture)
 * A brush remembers which layer it came from, so painting writes back to the
 * right one with no mode to remember. */
#define PAL_CELL 44 /* cell edge, logical px          */
#define PAL_COLS 3 /* thumbnails per row             */
#define PAL_PAD  5 /* inner padding                  */
#define PAL_TOP  22 /* below the top title bar        */
#define PAL_HDR  14 /* header-label height            */
#define PAL_TABH 16 /* tab-strip height               */

#define PAL_SRCH 15 /* palette-source selector height */

typedef struct {
	unsigned int sprite; /* packed 32-bit value as stored in the map      */
	unsigned int flags; /* flags this sprite most often carries (objects) */
} pal_entry;

static pal_entry g_pal[2][8192]; /* [layer][idx] */
static int g_pal_count[2] = {0, 0};
static int g_pal_scroll[2] = {0, 0}; /* first visible row, per tab */
static int g_pal_tab = LAYER_FLOOR;
static int g_pal_src_area = 0; /* zone the brushes were harvested from */

/* Flag combinations seen for one sprite, so we can pick the most common. */
#define PAL_MAXCOMBO 8

typedef struct {
	unsigned int flags;
	int count;
} flag_combo;

/* Collect the distinct non-empty sprites of both layers.
 *
 * For objects we also learn their flags: the same wall sprite is placed dozens
 * of times across a zone, nearly always with the same MF_SIGHTBLOCK/MF_MOVEBLOCK
 * combination. Recording the most common combination lets a painted wall behave
 * like a wall immediately, while a rug or a low decoration keeps its (empty)
 * flags -- which a blanket "objects always block" rule would get wrong. */
static void palette_build_from(const zio_map *src)
{
	static flag_combo combos[8192][PAL_MAXCOMBO];
	static int combo_count[8192];
	size_t n = (size_t)MAXMAP * MAXMAP;

	g_pal_count[LAYER_FLOOR] = g_pal_count[LAYER_OBJECT] = 0;
	g_pal_scroll[LAYER_FLOOR] = g_pal_scroll[LAYER_OBJECT] = 0;
	memset(combo_count, 0, sizeof combo_count);

	for (size_t i = 0; i < n; i++) {
		for (int layer = 0; layer < 2; layer++) {
			unsigned int v = (layer == LAYER_FLOOR) ? src->gsprite[i] : src->fsprite[i];
			if (!(v & 0xFFFF)) {
				continue;
			}

			int idx = -1;
			for (int j = 0; j < g_pal_count[layer]; j++) {
				if (g_pal[layer][j].sprite == v) {
					idx = j;
					break;
				}
			}
			if (idx < 0) {
				if (g_pal_count[layer] >= (int)(sizeof g_pal[0] / sizeof g_pal[0][0])) {
					continue;
				}
				idx = g_pal_count[layer]++;
				g_pal[layer][idx].sprite = v;
				g_pal[layer][idx].flags = 0;
			}

			if (layer != LAYER_OBJECT) {
				continue;
			}

			/* Tally this placement's flag combination for the object brush. */
			unsigned int fl = src->flags[i];
			int c;
			for (c = 0; c < combo_count[idx]; c++) {
				if (combos[idx][c].flags == fl) {
					combos[idx][c].count++;
					break;
				}
			}
			if (c == combo_count[idx] && c < PAL_MAXCOMBO) {
				combos[idx][c].flags = fl;
				combos[idx][c].count = 1;
				combo_count[idx]++;
			}
		}
	}

	/* Settle each object brush on its most frequently seen flag combination. */
	for (int idx = 0; idx < g_pal_count[LAYER_OBJECT]; idx++) {
		int best = -1;
		for (int c = 0; c < combo_count[idx]; c++) {
			if (best < 0 || combos[idx][c].count > combos[idx][best].count) {
				best = c;
			}
		}
		g_pal[LAYER_OBJECT][idx].flags = (best >= 0) ? combos[idx][best].flags : 0;
	}
}

/* Point the palette at another zone's brushes.
 *
 * palette_build_from() can only ever offer sprites that are already placed on a
 * map, which is exactly what a brand-new area has none of: the new-area wizard's
 * grass field yields a palette of a single brush, so the map cannot be painted
 * with anything but the sprite it was born with. Borrowing loads the source
 * zone's .map headlessly and harvests its brushes, while every edit still
 * applies to the map that is open -- the palette is a catalog, not the content.
 *
 * Returns 0 on success; on failure the previous palette is left untouched. */
static int palette_borrow(const char *zones_root, int from_area, int loaded_area)
{
	if (from_area == loaded_area) { /* the loaded map is already in memory */
		palette_build_from(&g_map);
		g_pal_src_area = loaded_area;
		return 0;
	}

	char dir[1024], mapfile[1024], err[256];
	snprintf(dir, sizeof dir, "%s/%d", zones_root, from_area);
	if (!find_map_file(dir, mapfile, sizeof mapfile)) {
		fprintf(stderr, "zoneedit: no .map in %s - palette unchanged\n", dir);
		return 1;
	}

	zio_file *zf = zio_load(mapfile, ZIO_MAP, err, sizeof err);
	if (!zf) {
		fprintf(stderr, "zoneedit: cannot load %s: %s - palette unchanged\n", mapfile, err);
		return 1;
	}

	zio_map src;
	int rc = zio_map_parse(zf, &src, err, sizeof err);
	zio_free(zf);
	if (rc != 0) {
		fprintf(stderr, "zoneedit: cannot parse %s: %s - palette unchanged\n", mapfile, err);
		return 1;
	}

	palette_build_from(&src);
	zio_map_free(&src);
	g_pal_src_area = from_area;
	fprintf(stderr, "zoneedit: palette borrowed from zone %d (%s): %d floor, %d object brushes\n", from_area, mapfile,
	    g_pal_count[LAYER_FLOOR], g_pal_count[LAYER_OBJECT]);
	return 0;
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
	int rows = (y1 - (y0 + PAL_HDR + PAL_SRCH + PAL_TABH) - PAL_PAD) / PAL_CELL;
	return rows < 0 ? 0 : rows;
}

/* Rect of the "borrow brushes from zone N" selector button, under the header. */
static void palette_src_rect(int w, int h, int *sx0, int *sy0, int *sx1, int *sy1)
{
	int x0, y0, x1, y1;
	palette_rect(w, h, &x0, &y0, &x1, &y1);
	*sx0 = x0;
	*sy0 = y0 + PAL_HDR;
	*sx1 = x1;
	*sy1 = *sy0 + PAL_SRCH;
}

/* Rect of tab `tab` in the tab strip. */
static void palette_tab_rect(int tab, int w, int h, int *tx0, int *ty0, int *tx1, int *ty1)
{
	int x0, y0, x1, y1;
	palette_rect(w, h, &x0, &y0, &x1, &y1);
	int half = (x1 - x0) / 2;
	*tx0 = x0 + tab * half;
	*ty0 = y0 + PAL_HDR + PAL_SRCH;
	*tx1 = (tab == 0) ? x0 + half : x1;
	*ty1 = *ty0 + PAL_TABH;
}

/* On-screen cell rect for palette entry idx at the current scroll; 0 if hidden. */
static int palette_cell_rect(int idx, int w, int h, int *cx0, int *cy0)
{
	int x0, y0, x1, y1;
	palette_rect(w, h, &x0, &y0, &x1, &y1);
	int row = idx / PAL_COLS - g_pal_scroll[g_pal_tab];
	int col = idx % PAL_COLS;
	if (row < 0 || row >= palette_rows_visible(w, h)) {
		return 0;
	}
	*cx0 = x0 + PAL_PAD + col * PAL_CELL;
	*cy0 = y0 + PAL_HDR + PAL_SRCH + PAL_TABH + PAL_PAD + row * PAL_CELL;
	return 1;
}

/* Click routing results that are not a cell index. */
#define PAL_OUTSIDE (-2) /* not over the panel at all -> let the map have it */
#define PAL_CHROME  (-1) /* panel background -> swallow the click           */
#define PAL_TAB0    (-3)
#define PAL_TAB1    (-4)

/* Window-pixel click -> palette cell index, or one of the PAL_* results above. */
static int palette_pick(int mouse_x, int mouse_y, int w, int h)
{
	double lx, ly;
	win_to_logical(mouse_x, mouse_y, &lx, &ly);

	int x0, y0, x1, y1;
	palette_rect(w, h, &x0, &y0, &x1, &y1);
	if (lx < x0 || lx >= x1 || ly < y0 || ly >= y1) {
		return PAL_OUTSIDE;
	}

	for (int tab = 0; tab < 2; tab++) {
		int tx0, ty0, tx1, ty1;
		palette_tab_rect(tab, w, h, &tx0, &ty0, &tx1, &ty1);
		if (lx >= tx0 && lx < tx1 && ly >= ty0 && ly < ty1) {
			return tab == 0 ? PAL_TAB0 : PAL_TAB1;
		}
	}

	for (int idx = 0; idx < g_pal_count[g_pal_tab]; idx++) {
		int cx0, cy0;
		if (!palette_cell_rect(idx, w, h, &cx0, &cy0)) {
			continue;
		}
		if (lx >= cx0 && lx < cx0 + PAL_CELL - 2 && ly >= cy0 && ly < cy0 + PAL_CELL - 2) {
			return idx;
		}
	}
	return PAL_CHROME;
}

static void palette_scroll(int delta, int w, int h)
{
	int total_rows = (g_pal_count[g_pal_tab] + PAL_COLS - 1) / PAL_COLS;
	int max_row = total_rows - palette_rows_visible(w, h);
	if (max_row < 0) {
		max_row = 0;
	}
	g_pal_scroll[g_pal_tab] += delta;
	if (g_pal_scroll[g_pal_tab] < 0) {
		g_pal_scroll[g_pal_tab] = 0;
	}
	if (g_pal_scroll[g_pal_tab] > max_row) {
		g_pal_scroll[g_pal_tab] = max_row;
	}
}

static void draw_palette(int w, int h, brush_t brush, int loaded_area, const dropdown_t *src_dd, int src_hover)
{
	static const char *tab_label[2] = {"FLOORS", "OBJECTS"};
	int x0, y0, x1, y1;
	char hdr[64];
	palette_rect(w, h, &x0, &y0, &x1, &y1);
	render_rect_alpha(x0, y0, x1, y1, IRGB(2, 2, 5), 245);

	snprintf(hdr, sizeof hdr, "%s %d", tab_label[g_pal_tab], g_pal_count[g_pal_tab]);
	render_text(x0 + PAL_PAD, y0 + 3, IRGB(31, 31, 16), 0, hdr);

	/* Palette-source selector. Brushes borrowed from another zone are shown in
	 * amber: the palette no longer describes the map that is open, which matters
	 * because painting from it introduces sprites the zone has never used. */
	int sx0, sy0, sx1, sy1, sel = -1;
	char src[32];
	palette_src_rect(w, h, &sx0, &sy0, &sx1, &sy1);
	for (int i = 0; i < g_zone_count; i++) {
		if (g_zones[i].area == g_pal_src_area) {
			sel = i;
			break;
		}
	}
	snprintf(src, sizeof src, "zone %d", g_pal_src_area);
	dropdown_draw_button(src_dd, "from", src, sx0, sy0, sx1, sy1, w, h,
	    (g_pal_src_area == loaded_area) ? IRGB(20, 20, 24) : IRGB(31, 24, 8));

	/* tab strip: the active tab is lit, the other one dim */
	for (int tab = 0; tab < 2; tab++) {
		int tx0, ty0, tx1, ty1;
		palette_tab_rect(tab, w, h, &tx0, &ty0, &tx1, &ty1);
		int on = (tab == g_pal_tab);
		render_rect_alpha(tx0 + 1, ty0, tx1 - 1, ty1 - 2, on ? IRGB(10, 10, 18) : IRGB(4, 4, 8), 255);
		render_text(tx0 + 6, ty0 + 3, on ? IRGB(31, 31, 20) : IRGB(16, 16, 18), 0, tab_label[tab]);
	}

	for (int idx = 0; idx < g_pal_count[g_pal_tab]; idx++) {
		int cx0, cy0;
		if (!palette_cell_rect(idx, w, h, &cx0, &cy0)) {
			continue;
		}
		int cx1 = cx0 + PAL_CELL - 2, cy1 = cy0 + PAL_CELL - 2;
		render_rect_alpha(cx0, cy0, cx1, cy1, IRGB(6, 6, 11), 255);

		/* thumbnail centered + clipped to the cell (sprites draw at native size) */
		render_set_clip(cx0, cy0, cx1, cy1);
		unsigned int g = g_pal[g_pal_tab][idx].sprite;
		int mx = (cx0 + cx1) / 2, my = (cy0 + cy1) / 2;
		/* Zoom 100: thumbnails are a fixed-size index of the brushes, so they
		 * must not follow the map's zoom -- the cells they sit in don't. */
		draw_map_sprite(g & 0xFFFF, (size_t)idx, mx, my, RENDER_ALIGN_CENTER, 100);
		draw_map_sprite(g >> 16, (size_t)idx, mx, my, RENDER_ALIGN_CENTER, 100);
		render_set_clip(0, 0, w, h);

		if (g == brush.sprite && g_pal_tab == brush.layer) { /* selected outline */
			unsigned short hi = IRGB(31, 31, 0);
			render_line(cx0, cy0, cx1, cy0, hi);
			render_line(cx1, cy0, cx1, cy1, hi);
			render_line(cx1, cy1, cx0, cy1, hi);
			render_line(cx0, cy1, cx0, cy0, hi);
		}
	}

	/* Last, so an open list sits above the cells it overhangs. */
	dropdown_draw_popup(src_dd, zone_list_label, g_zone_count, sel, src_hover, sx1, sy1, w, h);
	reset_clip();
}

/* Overlay bars: a title strip up top and a status/help strip along the bottom.
 * This is the first on-screen text (real client fonts via render_create_font),
 * the foundation the tile palette + quest forms will build on. */
static void draw_hud(int w, int h, int have_hover, int hx, int hy, brush_t brush, const char *save_path, int locked)
{
	char line[256];

	/* Top title bar. Only the tail of the path is shown: absolute zone paths are
	 * long enough to run under the status banner on the right. */
	render_rect_alpha(0, 0, w, 22, IRGB(0, 0, 0), 160);
	const char *shown = save_path ? save_path : "(no file)";
	if (save_path) {
		const char *slash = NULL, *p;
		int seen = 0;
		for (p = save_path + strlen(save_path); p > save_path; p--) {
			if (p[-1] == '/' || p[-1] == '\\') {
				if (++seen == 2) { /* keep "<zone>/<file>.map" */
					slash = p;
					break;
				}
			}
		}
		if (slash) {
			shown = slash;
		}
	}
	/* Zoom lives up here rather than in the status bar: at the narrow windowed
	 * canvas the bottom bar has no room left beside the help text. */
	snprintf(line, sizeof line, "zoneedit   %s   zoom %d%%", shown, g_zoom);
	render_text(8, 6, IRGB(31, 31, 31), 0, line);
	if (locked) {
		render_text(w - 330, 6, IRGB(31, 20, 8), 0, "READ-ONLY (run: zoneedit --claim=<area> to edit)");
	} else if (g_zone_claimed) {
		render_text(w - 230, 6, IRGB(12, 31, 12), 0, "EDITABLE - your zone, F5 saves");
	} else if (g_live_edit) {
		render_text(w - 300, 6, IRGB(31, 12, 12), 0, "LIVE EDIT - saves overwrite this zone (backed up)");
	}

	/* bottom status + help bar */
	const char *layer = (brush.layer == LAYER_OBJECT) ? "object" : "floor";
	render_rect_alpha(0, h - 22, w, h, IRGB(0, 0, 0), 160);
	if (have_hover) {
		snprintf(line, sizeof line, "tile %d,%d   brush %u (%s)", hx, hy, brush.sprite, layer);
	} else {
		snprintf(line, sizeof line, "brush %u (%s)   (hover a tile)", brush.sprite, layer);
	}
	render_text(8, h - 16, IRGB(24, 31, 24), 0, line);

	/* Help text, right-aligned by measured width rather than a guessed offset --
	 * the canvas ranges from an 800px windowed one to the full display, and a
	 * fixed offset either ran under the status text on the left or floated away
	 * from the right edge. Dropped entirely when it would still collide. */
	const char *help =
	    "drag paint  RMB erase  SHIFT+drag rect  CTRL+click fill  MMB pick  TAB layer  P palette  WHEEL zoom  F11 "
	    "window  F5 save  ESC quit";
	int help_w = render_text_length(0, help);
	int help_x = w - help_w - 8;
	if (help_x > 8 + render_text_length(0, line) + 12) {
		render_text(help_x, h - 16, IRGB(24, 24, 31), 0, help);
	}
}

/* Window layout.
 *
 * The client centers a fixed 800 x YRES canvas inside the window and letterboxes
 * whatever is left over, because its HUD is built to that exact size. For an
 * editor those bars are just wasted desk space, so zoneedit instead claims the
 * entire window: logical size = window pixels / sdl_scale, no centering offset,
 * no height cap. Bigger display simply means more map on screen.
 *
 * VIEW_SCALE (window pixels per logical pixel) is what trades sprite size
 * against map area -- 2 keeps sprites and HUD text comfortably legible while
 * still showing more of the map than the old 800x650 canvas did. It is pinned
 * before sdl_init because it also selects which res/gxN.zip is opened.
 *
 * WIN_W/WIN_H is the bordered-window size used by --windowed and by F11; at
 * VIEW_SCALE 2 it works out to the same 800x650 logical canvas the tool used
 * before, which keeps --shot screenshots comparable with earlier ones. */
#define VIEW_SCALE 2
#define WIN_W      1600
#define WIN_H      1300

static int g_borderless = 1; /* filling the display (F11 toggles) */

/* Re-derive the logical draw area from the current window size. Call after
 * anything that can change that size: startup, F11, a user resize. */
static void apply_layout(int *out_w, int *out_h)
{
	int px = 0, py = 0;

	SDL_GetWindowSize(sdlwnd, &px, &py);
	if (px <= 0 || py <= 0) { /* shouldn't happen; keep a sane canvas if it does */
		px = WIN_W;
		py = WIN_H;
	}

	g_view_w = px / sdl_scale;
	g_view_h = py / sdl_scale;

	__yres = g_view_h; /* lifts the client's YRES1 (650) height cap */
	render_set_offset(0, 0); /* no letterbox: the window IS the canvas   */
	reset_clip();

	*out_w = g_view_w;
	*out_h = g_view_h;
}

int main(int argc, char *argv[])
{
	int area = 1, max_frames = 0, i;
	const char *zones_root = "../astonia_community_server3/zones";
	const char *shot_path = NULL;
	const char *out_path = NULL; /* --out: write the (edited) map here     */
	const char *mapfile_override = NULL; /* --mapfile: load this .map directly  */
	int hl_x = -1, hl_y = -1; /* --highlight tile                        */
	int window_mode_set = 0; /* --windowed/--borderless given explicitly */
	int palette_from = 0; /* --palette-from: borrow brushes from this zone */
	int selftest = 0; /* --selftest: check the mouse->tile inverse and exit */

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
	int claim_area = 0, unclaim_area = 0;
	const char *area_name = NULL;

	/* Scripted rectangle fills (--fill), the batch form of shift+drag. */
	int fill_x0[16], fill_y0[16], fill_x1[16], fill_y1[16];
	unsigned int fill_g[16];
	int nfill = 0;

	/* Scripted flood fills (--bucket), the batch form of ctrl+click. */
	int bucket_x[16], bucket_y[16];
	unsigned int bucket_g[16];
	int nbucket = 0;

	/* Scripted warps: place a teleport door at (sx,sy) -> area (dx,dy). */
	int warp_sx[64], warp_sy[64], warp_a[64], warp_dx[64], warp_dy[64];
	unsigned int warp_sp[64];
	int nwarp = 0;

	/* Two-way links: same as a warp, plus the matching return door written into
	 * the destination zone, so one command connects two maps in both directions. */
	int link_sx[16], link_sy[16], link_a[16], link_dx[16], link_dy[16];
	unsigned int link_sp[16];
	int nlink = 0;

	for (i = 1; i < argc; i++) {
		if (!strncmp(argv[i], "--zones=", 8)) {
			zones_root = argv[i] + 8;
		} else if (!strncmp(argv[i], "--mapfile=", 10)) {
			mapfile_override = argv[i] + 10;
		} else if (!strncmp(argv[i], "--frames=", 9)) {
			max_frames = atoi(argv[i] + 9);
		} else if (!strncmp(argv[i], "--shot=", 7)) {
			shot_path = argv[i] + 7;
		} else if (!strncmp(argv[i], "--zoom=", 7)) {
			int z = atoi(argv[i] + 7), n = (int)(sizeof zoom_steps / sizeof zoom_steps[0]), j;
			for (j = 0; j < n && zoom_steps[j] != z; j++) {
				;
			}
			if (j < n) {
				g_zoom = z;
			} else {
				fprintf(stderr, "zoneedit: --zoom must be 50, 100, 150 or 200 (got %d), keeping %d\n", z, g_zoom);
			}
		} else if (!strcmp(argv[i], "--windowed")) {
			g_borderless = 0;
			window_mode_set = 1;
		} else if (!strcmp(argv[i], "--borderless")) {
			g_borderless = 1;
			window_mode_set = 1;
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
		} else if (!strncmp(argv[i], "--link=", 7)) {
			unsigned int sp = 11061;
			int got = sscanf(argv[i] + 7, "%d,%d,%d,%d,%d,%u", &link_sx[nlink], &link_sy[nlink], &link_a[nlink],
			    &link_dx[nlink], &link_dy[nlink], &sp);
			if (nlink < 16 && got >= 5) {
				link_sp[nlink] = sp;
				nlink++;
			} else {
				fprintf(stderr, "zoneedit: bad --link (want sx,sy,destarea,dx,dy[,sprite])\n");
			}
		} else if (!strncmp(argv[i], "--fill=", 7)) {
			/* Batch rectangle fill -- the scripted form of shift+drag, and the
			 * quickest way to enlarge a map's painted area. */
			if (nfill < 16 && sscanf(argv[i] + 7, "%d,%d,%d,%d,%u", &fill_x0[nfill], &fill_y0[nfill], &fill_x1[nfill],
			                      &fill_y1[nfill], &fill_g[nfill]) == 5) {
				nfill++;
			} else {
				fprintf(stderr, "zoneedit: bad --fill (want x0,y0,x1,y1,gsprite)\n");
			}
		} else if (!strncmp(argv[i], "--bucket=", 9)) {
			/* Batch flood fill -- the scripted form of ctrl+click. */
			if (nbucket < 16 &&
			    sscanf(argv[i] + 9, "%d,%d,%u", &bucket_x[nbucket], &bucket_y[nbucket], &bucket_g[nbucket]) == 3) {
				nbucket++;
			} else {
				fprintf(stderr, "zoneedit: bad --bucket (want x,y,gsprite)\n");
			}
		} else if (!strncmp(argv[i], "--palette-from=", 15)) {
			palette_from = atoi(argv[i] + 15);
		} else if (!strncmp(argv[i], "--claim=", 8)) {
			claim_area = atoi(argv[i] + 8);
		} else if (!strncmp(argv[i], "--unclaim=", 10)) {
			unclaim_area = atoi(argv[i] + 10);
		} else if (!strncmp(argv[i], "--new-area=", 11)) {
			new_area = atoi(argv[i] + 11);
		} else if (!strncmp(argv[i], "--area-name=", 12)) {
			area_name = argv[i] + 12;
		} else if (!strcmp(argv[i], "--selftest")) {
			selftest = 1;
		} else if (!strcmp(argv[i], "--show-flags")) {
			g_show_flags = 1;
		} else if (!strcmp(argv[i], "--allow-live")) {
			g_live_edit = 1;
		} else if (argv[i][0] != '-') {
			area = atoi(argv[i]);
		}
	}

	/* Bookkeeping commands: do the thing and exit (no rendering needed). */
	if (claim_area > 0) {
		return zone_claim(zones_root, claim_area, 1);
	}
	if (unclaim_area > 0) {
		return zone_claim(zones_root, unclaim_area, 0);
	}

	/* New-area wizard: scaffold zones/<N>/ and emit the mandatory area-row SQL. */
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
	g_char_here = calloc((size_t)MAXMAP * MAXMAP, sizeof(unsigned char));
	if (!g_place_sprite || !g_char_here) {
		fprintf(stderr, "zoneedit: out of memory\n");
		return 1;
	}
	int resolved = 0;
	for (i = 0; i < g_map.place_count; i++) {
		size_t pc = cellof(g_map.place[i].x, g_map.place[i].y);
		if (g_map.place[i].is_char) {
			g_char_here[pc] = 1;
		}
		unsigned int sp = tmpl_lookup(g_map.place[i].name);
		if (sp) {
			g_place_sprite[pc] = sp;
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

	/* Where F5 / batch edits save, and whether that target is write-protected.
	 * By default anything that already exists (every live zone) is locked, so a
	 * live map can never be clobbered by accident and only a brand-new --out
	 * path is writable. --allow-live opts into editing a zone in place, which is
	 * what linking a door into shipped content (e.g. a door in Aston) requires;
	 * every such overwrite is preceded by a one-time backup. Computed before the
	 * warp pass below so a door is never half-applied -- template written to
	 * disk but placement dropped by a blocked save. */
	const char *save_path = out_path ? out_path : mapfile;
	g_zone_claimed = zone_is_editable(zones_root, area);
	int save_locked = file_exists(save_path) && !g_live_edit && !g_zone_claimed;
	if (save_locked) {
		fprintf(stderr,
		    "zoneedit: %s already exists -> READ-ONLY (saving disabled).\n"
		    "  This zone is not claimed for editing. To make area %d yours to edit:  zoneedit --claim=%d\n"
		    "  (or --allow-live for a one-off in-place edit of a shipped zone.)\n",
		    save_path, area, area);
	} else if (g_zone_claimed) {
		fprintf(stderr, "zoneedit: area %d is claimed for editing - F5 saves in place (backup written first)\n", area);
	} else if (g_live_edit && file_exists(save_path)) {
		fprintf(stderr, "zoneedit: LIVE EDIT enabled - saves overwrite %s (a backup is written first)\n", save_path);
	}

	/* Scripted rectangle fills. Runs before placements so a fill can lay down
	 * the ground a later --place or --link needs. */
	for (i = 0; i < nfill; i++) {
		brush_t fb = {fill_g[i], 0, LAYER_FLOOR};
		fill_rect(fill_x0[i], fill_y0[i], fill_x1[i], fill_y1[i], fb, 0);
		fprintf(stderr, "zoneedit: filled (%d,%d)-(%d,%d) with gsprite %u\n", fill_x0[i], fill_y0[i], fill_x1[i],
		    fill_y1[i], fill_g[i]);
	}

	for (i = 0; i < nbucket; i++) {
		brush_t bb = {bucket_g[i], 0, LAYER_FLOOR};
		flood_fill(bucket_x[i], bucket_y[i], bb, 0);
		fprintf(
		    stderr, "zoneedit: bucket-filled from (%d,%d) with gsprite %u\n", bucket_x[i], bucket_y[i], bucket_g[i]);
	}

	/* Generate + place scripted warp doors (writes the zone's warps.itm). */
	for (i = 0; i < nwarp; i++) {
		add_warp(zonedir, warp_sx[i], warp_sy[i], warp_a[i], warp_dx[i], warp_dy[i], warp_sp[i], !save_locked);
	}

	/* Two-way links: the outbound door here, plus the return door over in the
	 * destination zone. Both halves are written, or neither. */
	for (i = 0; i < nlink; i++) {
		if (save_locked) {
			fprintf(stderr, "zoneedit: --link needs a writable map (--allow-live or --out); skipped\n");
			break;
		}
		if (link_a[i] == area) {
			fprintf(stderr, "zoneedit: --link destination area %d is the loaded area; use --warp instead\n", link_a[i]);
			continue;
		}
		if (add_return_warp(zones_root, link_a[i], link_dx[i], link_dy[i], area, link_sx[i], link_sy[i], link_sp[i]) !=
		    0) {
			fprintf(stderr, "zoneedit: link aborted - the outbound door was NOT written\n");
			continue;
		}
		add_warp(zonedir, link_sx[i], link_sy[i], link_a[i], link_dx[i], link_dy[i], link_sp[i], 1);
	}

	/* Save the (edited) map if requested (blocked when the target is locked).
	 * A claimed zone also saves its scripted edits in place -- otherwise the only
	 * batch route would be --out, which must never point inside a zone folder. */
	int batch_edits = nset + nflag + nplace + nwarp + nlink + nfill + nbucket;
	if ((out_path || nlink || (g_zone_claimed && batch_edits)) && !save_locked) {
		if (save_map(&g_map, save_path) == 0) {
			fprintf(stderr, "zoneedit: wrote map %s\n", save_path);
		} else {
			fprintf(stderr, "zoneedit: FAILED to write map %s\n", save_path);
		}
	}

	/* A headless screenshot run wants a fixed, reproducible canvas rather than
	 * whatever this machine's display happens to be, so --shot implies --windowed
	 * unless the window mode was asked for explicitly. */
	if (shot_path && !window_mode_set) {
		g_borderless = 0;
	}

	sdl_multi = 0;
	sdl_cache_size = 8000;
	sdl_scale_pin = VIEW_SCALE; /* must precede sdl_init: it picks res/gxN.zip */

	/* Always create the window at the bordered size, then go fullscreen-desktop
	 * separately. Letting sdl_init do the fullscreen (it does that when asked for
	 * the display size) would leave SDL with no smaller size to restore on F11. */
	if (!sdl_init(WIN_W, WIN_H, "Astonia zoneedit", 0)) {
		fprintf(stderr, "zoneedit: sdl_init failed\n");
		return 1;
	}
	if (g_borderless) {
		SDL_SetWindowFullscreenMode(sdlwnd, NULL); /* NULL = borderless desktop */
		SDL_SetWindowFullscreen(sdlwnd, true);
		SDL_SyncWindow(sdlwnd);
	}

	int w, h;
	apply_layout(&w, &h);
	render_create_font(); /* real client fonts (font.c) for the HUD/panels */

	/* Loads the res/config JSON tables, including the animated-variant table that
	 * maps virtual map sprite ids onto real archive sprites (see draw_map_sprite). */
	sprite_config_init();

	/* Start on the highlighted tile when one was given -- asking to highlight a
	 * tile means you want to look at it, and on a full-size zone the content
	 * centroid can be far enough away that the highlight lands off-screen. */
	double camx, camy;
	if (hl_x >= 0 && hl_y >= 0) {
		camx = hl_x;
		camy = hl_y;
	} else {
		content_center(&camx, &camy);
	}
	fprintf(stderr, "zoneedit: camera at %.0f,%.0f, view %dx%d\n", camx, camy, w, h);

	if (selftest) {
		int fails = selftest_pick(camx, camy, w, h);
		sdl_exit();
		return fails ? 1 : 0;
	}

	/* Brushes come from the loaded map by default; --palette-from borrows another
	 * zone's, which is what makes a freshly created area paintable at all. */
	zone_list_build(zones_root);
	palette_build_from(&g_map);
	g_pal_src_area = area;
	fprintf(stderr, "zoneedit: %d zones available; palette has %d floor and %d object brushes\n", g_zone_count,
	    g_pal_count[LAYER_FLOOR], g_pal_count[LAYER_OBJECT]);
	if (palette_from > 0) {
		palette_borrow(zones_root, palette_from, area);
	}

	dropdown_t src_dd = {0, 0}; /* the palette-source selector */
	int src_hover = -1;

	/* Start on whatever the camera is sitting on. */
	brush_t brush = {g_map.gsprite[cellof((int)camx, (int)camy)], 0, LAYER_FLOOR};

	/* Paint-tool state: which button is held for a drag stroke, and the anchor
	 * corner of an in-progress shift+drag rectangle. */
	int drag_button = 0;
	int rect_active = 0, rect_x0 = 0, rect_y0 = 0, rect_btn = 0;
	int running = 1, frame = 0;
	const double pan = 1.0; /* tiles per frame while a pan key is held */
	while (running) {
		float mfx = 0, mfy = 0;
		SDL_GetMouseState(&mfx, &mfy);
		int hx, hy, have_hover = pick_tile((int)mfx, (int)mfy, camx, camy, w, h, &hx, &hy);

		/* The palette-source selector claims the pointer before anything else, so
		 * an open list is never painted through. Its rect is recomputed each frame
		 * because the canvas can be resized (F11) underneath it. */
		int sx0, sy0, sx1, sy1;
		palette_src_rect(w, h, &sx0, &sy0, &sx1, &sy1);
		int dd_hit = dropdown_pick(&src_dd, (int)mfx, (int)mfy, g_zone_count, sx0, sy0, sx1, sy1, h, &src_hover);

		SDL_Event e;
		while (SDL_PollEvent(&e)) {
			if (e.type == SDL_EVENT_QUIT) {
				running = 0;
			} else if (e.type == SDL_EVENT_KEY_DOWN) {
				if (e.key.key == SDLK_ESCAPE) {
					if (src_dd.open) {
						src_dd.open = 0; /* first ESC dismisses the list, not the tool */
					} else {
						running = 0;
					}
				} else if (e.key.key == SDLK_P) {
					src_dd.open = !src_dd.open;
				} else if (e.key.key == SDLK_TAB) {
					g_pal_tab = (g_pal_tab == LAYER_FLOOR) ? LAYER_OBJECT : LAYER_FLOOR;
				} else if (e.key.key == SDLK_F5) {
					if (save_locked) {
						fprintf(stderr,
						    "zoneedit: save blocked - this zone is not claimed for editing. Run `zoneedit --claim=%d` "
						    "to make it yours, or relaunch with --allow-live for a one-off in-place edit.\n",
						    area);
					} else if (save_map(&g_map, save_path) == 0) {
						fprintf(stderr, "zoneedit: saved %s\n", save_path);
					}
				} else if (e.key.key == SDLK_F11) {
					g_borderless = !g_borderless;
					SDL_SetWindowFullscreen(sdlwnd, g_borderless);
					SDL_SyncWindow(sdlwnd);
					apply_layout(&w, &h);
				} else if (e.key.key == SDLK_EQUALS || e.key.key == SDLK_KP_PLUS) {
					zoom_step(1); /* keyboard zoom holds the center, not the cursor */
				} else if (e.key.key == SDLK_MINUS || e.key.key == SDLK_KP_MINUS) {
					zoom_step(-1);
				}
			} else if (e.type == SDL_EVENT_WINDOW_RESIZED || e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
				apply_layout(&w, &h);
			} else if (e.type == SDL_EVENT_MOUSE_WHEEL) {
				/* Over an open zone list the wheel scrolls that; over the palette
				 * it scrolls the brushes; over the map it zooms. palette_pick
				 * returns -2 for "outside the panel". */
				if (dd_hit != DD_NONE && dd_hit != DD_BUTTON) {
					dropdown_scroll(&src_dd, -(int)e.wheel.y, g_zone_count, sy1, h);
				} else if (palette_pick((int)mfx, (int)mfy, w, h) != -2) {
					palette_scroll(-(int)e.wheel.y, w, h);
				} else {
					/* Zoom about the cursor: find the map position under the
					 * pointer, change zoom, then shift the camera by however much
					 * that same pixel now resolves to, so the tile you are pointing
					 * at stays put instead of drifting toward the center. */
					double ax, ay, bx, by;
					pick_tile_frac((int)mfx, (int)mfy, camx, camy, w, h, &ax, &ay);
					zoom_step((int)e.wheel.y);
					pick_tile_frac((int)mfx, (int)mfy, camx, camy, w, h, &bx, &by);

					/* Snap back to whole tiles: draw_zone works off an integer
					 * camera, so a fractional one would just be truncated away. */
					camx = floor(camx + (ax - bx) + 0.5);
					camy = floor(camy + (ay - by) + 0.5);
				}
			} else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
				int dk =
				    dropdown_pick(&src_dd, (int)e.button.x, (int)e.button.y, g_zone_count, sx0, sy0, sx1, sy1, h, NULL);
				int pk = palette_pick((int)e.button.x, (int)e.button.y, w, h);

				if (dk != DD_NONE) {
					/* The selector owns this click: open/close it, or borrow the
					 * chosen zone's brushes. Edits keep targeting the loaded map. */
					if (e.button.button == SDL_BUTTON_LEFT) {
						if (dk == DD_BUTTON) {
							src_dd.open = !src_dd.open;
						} else if (dk >= 0) {
							palette_borrow(zones_root, g_zones[dk].area, area);
							src_dd.open = 0;
						}
					}
				} else if (src_dd.open) {
					/* Click away closes the list and is swallowed, so dismissing it
					 * cannot paint the tile that happened to be underneath. */
					src_dd.open = 0;
				} else if (pk == PAL_TAB0 || pk == PAL_TAB1) {
					if (e.button.button == SDL_BUTTON_LEFT) {
						g_pal_tab = (pk == PAL_TAB0) ? LAYER_FLOOR : LAYER_OBJECT;
					}
				} else if (pk >= 0) {
					if (e.button.button == SDL_BUTTON_LEFT) { /* pick brush from palette */
						brush.sprite = g_pal[g_pal_tab][pk].sprite;
						brush.flags = g_pal[g_pal_tab][pk].flags;
						brush.layer = g_pal_tab;
					}
				} else if (pk == PAL_OUTSIDE && have_hover) { /* over the map, not the panel */
					size_t c = cellof(hx, hy);
					SDL_Keymod mod = SDL_GetModState();
					int is_paint = (e.button.button == SDL_BUTTON_LEFT);
					int is_erase = (e.button.button == SDL_BUTTON_RIGHT);

					if ((mod & SDL_KMOD_CTRL) && (is_paint || is_erase)) {
						/* Ctrl+click = bucket: recolor the connected region. */
						flood_fill(hx, hy, brush, is_erase);
					} else if ((mod & SDL_KMOD_SHIFT) && (is_paint || is_erase)) {
						/* Shift+drag = rectangle; anchor here, fill on release. */
						rect_active = 1;
						rect_x0 = hx;
						rect_y0 = hy;
						rect_btn = e.button.button;
					} else if (is_paint) {
						paint_cell(c, brush);
						drag_button = SDL_BUTTON_LEFT; /* keep painting while held */
					} else if (is_erase) {
						erase_cell(c, brush);
						drag_button = SDL_BUTTON_RIGHT;
					} else if (e.button.button == SDL_BUTTON_MIDDLE) { /* eyedropper */
						if (g_map.fsprite[c] & 0xFFFF) {
							brush.sprite = g_map.fsprite[c];
							brush.flags = g_map.flags[c];
							brush.layer = LAYER_OBJECT;
						} else {
							brush.sprite = g_map.gsprite[c];
							brush.flags = 0;
							brush.layer = LAYER_FLOOR;
						}
						g_pal_tab = brush.layer;
					}
				}
			} else if (e.type == SDL_EVENT_MOUSE_MOTION) {
				/* Drag-paint: a stroke is just the click op repeated over each
				 * tile the cursor crosses. Re-check the palette so dragging onto
				 * the panel does not paint underneath it. */
				if (drag_button && have_hover && !src_dd.open &&
				    palette_pick((int)e.motion.x, (int)e.motion.y, w, h) == PAL_OUTSIDE) {
					if (drag_button == SDL_BUTTON_LEFT) {
						paint_cell(cellof(hx, hy), brush);
					} else {
						erase_cell(cellof(hx, hy), brush);
					}
				}
			} else if (e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
				if (rect_active && e.button.button == rect_btn) {
					if (have_hover) {
						fill_rect(rect_x0, rect_y0, hx, hy, brush, rect_btn == SDL_BUTTON_RIGHT);
					}
					rect_active = 0;
				}
				if (e.button.button == drag_button) {
					drag_button = 0;
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

		/* The client's world runs at 24Hz; match it so animated tiles cycle at
		 * the same speed here as they do in game. */
		g_anim_tick = (tick_t)(SDL_GetTicks() * 24 / 1000);

		sdl_clear();
		reset_clip();
		draw_zone(camx, camy, w, h);
		if (have_hover) {
			draw_highlight(hx, hy, camx, camy, w, h);
		}
		if (hl_x >= 0 && hl_y >= 0) {
			draw_highlight(hl_x, hl_y, camx, camy, w, h);
		}
		if (rect_active && have_hover) {
			draw_rect_outline(rect_x0, rect_y0, hx, hy, camx, camy, w, h);
		}
		draw_hud(w, h, have_hover, hx, hy, brush, save_path, save_locked);
		draw_palette(w, h, brush, area, &src_dd, src_hover);

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
