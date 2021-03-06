/* Portions of this code are from wlroots layer-shell example and used under the
 * following license:
 *
 * Copyright (c) 2017, 2018 Drew DeVault
 * Copyright (c) 2014 Jari Vetoniemi
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is furnished to do
 * so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 *
 * Portions of this code are derived from example code found in The Wayland
 * Protocol by Drew DeVault (https://wayland-book.com/).  The book is available
 * under CC-BY-SA 4.0; we will assume (unless notified otherwise) that the
 * author intends the book's code examples to be permissively licensed as well.
 */

#define _POSIX_C_SOURCE 200112L
#include <assert.h>
#include <errno.h>
#include <fcft/fcft.h>
#include <fcntl.h>
#include <limits.h>
#include <pixman.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include "utf8.h"
#include "wlr-layer-shell-unstable-v1-protocol.h"
#include "xdg-shell-protocol.h"

#define TEXT "howdy ^^^^ 😎 ^bg(#ffffff)world"

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *xdg_wm_base;
static struct zwlr_layer_shell_v1 *layer_shell;

static struct zwlr_layer_surface_v1 *layer_surface;
static struct wl_output *wl_output;
static struct wl_surface *wl_surface;

static uint32_t output = UINT32_MAX;

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

static char *
handle_cmd(char *cmd, pixman_color_t *bg)
{
	char *arg, *end;

	if (!(arg = strchr(cmd, '(')) || !(end = strchr(arg + 1, ')')))
		return cmd;

	*arg++ = '\0';
	*end = '\0';

	if (!strcmp(cmd, "bg")) {
		uint16_t r, g, b, a;
		if (!*arg) {
			/* XXX set default background color */
			bg->red = bg->green = bg->blue = 0x0000;
			bg->alpha = 0xffff;
			return end;
		}
		int ret = sscanf(arg, "#%02hx%02hx%02hx%02hx", &r, &g, &b, &a);
		fprintf(stderr, "ret=%d\n", ret);
		if (ret < 3) {
			fprintf(stderr, "Malformed bg command\n");
			return end;
		}
		if (ret == 3)
			a = 0xff;
		bg->red = r | r << 8;
		bg->green = g | g << 8;
		bg->blue = b | b << 8;
		bg->alpha = a | a << 8;
	} else {
		fprintf(stderr, "Unrecognized command \"%s\"\n", cmd);
	}

	return end;
}

static struct wl_buffer *
draw_frame(void)
{
	int stride = width * 4;
	int size = stride * height;

	/* Allocate buffer to be attached to the surface */
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
			width, height, stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);

	/* Colors (premultiplied!) */
	pixman_color_t bgcolor = {
		.red = 0x0000,
		.green = 0x0000,
		.blue = 0x2000,
		.alpha = 0x4fff,
	};
	pixman_color_t textbgcolor = bgcolor;
	pixman_color_t textfgcolor = {
		.red = 0x1fff,
		.green = 0x0000,
		.blue = 0x0000,
		.alpha = 0x8fff,
	};

	/* Pixman image corresponding to main buffer */
	pixman_image_t *bar = pixman_image_create_bits(PIXMAN_a8r8g8b8,
			width, height, data, width * 4);

	/* Text foreground layer */
	pixman_image_t *foreground = pixman_image_create_bits(PIXMAN_a8r8g8b8,
			width, height, NULL, width * 4);


	pixman_image_t *fgfill = pixman_image_create_solid_fill(&textfgcolor);

	/* XXX for testing */
	char *text = strdup(TEXT);

	/* Start drawing in top left (ypos sets the text baseline) */
	int xpos = 0, ypos = font->ascent;
	int lastbgx = 0;

	uint32_t codepoint, lastcp = 0, state = UTF8_ACCEPT;
	for (char *p = text; *p; p++) {
		/* Check for inline ^ commands */
		if (state == UTF8_ACCEPT && *p == '^') {
			p++;
			if (*p != '^') {
				p = handle_cmd(p, &textbgcolor);
				continue;
			}
		}

		/* Returns nonzero if more bytes are needed */
		if (utf8decode(&state, &codepoint, *p))
			continue;

		/* Turn off subpixel rendering, which complicates things when
		 * mixed with alpha channels */
		const struct fcft_glyph *glyph = fcft_glyph_rasterize(font, codepoint,
				FCFT_SUBPIXEL_NONE);
		if (!glyph)
			continue;

		/* Adjust x position based on kerning with previous glyph */
		long x_kern = 0;
		if (lastcp)
			fcft_kerning(font, lastcp, codepoint, &x_kern, NULL);
		xpos += x_kern;
		lastcp = codepoint;

		/* Detect and handle pre-rendered glyphs (e.g. emoji) */
		if (pixman_image_get_format(glyph->pix) == PIXMAN_a8r8g8b8) {
			/* Only the alpha channel of the mask is used, so we can
			 * use fgfill here to blend prerendered glyphs with the
			 * same opacity */
			pixman_image_composite32(
				PIXMAN_OP_OVER, glyph->pix, fgfill, foreground, 0, 0, 0, 0,
				xpos + glyph->x, ypos - glyph->y, glyph->width, glyph->height);
		} else {
			/* Applying the foreground color here would mess up
			 * component alphas for subpixel-rendered text, so we
			 * apply it when blending. */
			pixman_image_composite32(
				PIXMAN_OP_OVER, fgfill, glyph->pix, foreground, 0, 0, 0, 0,
				xpos + glyph->x, ypos - glyph->y, glyph->width, glyph->height);
		}

		/* increment pen position */
		xpos += glyph->advance.x;
		ypos += glyph->advance.y;
	}
	pixman_image_unref(fgfill);

	if (state != UTF8_ACCEPT)
		fprintf(stderr, "malformed UTF-8 sequence\n");

	/* Example - something like this could be used for ^bg() */
	pixman_box32_t bgbox = {.x1 = lastbgx, .x2 = xpos, .y1 = 0, .y2 = height};
	pixman_image_fill_boxes(PIXMAN_OP_SRC, bar, &textbgcolor, 1, &bgbox);
	lastbgx = xpos;

	/* Fill remainder of bar with background color */
	pixman_image_fill_boxes(PIXMAN_OP_SRC, bar, &bgcolor, 1,
			&(pixman_box32_t) {.x1 = lastbgx, .x2 = width, .y1 = 0, .y2 = height});

	/* Draw text over background */
	pixman_image_composite32(PIXMAN_OP_OVER, foreground, NULL, bar, 0, 0, 0, 0,
			0, 0, width, height);

	pixman_image_unref(foreground);
	pixman_image_unref(bar);
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

static struct zwlr_layer_surface_v1_listener layer_surface_listener = {
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

static const struct wl_registry_listener registry_listener = {.global = handle_global,};

int
main(int argc, char **argv)
{
	char *namespace = "wlroots";
	char *fontstr = "";
	int exclusive_zone = -1;
	int c;
	uint32_t layer = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
	uint32_t anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
	while ((c = getopt(argc, argv, "bf:h:o:w:x")) != -1) {
		switch (c) {
			case 'b':
				anchor ^= ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
					ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
				break;
			case 'f':
				fontstr = optarg;
				break;
			case 'h':
				height = atoi(optarg);
				break;
			case 'o':
				output = atoi(optarg);
				break;
			case 'w':
				width = atoi(optarg);
				break;
			case 'x':
				// -x: avoid other exclusive zones
				// -xx: request exclusive zone for ourselves
				exclusive_zone++;
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
		/* This space intentionally left blank */
	}

	fcft_destroy(font);

	return 0;
}
