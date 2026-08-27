# ft - Fearless Terminal
# See LICENSE file for copyright and license details.
.POSIX:

include config.mk

SRC = st.c x.c lua_config.c
OBJ = $(SRC:.c=.o)

# Icon sizes to install
ICON_SIZES = 16 24 32 48 64 128 256 512

all: ft

config.h:
	cp config.def.h config.h

.c.o:
	$(CC) $(STCFLAGS) -c $<

st.o: config.h st.h win.h
x.o: arg.h config.h st.h win.h lua_config.h
lua_config.o: lua_config.h st.h

$(OBJ): config.h config.mk

ft: $(OBJ)
	$(CC) -o $@ $(OBJ) $(STLDFLAGS)

# Static linking target - links most libraries statically except glibc
# Results in larger binary (~6MB) but fewer runtime dependencies
STATIC_LIBS = \
	-Wl,-Bstatic \
	-lXft -lXrender -lfontconfig -lfreetype \
	-lX11 -lxcb -lXau -lXdmcp \
	-llua5.4 \
	-lexpat -lpng -lz -lbz2 \
	-lbrotlidec -lbrotlicommon \
	-Wl,-Bdynamic \
	-lm -lrt -lutil -ldl -lpthread

ft-static: $(OBJ)
	$(CC) -o $@ $(OBJ) $(STATIC_LIBS) $(LDFLAGS)
	@echo "Built ft-static with static linking"
	@echo "Binary size: $$(du -h ft-static | cut -f1)"
	@echo "Runtime dependencies:"
	@ldd ft-static | grep -E "libc|libm|librt|libutil|libdl|libpthread|ld-linux" || true

clean:
	rm -f ft ft-static $(OBJ) ft-$(VERSION).tar.gz

dist: clean
	mkdir -p ft-$(VERSION)
	cp -R FAQ LEGACY TODO LICENSE Makefile README config.mk\
		config.def.h ft.info ft.1 arg.h st.h win.h $(SRC)\
		assets ft-$(VERSION)
	tar -cf - ft-$(VERSION) | gzip > ft-$(VERSION).tar.gz
	rm -rf ft-$(VERSION)

install: ft
	# Binary
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp -f ft $(DESTDIR)$(PREFIX)/bin
	chmod 755 $(DESTDIR)$(PREFIX)/bin/ft
	# Man page
	mkdir -p $(DESTDIR)$(MANPREFIX)/man1
	sed "s/VERSION/$(VERSION)/g" < ft.1 > $(DESTDIR)$(MANPREFIX)/man1/ft.1
	chmod 644 $(DESTDIR)$(MANPREFIX)/man1/ft.1
	# Terminfo
	tic -sx ft.info
	# Desktop file
	mkdir -p $(DESTDIR)$(PREFIX)/share/applications
	cp -f assets/fearless-terminal.desktop $(DESTDIR)$(PREFIX)/share/applications
	chmod 644 $(DESTDIR)$(PREFIX)/share/applications/fearless-terminal.desktop
	# Icons
	@for size in $(ICON_SIZES); do \
		mkdir -p $(DESTDIR)$(PREFIX)/share/icons/hicolor/$${size}x$${size}/apps; \
		cp -f assets/icons/hicolor/$${size}x$${size}/apps/fearless-terminal.png \
			$(DESTDIR)$(PREFIX)/share/icons/hicolor/$${size}x$${size}/apps/; \
		chmod 644 $(DESTDIR)$(PREFIX)/share/icons/hicolor/$${size}x$${size}/apps/fearless-terminal.png; \
	done
	# Update icon cache (may fail if not root, that's ok)
	-gtk-update-icon-cache -f -t $(DESTDIR)$(PREFIX)/share/icons/hicolor 2>/dev/null
	@echo "Please see the README file regarding the terminfo entry of ft."

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/ft
	rm -f $(DESTDIR)$(MANPREFIX)/man1/ft.1
	rm -f $(DESTDIR)$(PREFIX)/share/applications/fearless-terminal.desktop
	@for size in $(ICON_SIZES); do \
		rm -f $(DESTDIR)$(PREFIX)/share/icons/hicolor/$${size}x$${size}/apps/fearless-terminal.png; \
	done
	-gtk-update-icon-cache -f -t $(DESTDIR)$(PREFIX)/share/icons/hicolor 2>/dev/null

.PHONY: all clean dist install uninstall ft-static
