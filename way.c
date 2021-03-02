#define _POSIX_C_SOURCE 200112L
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-protocol.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#define FONTFILE "/usr/share/fonts/OTF/FantasqueSansMono-Regular.otf"
#define PTSIZE 50
#define TEXTHEIGHT 60
#define TEXT "hello!"
#define WIDTH 640
#define HEIGHT 480
#define BASE 200
#define LEFT 10

// Text color
#define RED 200
#define GRN 240
#define BLU 120

/* Shared memory support code */
static void
randname(char *buf)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	long r = ts.tv_nsec;
	for (int i = 0; i < 6; ++i) {
		buf[i] = 'A' + (r & 15) + (r & 16) * 2;
		r >>= 5;
	}
}

static int
create_shm_file(void)
{
	int retries = 100;
	do {
		char name[] = "/wl_shm-XXXXXX";
		randname(name + sizeof(name) - 7);
		--retries;
		int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
		if (fd >= 0) {
			shm_unlink(name);
			return fd;
		}
	} while (retries > 0 && errno == EEXIST);
	return -1;
}

static int
allocate_shm_file(size_t size)
{
	int fd = create_shm_file();
	if (fd < 0)
		return -1;
	int ret;
	do {
		ret = ftruncate(fd, size);
	} while (ret < 0 && errno == EINTR);
	if (ret < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

/* Wayland code */
struct client_state {
	/* Globals */
	struct wl_display *wl_display;
	struct wl_registry *wl_registry;
	struct wl_shm *wl_shm;
	struct wl_compositor *wl_compositor;
	struct xdg_wm_base *xdg_wm_base;
	/* Objects */
	struct wl_surface *wl_surface;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *xdg_toplevel;
};

static void
wl_buffer_release(void *data, struct wl_buffer *wl_buffer)
{
	/* Sent by the compositor when it's no longer using this buffer */
	wl_buffer_destroy(wl_buffer);
}

static const struct wl_buffer_listener wl_buffer_listener = {
	.release = wl_buffer_release,
};

void
draw_bitmap(uint32_t buffer[][WIDTH], FT_Bitmap * bitmap, FT_Int x, FT_Int y)
{
	FT_Int i, j, p, q;
	FT_Int x_max = x + bitmap->width;
	FT_Int y_max = y + bitmap->rows;


	/* for simplicity, we assume that `bitmap->pixel_mode' */
	/* is `FT_PIXEL_MODE_GRAY' (i.e., not a bitmap font)   */

	for (i = x, p = 0; i < x_max; i++, p++) {
		for (j = y, q = 0; j < y_max; j++, q++) {
			if (i < 0 || j < 0 || i >= WIDTH || j >= HEIGHT)
				continue;

			uint32_t gray = bitmap->buffer[q * bitmap->width + p];
			uint32_t red = (gray * RED + 128) / 256;
			uint32_t grn = (gray * GRN + 128) / 256;
			uint32_t blu = (gray * BLU + 128) / 256;
			buffer[j][i] = red << 16 | grn << 8 | blu;
		}
	}
}

static struct wl_buffer *
draw_frame(struct client_state *state)
{
	int stride = WIDTH * 4;
	int size = stride * HEIGHT;

	int fd = allocate_shm_file(size);
	if (fd == -1) {
		return NULL;
	}

	uint32_t *data = mmap(NULL, size,
			PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		close(fd);
		return NULL;
	}

	struct wl_shm_pool *pool = wl_shm_create_pool(state->wl_shm, fd, size);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0,
			WIDTH, HEIGHT, stride, WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);

	FT_Library library;
	FT_Face face;

	FT_GlyphSlot slot;
	FT_Vector pen;		/* untransformed origin  */
	FT_Error error;

	char *filename;
	char *text;

	int points;
	int target_height;
	int n, num_chars;


	filename = FONTFILE;
	text = TEXT;
	points = PTSIZE;
	num_chars = strlen(text);
	target_height = TEXTHEIGHT;

	error = FT_Init_FreeType(&library);	/* initialize library */
	/* error handling omitted */

	error = FT_New_Face(library, filename, 0, &face);	/* create face object */
	/* error handling omitted */

	/* use 50pt at 100dpi */
	error = FT_Set_Char_Size(face, points * 64, 0, 100, 0);	/* set character size */
	/* error handling omitted */

	/* cmap selection omitted;                                        */
	/* for simplicity we assume that the font contains a Unicode cmap */

	slot = face->glyph;

	/* the pen position in 26.6 cartesian space coordinates; */
	/* start at (300,200) relative to the upper left corner  */
	pen.x = LEFT * 64;
	pen.y = 0; //(target_height - BASE) * 64;

	for (n = 0; n < num_chars; n++) {
		/* set transformation */
		FT_Set_Transform(face, NULL, &pen);

		/* load glyph image into the slot (erase previous one) */
		error = FT_Load_Char(face, text[n], FT_LOAD_RENDER);
		if (error)
			continue;	/* ignore errors */

		/* now, draw to our target surface (convert position) */
		draw_bitmap(data, &slot->bitmap,
				slot->bitmap_left,
				target_height - slot->bitmap_top);

		/* increment pen position */
		pen.x += slot->advance.x;
		pen.y += slot->advance.y;
	}

	FT_Done_Face(face);
	FT_Done_FreeType(library);


/* 	/1* Draw checkerboxed background *1/ */
/* 	for (int y = 0; y < HEIGHT; ++y) { */
/* 		for (int x = 0; x < WIDTH; ++x) { */
/* 			if ((x + y / 8 * 8) % 16 < 8) */
/* 				data[y * WIDTH + x] = 0xFF666666; */
/* 			else */
/* 				data[y * WIDTH + x] = 0xFFEEEEEE; */
/* 		} */
/* 	} */

	munmap(data, size);
	wl_buffer_add_listener(buffer, &wl_buffer_listener, NULL);
	return buffer;
}

static void
xdg_surface_configure(void *data,
		struct xdg_surface *xdg_surface, uint32_t serial)
{
	struct client_state *state = data;
	xdg_surface_ack_configure(xdg_surface, serial);

	struct wl_buffer *buffer = draw_frame(state);
	wl_surface_attach(state->wl_surface, buffer, 0, 0);
	wl_surface_commit(state->wl_surface);
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure,
};

static void
xdg_wm_base_ping(void *data, struct xdg_wm_base *xdg_wm_base, uint32_t serial)
{
	xdg_wm_base_pong(xdg_wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
	.ping = xdg_wm_base_ping,
};

static void
registry_global(void *data, struct wl_registry *wl_registry,
		uint32_t name, const char *interface, uint32_t version)
{
	struct client_state *state = data;
	if (strcmp(interface, wl_shm_interface.name) == 0) {
		state->wl_shm = wl_registry_bind(wl_registry, name,
				&wl_shm_interface, 1);
	} else if (strcmp(interface, wl_compositor_interface.name) == 0) {
		state->wl_compositor =
				wl_registry_bind(wl_registry, name,
				&wl_compositor_interface, 4);
	} else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		state->xdg_wm_base =
				wl_registry_bind(wl_registry, name,
				&xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(state->xdg_wm_base,
				&xdg_wm_base_listener, state);
	}
}

static void
registry_global_remove(void *data,
		struct wl_registry *wl_registry, uint32_t name)
{
	/* This space deliberately left blank */
}

static const struct wl_registry_listener wl_registry_listener = {
.global = registry_global,.global_remove = registry_global_remove,};

int
main(int argc, char *argv[])
{
	struct client_state state = { 0 };
	state.wl_display = wl_display_connect(NULL);
	state.wl_registry = wl_display_get_registry(state.wl_display);
	wl_registry_add_listener(state.wl_registry, &wl_registry_listener,
			&state);
	wl_display_roundtrip(state.wl_display);

	state.wl_surface = wl_compositor_create_surface(state.wl_compositor);
	state.xdg_surface =
			xdg_wm_base_get_xdg_surface(state.xdg_wm_base,
			state.wl_surface);
	xdg_surface_add_listener(state.xdg_surface, &xdg_surface_listener,
			&state);
	state.xdg_toplevel = xdg_surface_get_toplevel(state.xdg_surface);
	xdg_toplevel_set_title(state.xdg_toplevel, "Example client");
	wl_surface_commit(state.wl_surface);

	while (wl_display_dispatch(state.wl_display)) {
		/* This space deliberately left blank */
	}

	return 0;
}
