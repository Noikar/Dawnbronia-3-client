/*
 * Part of Astonia content tooling (Dawnbronia). Please read license.txt.
 *
 * Minimal game-glue stubs so the reused client sprite/render layer
 * (the src/sdl layer + src/game/render.c) links into the standalone zoneedit
 * binary without dragging in the whole client (GUI, network, options, main loop).
 *
 * Modeled on tests/test_stubs.c, but this build uses the REAL SDL renderer, so
 * the SDL_* GPU stubs from the test file are intentionally absent.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_keycode.h>

/* --- logging (signatures match the client's internal log helpers) --------- */

void note(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
	fprintf(stderr, "\n");
}

char *fail(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	fprintf(stderr, "FAIL: ");
	vfprintf(stderr, format, args);
	va_end(args);
	fprintf(stderr, "\n");
	return "failure";
}

void warn(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	fprintf(stderr, "WARN: ");
	vfprintf(stderr, format, args);
	va_end(args);
	fprintf(stderr, "\n");
}

void paranoia(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	fprintf(stderr, "PARANOIA: ");
	vfprintf(stderr, format, args);
	va_end(args);
	fprintf(stderr, "\n");
	abort();
}

/* --- game state the sprite/render layer references ------------------------ */

int quit = 0;
uint64_t game_options = 0; /* GO_SOUND unset -> sdl_init skips audio */
char *localdata = NULL;
int frames_per_second = 24;
int frames_per_second_auto = 1;
int xmemcheck_failed = 0;

/* --- input callbacks invoked by sdl_loop() (we drive our own loop) -------- */

void gui_sdl_mouseproc(float x __attribute__((unused)), float y __attribute__((unused)), int b __attribute__((unused)))
{
}

void gui_sdl_keyproc(SDL_Keycode key __attribute__((unused))) {}

void context_keyup(SDL_Keycode key __attribute__((unused))) {}

void cmd_proc(int key __attribute__((unused))) {}

void display_messagebox(const char *title, const char *msg)
{
	fprintf(stderr, "MessageBox: %s - %s\n", title ? title : "(no title)", msg ? msg : "(no message)");
}

/* --- misc helpers the sprite layer expects -------------------------------- */

int rrand(int min, int max)
{
	if (max <= min) {
		return min;
	}
	return min + (rand() % (max - min + 1));
}

int sprite_config_do_smoothify(unsigned int sprite __attribute__((unused)))
{
	return -1;
}

int sprite_config_drop_alpha(unsigned int sprite __attribute__((unused)))
{
	return 0;
}

/* --- text/GUI symbols render.c references -------------------------------- *
 * The base fonts (fonta/fontb/fontc) are now provided by the real src/game/font.c
 * (compiled-in glyph data), so zoneedit can render text via render_create_font()
 * + render_text(). Only the chat-window globals below still need stubbing, since
 * zoneedit skips render_init_text() (the chat subsystem). */

int __textdisplay_sy = 0;
unsigned int tick = 0;

int dotx(int didx __attribute__((unused)))
{
	return 0;
}

int doty(int didx __attribute__((unused)))
{
	return 0;
}
