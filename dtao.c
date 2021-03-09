#define _GNU_SOURCE
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
#include <sys/select.h>
#include <time.h>
#include <unistd.h>
#include "utf8.h"
#include "wlr-layer-shell-unstable-v1-protocol.h"
#include "xdg-shell-protocol.h"

#define BARF(fmt, ...)		do { fprintf(stderr, fmt "\n", ##__VA_ARGS__); exit(EXIT_FAILURE); } while (0)
#define EBARF(fmt, ...)		BARF(fmt ": %s", ##__VA_ARGS__, strerror(errno))

#define MAX_LINE_LEN 8192

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct zwlr_layer_shell_v1 *layer_shell;

static struct zwlr_layer_surface_v1 *layer_surface;
static struct wl_output *wl_output;
static struct wl_surface *wl_surface;

static uint32_t output = UINT32_MAX;

static uint32_t width, height, stride, bufsize;
static int exclusive_zone = -1;
static bool run_display = true;

static struct fcft_font *font;
static char line[MAX_LINE_LEN];

static void
wl_buffer_release(void *data, struct wl_buffer *wl_buffer)
{
	/* Sent by the compositor when it's no longer using this buffer */
	wl_buffer_destroy(wl_buffer);
}

static const struct wl_buffer_listener wl_buffer_listener = {
	.release = wl_buffer_release,
};

/* Shared memory support function adapted from [wayland-book] */
static int
allocate_shm_file(size_t size)
{
	int fd = memfd_create("surface", MFD_CLOEXEC);
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

static uint32_t *
new_buffer(struct wl_buffer **pbuf)
{
	/* Allocate buffer to be attached to the surface */
	int fd = allocate_shm_file(bufsize);
	if (fd == -1)
		return NULL;

	uint32_t *data = mmap(NULL, bufsize,
			PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		close(fd);
		return NULL;
	}

	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, bufsize);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0,
			width, height, stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	wl_buffer_add_listener(buffer, &wl_buffer_listener, NULL);
	*pbuf = buffer;
	return data;
}

static struct wl_buffer *
draw_frame(char *text)
{
	struct wl_buffer *buffer;
	uint32_t *data = new_buffer(&buffer);
	if (!data)
		return NULL;

	/* Colors (premultiplied!) */
	pixman_color_t bgcolor = {
		.red = 0x8000,
		.green = 0x7000,
		.blue = 0xd000,
		.alpha = 0xffff,
	};
	pixman_color_t textbgcolor = bgcolor;
	pixman_color_t textfgcolor = {
		.red = 0x0000,
		.green = 0x0000,
		.blue = 0x0000,
		.alpha = 0x8fff,
	};

	/* Pixman image corresponding to main buffer */
	pixman_image_t *bar = pixman_image_create_bits(PIXMAN_a8r8g8b8,
			width, height, data, width * 4);
	/* Fill bar with background color */
	pixman_image_fill_boxes(PIXMAN_OP_SRC, bar, &bgcolor, 1,
			&(pixman_box32_t) {.x1 = 0, .x2 = width, .y1 = 0, .y2 = height});

	/* Text background and foreground layers */
	pixman_image_t *background = pixman_image_create_bits(PIXMAN_a8r8g8b8,
			width, height, data, width * 4);
	pixman_image_t *foreground = pixman_image_create_bits(PIXMAN_a8r8g8b8,
			width, height, NULL, width * 4);

	pixman_image_t *fgfill = pixman_image_create_solid_fill(&textfgcolor);

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
	pixman_image_fill_boxes(PIXMAN_OP_SRC, background, &textbgcolor, 1, &bgbox);
	lastbgx = xpos;

	/* Draw background and foreground on bar */
	pixman_image_composite32(PIXMAN_OP_OVER, background, NULL, bar, 0, 0, 0, 0,
			0, 0, width, height);
	pixman_image_composite32(PIXMAN_OP_OVER, foreground, NULL, bar, 0, 0, 0, 0,
			0, 0, width, height);

	pixman_image_unref(foreground);
	pixman_image_unref(background);
	pixman_image_unref(bar);
	munmap(data, bufsize);
	return buffer;
}

/* Layer-surface setup adapted from layer-shell example in [wlroots] */
static void
layer_surface_configure(void *data,
		struct zwlr_layer_surface_v1 *surface,
		uint32_t serial, uint32_t w, uint32_t h)
{
	width = w;
	height = h;
	stride = width * 4;
	bufsize = stride * height;

	if (exclusive_zone > 0)
		exclusive_zone = height;
	zwlr_layer_surface_v1_set_exclusive_zone(layer_surface, exclusive_zone);
	zwlr_layer_surface_v1_ack_configure(surface, serial);

	struct wl_buffer *buffer = draw_frame(line);
	if (!buffer)
		return;
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
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
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
	}
}

static const struct wl_registry_listener registry_listener = {.global = handle_global,};

static void
read_stdin(void)
{
	char *end;
	ssize_t b = read(STDIN_FILENO, line, MAX_LINE_LEN - 1);
	if (b < 0)
		perror("read");
	if (b <= 0) {
		run_display = 0;
		return;
	}
	/* Terminate string after first line */
	/* XXX handle multiple lines here */
	if ((end = memchr(line, '\n', b))) {
		*end = '\0';
	} else {
		line[b] = '\0';
	}
	struct wl_buffer *buffer = draw_frame(line);
	if (!buffer)
		return;
	wl_surface_attach(wl_surface, buffer, 0, 0);
	wl_surface_commit(wl_surface);
}

static void
event_loop(void)
{
	int ret;
	int wlfd = wl_display_get_fd(display);

	while (run_display) {
		fd_set rfds;
		FD_ZERO(&rfds);
		FD_SET(STDIN_FILENO, &rfds);
		FD_SET(wlfd, &rfds);

		/* Does this need to be inside the loop? */
		wl_display_flush(display);

		ret = select(wlfd + 1, &rfds, NULL, NULL, NULL);
		if (ret < 0)
			EBARF("select");

		if (FD_ISSET(STDIN_FILENO, &rfds))
			read_stdin();

		if (FD_ISSET(wlfd, &rfds))
			if (wl_display_dispatch(display) == -1)
				break;
	}
}

int
main(int argc, char **argv)
{
	char *namespace = "wlroots";
	char *fontstr = "";
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

	/* Set up display and protocols */
	display = wl_display_connect(NULL);
	if (!display)
		BARF("Failed to create display");

	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);

	if (!compositor || !shm || !layer_shell)
		BARF("compositor does not support all needed protocols");

	/* Load selected font */
	fcft_set_scaling_filter(FCFT_SCALING_FILTER_LANCZOS3);
	font = fcft_from_name(1, (const char *[]) {fontstr}, NULL);
	if (!font)
		BARF("could not load font");

	/* Create layer-shell surface */
	wl_surface = wl_compositor_create_surface(compositor);
	if (!wl_surface)
		BARF("could not create wl_surface");

	layer_surface = zwlr_layer_shell_v1_get_layer_surface(layer_shell,
			wl_surface, wl_output, layer, namespace);
	if (!layer_surface)
		BARF("could not create layer_surface");
	zwlr_layer_surface_v1_add_listener(layer_surface,
			&layer_surface_listener, layer_surface);

	/* Set layer size and positioning */
	if (!height)
		height = font->ascent + font->descent;

	zwlr_layer_surface_v1_set_size(layer_surface, width, height);
	zwlr_layer_surface_v1_set_anchor(layer_surface, anchor);
	if (exclusive_zone > 0)
		exclusive_zone = height;
	zwlr_layer_surface_v1_set_exclusive_zone(layer_surface, exclusive_zone);
	wl_surface_commit(wl_surface);
	wl_display_roundtrip(display);

	event_loop();

	/* Clean everything up */
	zwlr_layer_surface_v1_destroy(layer_surface);
	wl_surface_destroy(wl_surface);
	zwlr_layer_shell_v1_destroy(layer_shell);
	fcft_destroy(font);
	wl_shm_destroy(shm);
	wl_compositor_destroy(compositor);
	wl_registry_destroy(registry);
	wl_display_disconnect(display);

	return 0;
}
