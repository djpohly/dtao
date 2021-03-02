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
#include <fcft/fcft.h>
#include <pixman.h>
#include "wlr-layer-shell-unstable-v1-protocol.h"
#include "xdg-shell-protocol.h"

#define TEXT "howdy world"

// Text color
static pixman_image_t *fgcolor;

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
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

static struct fcft_font *font;

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

	pixman_image_t *canvas = pixman_image_create_bits(PIXMAN_x8r8g8b8,
			width, height, data, width * 4);

	char *text;

	int n, num_chars;


	text = TEXT;
	num_chars = strlen(text);

	/* cmap selection omitted;                                        */
	/* for simplicity we assume that the font contains a Unicode cmap */

	fgcolor = pixman_image_create_solid_fill(
			&(pixman_color_t){
				.red = 0xd000,
				.green = 0xe800,
				.blue = 0x4000,
				.alpha = 0xffff,
			});

	int xpos = 0, ypos = 0;

	for (n = 0; n < num_chars; n++) {
		const struct fcft_glyph *glyph = fcft_glyph_rasterize(
				font, text[n], FCFT_SUBPIXEL_DEFAULT);
		if (!glyph)
			continue;
		long x_kern = 0;
		if (n > 0)
			fcft_kerning(font, text[n - 1], text[n], &x_kern, NULL);
		xpos += x_kern;

		if (pixman_image_get_format(glyph->pix) == PIXMAN_a8r8g8b8) {
			pixman_image_composite32(
				PIXMAN_OP_OVER, glyph->pix, NULL, canvas, 0, 0, 0, 0,
				xpos + glyph->x, ypos + font->ascent - glyph->y,
				glyph->width, glyph->height);
		} else {
			pixman_image_composite32(
				PIXMAN_OP_OVER, fgcolor, glyph->pix, canvas, 0, 0, 0, 0,
				xpos + glyph->x, ypos + font->ascent - glyph->y,
				glyph->width, glyph->height);
		}

		/* increment pen position */
		xpos += glyph->advance.x;
		ypos += glyph->advance.y;
	}

	pixman_image_unref(canvas);
	pixman_image_unref(fgcolor);
	munmap(data, size);
	wl_buffer_add_listener(buffer, &wl_buffer_listener, NULL);
	return buffer;
}

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
	char *fontstr = "";
	int exclusive_zone = -1;
	int c;
	layer = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
	anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
	while ((c = getopt(argc, argv, "xw:h:o:f:")) != -1) {
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
			case 'f':
				fontstr = optarg;
				break;
			default:
				break;
		}
	}

	fcft_set_scaling_filter(FCFT_SCALING_FILTER_LANCZOS3);
	font = fcft_from_name(1, (const char *[]) {fontstr}, NULL);
	if (!font) {
		fprintf(stderr, "error in fcft_from_name\n");
		return 1;
	}

	if (!height) {
		height = font->ascent + font->descent;
		fprintf(stderr, "height = %d + %d = %d\n", font->ascent,
				font->descent, height);
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
	if (exclusive_zone > 0) {
		zwlr_layer_surface_v1_set_exclusive_zone(layer_surface, height);
	} else {
		zwlr_layer_surface_v1_set_exclusive_zone(layer_surface,
				exclusive_zone);
	}
	zwlr_layer_surface_v1_add_listener(layer_surface,
			&layer_surface_listener, layer_surface);
	wl_surface_commit(wl_surface);

	wl_display_roundtrip(display);

	while (wl_display_dispatch(display) != -1 && run_display) {
		// This space intentionally left blank
	}

	fcft_destroy(font);

	return 0;
}
