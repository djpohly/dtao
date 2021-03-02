#define _POSIX_C_SOURCE 200112L
#include <linux/input-event-codes.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <GLES2/gl2.h>
#include <limits.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "wlr-layer-shell-unstable-v1-protocol.h"
#include "xdg-shell-protocol.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#define FONTFILE "/usr/share/fonts/OTF/FantasqueSansMono-Regular.otf"
#define PTSIZE 18
#define TEXT "hello world"
#define TEXTHEIGHT 24
#define BASE 200
#define LEFT 10

// Text color
#define RED 200
#define GRN 240
#define BLU 120

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_pointer *pointer;
static struct wl_keyboard *keyboard;
static struct xdg_wm_base *xdg_wm_base;
static struct zwlr_layer_shell_v1 *layer_shell;

struct zwlr_layer_surface_v1 *layer_surface;
static struct wl_output *wl_output;

struct wl_surface *wl_surface;
struct wl_callback *frame_callback;

static uint32_t output = UINT32_MAX;

static uint32_t layer = ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND;
static uint32_t anchor = 0;
static uint32_t width = 0, height = 0;
static bool run_display = true;
static double frame = 0;
static int cur_x = -1, cur_y = -1;
static int buttons = 0;

static struct {
	struct timespec last_frame;
	float color[3];
	int dec;
} demo;

static void
wl_buffer_release(void *data, struct wl_buffer *wl_buffer)
{
	/* Sent by the compositor when it's no longer using this buffer */
	wl_buffer_destroy(wl_buffer);
}

static const struct wl_buffer_listener wl_buffer_listener = {
	.release = wl_buffer_release,
};

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

void
draw_bitmap(uint32_t buffer[][width], FT_Bitmap * bitmap, FT_Int x, FT_Int y)
{
	FT_Int i, j, p, q;
	FT_Int x_max = x + bitmap->width;
	FT_Int y_max = y + bitmap->rows;


	/* for simplicity, we assume that `bitmap->pixel_mode' */
	/* is `FT_PIXEL_MODE_GRAY' (i.e., not a bitmap font)   */

	for (i = x, p = 0; i < x_max; i++, p++) {
		for (j = y, q = 0; j < y_max; j++, q++) {
			if (i < 0 || j < 0 || i >= width || j >= height)
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
draw_frame(void)
{
	int stride = width * 4;
	int size = stride * height;

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

	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0,
			width, height, stride, WL_SHM_FORMAT_XRGB8888);
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
/* 	for (int y = 0; y < height; ++y) { */
/* 		for (int x = 0; x < width; ++x) { */
/* 			if ((x + y / 8 * 8) % 16 < 8) */
/* 				data[y * width + x] = 0xFF666666; */
/* 			else */
/* 				data[y * width + x] = 0xFFEEEEEE; */
/* 		} */
/* 	} */

	munmap(data, size);
	wl_buffer_add_listener(buffer, &wl_buffer_listener, NULL);
	return buffer;
}

static void
xdg_surface_handle_configure(void *data,
		struct xdg_surface *xdg_surface, uint32_t serial)
{
	xdg_surface_ack_configure(xdg_surface, serial);
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_handle_configure,
};

static void
layer_surface_configure(void *data,
		struct zwlr_layer_surface_v1 *surface,
		uint32_t serial, uint32_t w, uint32_t h)
{
	width = w;
	height = h;
	zwlr_layer_surface_v1_ack_configure(surface, serial);

	struct wl_buffer *buffer = draw_frame();
	wl_surface_attach(wl_surface, buffer, 0, 0);
	wl_surface_commit(wl_surface);
}

static void
layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *surface)
{
	zwlr_layer_surface_v1_destroy(surface);
	wl_surface_destroy(wl_surface);
	run_display = false;
}

struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = layer_surface_configure,
	.closed = layer_surface_closed,
};

static void
handle_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version)
{
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		compositor = wl_registry_bind(registry, name,
				&wl_compositor_interface, 1);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, "wl_output") == 0) {
		if (output != UINT32_MAX) {
			if (!wl_output) {
				wl_output = wl_registry_bind(registry, name,
						&wl_output_interface, 1);
			} else {
				output--;
			}
		}
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		layer_shell = wl_registry_bind(registry, name,
				&zwlr_layer_shell_v1_interface, 1);
	} else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		xdg_wm_base = wl_registry_bind(registry, name,
				&xdg_wm_base_interface, 1);
	}
}

static void
handle_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
	// who cares
}

static const struct wl_registry_listener registry_listener = {
.global = handle_global,.global_remove = handle_global_remove,};

int
main(int argc, char **argv)
{
	char *namespace = "wlroots";
	int exclusive_zone = -1;
	bool found;
	int c;
	layer = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
	anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
	while ((c = getopt(argc, argv, "xw:h:o:")) != -1) {
		switch (c) {
			case 'o':
				output = atoi(optarg);
				break;
			case 'w':
				width = atoi(optarg);
				break;
			case 'h':
				height = atoi(optarg);
				break;
			case 'x':
				exclusive_zone++;
				break;
			default:
				break;
		}
	}

	if (!height) {
		height = TEXTHEIGHT;
	}

	display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "Failed to create display\n");
		return 1;
	}

	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);

	if (compositor == NULL) {
		fprintf(stderr, "wl_compositor not available\n");
		return 1;
	}
	if (shm == NULL) {
		fprintf(stderr, "wl_shm not available\n");
		return 1;
	}
	if (layer_shell == NULL) {
		fprintf(stderr, "layer_shell not available\n");
		return 1;
	}

	wl_surface = wl_compositor_create_surface(compositor);
	assert(wl_surface);

	layer_surface = zwlr_layer_shell_v1_get_layer_surface(layer_shell,
			wl_surface, wl_output, layer, namespace);
	assert(layer_surface);
	zwlr_layer_surface_v1_set_size(layer_surface, width, height);
	zwlr_layer_surface_v1_set_anchor(layer_surface, anchor);
	zwlr_layer_surface_v1_set_exclusive_zone(layer_surface,
			exclusive_zone <= 0 ? exclusive_zone : height);
	zwlr_layer_surface_v1_add_listener(layer_surface,
			&layer_surface_listener, layer_surface);
	wl_surface_commit(wl_surface);

	wl_display_roundtrip(display);

	while (wl_display_dispatch(display) != -1 && run_display) {
		// This space intentionally left blank
	}

	return 0;
}
