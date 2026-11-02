################################################################################
#
# wr-wayland
#
################################################################################

# The session's font is X's misc-fixed 6x13, the console keyboard's, as a
# BDF file, which FreeType reads directly; only that one of the package's
# fonts is installed.
WR_WAYLAND_VERSION = 1.1.3
WR_WAYLAND_SOURCE = font-misc-misc-$(WR_WAYLAND_VERSION).tar.xz
WR_WAYLAND_SITE = https://xorg.freedesktop.org/archive/individual/font
WR_WAYLAND_LICENSE = Public Domain
WR_WAYLAND_LICENSE_FILES = COPYING
# After foot, whose default foot.ini this replaces, and after the X server,
# which prunes the keyboard data this installs beside its own keymap.
WR_WAYLAND_DEPENDENCIES = cage foot host-xapp_xkbcomp xkeyboard-config \
	$(if $(BR2_PACKAGE_XSERVER_KDRIVE),xserver-kdrive)

# libxkbcommon builds a keymap from rules, model and layout, reading
# xkeyboard-config's 4 MB of sources each time.  The panel has one keymap,
# a US keyboard, so it is resolved here, by xkbcomp, into a keymap of one
# file a component, and a rules file that gives it for every model and
# layout: what every client asks for, evdev, pc105 and us, included.
WR_WAYLAND_XKB = $(TARGET_DIR)/usr/share/X11/xkb

define WR_WAYLAND_BUILD_CMDS
	printf '%s\n' 'xkb_keymap {' \
		'	xkb_keycodes { include "evdev+aliases(qwerty)" };' \
		'	xkb_types { include "complete" };' \
		'	xkb_compat { include "complete" };' \
		'	xkb_symbols { include "pc+us+inet(evdev)" };' \
		'};' > $(@D)/wikireader.xkb
	$(HOST_DIR)/bin/xkbcomp -w 0 -I$(STAGING_DIR)/usr/share/X11/xkb -xkb \
		$(@D)/wikireader.xkb $(@D)/wikireader-resolved.xkb
	rm -rf $(@D)/xkb
	mkdir -p $(@D)/xkb/keycodes $(@D)/xkb/types $(@D)/xkb/compat \
		$(@D)/xkb/symbols $(@D)/xkb/rules
	awk -v dir=$(@D)/xkb ' \
		/^xkb_keycodes / { out = dir "/keycodes/wikireader" } \
		/^xkb_types / { out = dir "/types/wikireader" } \
		/^xkb_compatibility / { out = dir "/compat/wikireader" } \
		/^xkb_symbols / { out = dir "/symbols/wikireader" } \
		out != "" { sub(/^xkb_[a-z]+ "[^"]*"/, "default &"); print > out } \
		/^};/ { out = "" }' $(@D)/wikireader-resolved.xkb
	printf '%s\n' \
		'// One keymap, a US keyboard, for every model and layout.' \
		'! model = keycodes' '  * = wikireader' \
		'! model = types' '  * = wikireader' \
		'! model = compat' '  * = wikireader' \
		'! model layout = symbols' '  * * = wikireader' \
		> $(@D)/xkb/rules/evdev
endef

define WR_WAYLAND_INSTALL_TARGET_CMDS
	rm -rf $(WR_WAYLAND_XKB)/compat $(WR_WAYLAND_XKB)/geometry \
		$(WR_WAYLAND_XKB)/keycodes $(WR_WAYLAND_XKB)/rules \
		$(WR_WAYLAND_XKB)/symbols $(WR_WAYLAND_XKB)/types
	cp -R $(@D)/xkb/. $(WR_WAYLAND_XKB)/
	$(INSTALL) -D -m 0644 $(@D)/6x13.bdf \
		$(TARGET_DIR)/usr/share/fonts/misc/6x13.bdf
	$(INSTALL) -D -m 0644 $(WR_WAYLAND_PKGDIR)/70-wr-fixed.conf \
		$(TARGET_DIR)/etc/fonts/conf.d/70-wr-fixed.conf
	$(INSTALL) -D -m 0644 $(WR_WAYLAND_PKGDIR)/foot.ini \
		$(TARGET_DIR)/etc/xdg/foot/foot.ini
	$(INSTALL) -D -m 0755 $(WR_WAYLAND_PKGDIR)/startw \
		$(TARGET_DIR)/usr/bin/startw
endef

$(eval $(generic-package))
