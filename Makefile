BINS = ft way layer-shell

all: $(BINS)

clean:
	$(RM) $(BINS)

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

# Protocol dependencies
way: xdg-shell-protocol.o wlr-layer-shell-unstable-v1-protocol.o
layer-shell: xdg-shell-protocol.o wlr-layer-shell-unstable-v1-protocol.o

# Library dependencies
way: CFLAGS+=$(shell pkg-config --cflags freetype2 wayland-client)
way: LDLIBS+=$(shell pkg-config --libs freetype2 wayland-client) -lrt
layer-shell: CFLAGS+=$(shell pkg-config --cflags freetype2 wayland-client)
layer-shell: LDLIBS+=$(shell pkg-config --libs freetype2 wayland-client) -lrt
ft: CFLAGS+=$(shell pkg-config --cflags freetype2)
ft: LDLIBS+=$(shell pkg-config --libs freetype2)
