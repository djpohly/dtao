BINS = ft way layer-shell
DEPS = freetype2 wayland-client wayland-egl wayland-cursor egl gl pixman-1 wlroots

all: $(BINS)

clean:
	$(RM) $(BINS)

CFLAGS += $(shell pkg-config --cflags $(DEPS))
LDLIBS += $(shell pkg-config --libs $(DEPS))

CPPFLAGS += -DWLR_USE_UNSTABLE

WAYLAND_PROTOCOLS=$(shell pkg-config --variable=pkgdatadir wayland-protocols)
WAYLAND_SCANNER=$(shell pkg-config --variable=wayland_scanner wayland-scanner)

xdg-shell-protocol.h:
	$(WAYLAND_SCANNER) client-header \
		$(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@

xdg-shell-protocol.c:
	$(WAYLAND_SCANNER) private-code \
		$(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@

xdg-shell-protocol.o: xdg-shell-protocol.h

wlr-layer-shell-unstable-v1-protocol.h:
	$(WAYLAND_SCANNER) client-header \
		protocols/wlr-layer-shell-unstable-v1.xml $@

wlr-layer-shell-unstable-v1-protocol.c:
	$(WAYLAND_SCANNER) private-code \
		protocols/wlr-layer-shell-unstable-v1.xml $@

wlr-layer-shell-unstable-v1-protocol.o: wlr-layer-shell-unstable-v1-protocol.h

way.o: xdg-shell-protocol.h wlr-layer-shell-unstable-v1-protocol.h

way: xdg-shell-protocol.o wlr-layer-shell-unstable-v1-protocol.o

way: -lrt

layer-shell: xdg-shell-protocol.o wlr-layer-shell-unstable-v1-protocol.o egl_common.o
