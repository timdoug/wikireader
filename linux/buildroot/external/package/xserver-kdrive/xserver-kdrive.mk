################################################################################
#
# xserver-kdrive
#
################################################################################

XSERVER_KDRIVE_VERSION = 1.19.7
XSERVER_KDRIVE_SOURCE = xorg-server-$(XSERVER_KDRIVE_VERSION).tar.bz2
XSERVER_KDRIVE_SITE = https://xorg.freedesktop.org/archive/individual/xserver
XSERVER_KDRIVE_LICENSE = MIT
XSERVER_KDRIVE_LICENSE_FILES = COPYING
XSERVER_KDRIVE_DEPENDENCIES = \
	host-xapp_xkbcomp libsha1 pixman xkeyboard-config xlib_libXau \
	xlib_libXfont2 xlib_libxkbfile xlib_xtrans xorgproto

XSERVER_KDRIVE_KEYMAP = /usr/share/X11/xkb/wikireader.xkm
# 1.19 predates GCC 14's pointer-type errors: CARD32 is unsigned long here
# and libXfont2's callbacks take uint32_t, the same size.
# The server recurses deeply (region code, the font renderer, requests),
# and without an MMU its stack is the size the link asks for.
XSERVER_KDRIVE_CONF_ENV = \
	CFLAGS="$(TARGET_CFLAGS) -Wno-error=incompatible-pointer-types \
		-DXKB_COMPILED_KEYMAP=\\\"$(XSERVER_KDRIVE_KEYMAP)\\\"" \
	LDFLAGS="$(TARGET_LDFLAGS) -Wl,-z,stack-size=131072"

XSERVER_KDRIVE_CONF_OPTS = \
	--enable-kdrive --enable-xfbdev --enable-kdrive-evdev \
	--disable-kdrive-kbd --disable-kdrive-mouse --disable-tslib \
	--disable-xorg --disable-xvfb --disable-xnest --disable-xwin \
	--disable-xquartz --disable-xephyr --disable-xfake --disable-dmx \
	--disable-glx --disable-dri --disable-dri2 --disable-dri3 \
	--disable-glamor --disable-libdrm --disable-present \
	--disable-config-udev --disable-config-udev-kms --disable-config-hal \
	--disable-systemd-logind --disable-xshmfence --disable-mitshm \
	--disable-xdmcp --disable-xdm-auth-1 --disable-secure-rpc \
	--disable-xselinux --disable-xcsecurity --disable-xinerama \
	--disable-xf86vidmode --disable-input-thread --disable-libunwind \
	--disable-ipv6 --disable-listen-tcp --enable-listen-unix \
	--disable-listen-local --disable-docs --disable-devel-docs \
	--disable-unit-tests --disable-selective-werror --without-dtrace \
	--with-sha1=libsha1 \
	--with-default-font-path=built-ins \
	--with-xkb-path=/usr/share/X11/xkb --with-xkb-output=/tmp

# A US keyboard, compiled here from xkeyboard-config's sources: the target
# carries only the result.
define XSERVER_KDRIVE_BUILD_KEYMAP
	printf '%s\n' 'xkb_keymap {' \
		'	xkb_keycodes { include "evdev+aliases(qwerty)" };' \
		'	xkb_types { include "complete" };' \
		'	xkb_compat { include "complete" };' \
		'	xkb_symbols { include "pc+us+inet(evdev)" };' \
		'	xkb_geometry { include "pc(pc105)" };' \
		'};' > $(@D)/wikireader.xkb
	$(HOST_DIR)/bin/xkbcomp -w 0 -I$(XKEYBOARD_CONFIG_DIR) -xkm \
		$(@D)/wikireader.xkb $(@D)/wikireader.xkm
endef
XSERVER_KDRIVE_POST_BUILD_HOOKS += XSERVER_KDRIVE_BUILD_KEYMAP

define XSERVER_KDRIVE_INSTALL_KEYMAP
	rm -rf $(TARGET_DIR)/usr/share/X11/xkb
	$(INSTALL) -D -m 0644 $(@D)/wikireader.xkm $(TARGET_DIR)$(XSERVER_KDRIVE_KEYMAP)
	$(INSTALL) -D -m 0755 $(XSERVER_KDRIVE_PKGDIR)/startx $(TARGET_DIR)/usr/bin/startx
	$(INSTALL) -D -m 0644 $(XSERVER_KDRIVE_PKGDIR)/wikireader.twmrc \
		$(TARGET_DIR)/usr/share/X11/twm/wikireader.twmrc
endef
XSERVER_KDRIVE_POST_INSTALL_TARGET_HOOKS += XSERVER_KDRIVE_INSTALL_KEYMAP

$(eval $(autotools-package))
