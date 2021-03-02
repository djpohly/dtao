BINS = dtao

CFLAGS += -Wall -Wextra -Wno-unused-parameter

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
dtao: xdg-shell-protocol.o wlr-layer-shell-unstable-v1-protocol.o

# Library dependencies
dtao: CFLAGS+=$(shell pkg-config --cflags freetype2 wayland-client fcft pixman-1)
dtao: LDLIBS+=$(shell pkg-config --libs freetype2 wayland-client fcft pixman-1) -lrt
