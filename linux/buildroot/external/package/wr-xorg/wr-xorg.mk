################################################################################
#
# wr-xorg
#
################################################################################

# The X session on the panel: Buildroot's xorg-server, configured below,
# with its modesetting driver and libinput; startx, which writes the server's
# configuration (there is no udev to find the devices) and starts twm,
# xeyes and an xterm with the on-screen keyboard; and the keymap.  The
# server's patches, modesetting's one-bit support among them, are in
# patches/xserver_xorg-server.
WR_XORG_VERSION = local
WR_XORG_SITE = $(BR2_EXTERNAL_WIKIREADER_PATH)/package/wr-xorg
WR_XORG_SITE_METHOD = local
WR_XORG_LICENSE = MIT
# After the server and xkeyboard-config, whose keyboard data this replaces
# with the one keymap the server loads.
WR_XORG_DEPENDENCIES = host-xapp_xkbcomp xkeyboard-config xserver_xorg-server \
	xdriver_xf86-input-libinput

WR_XORG_KEYMAP = /usr/share/X11/xkb/wikireader.xkm

# Extensions off as nothing here uses them, but Present, which 21.1's
# os/utils.c cannot build without, and DRI2, without which its autotools
# build leaves out the modesetting driver; pciaccess kept, as without it drivers
# take a branch the server's API no longer has.  The keymap is compiled at
# build time, and the stack is the size the server's recursion needs: a
# program's stack without an MMU is what its link asks for.
ifeq ($(BR2_PACKAGE_WR_XORG),y)
XSERVER_XORG_SERVER_CONF_OPTS += \
	--disable-composite --disable-xres --disable-clientids \
	--disable-record --disable-xv --disable-xvmc --disable-screensaver \
	--disable-dbe --disable-dpms --disable-xace --disable-mitshm \
	--disable-xdmcp --disable-xdm-auth-1 --disable-secure-rpc \
	--disable-xselinux --disable-xcsecurity --disable-xinerama \
	--disable-xf86vidmode --disable-dga \
	--disable-dri --disable-dri3 --disable-glamor \
	--disable-glx \
	--disable-config-udev --disable-config-udev-kms --disable-vgahw \
	--disable-int10-module --disable-linux-acpi --disable-linux-apm \
	--disable-xfree86-utils --disable-suid-wrapper \
	--disable-input-thread --disable-libunwind --disable-ipv6 \
	--disable-listen-tcp --enable-listen-unix --disable-listen-local \
	--disable-docs --disable-devel-docs --disable-xvfb --disable-xephyr \
	--disable-xf86-input-inputtest \
	--with-default-font-path=built-ins --with-xkb-output=/tmp \
	CFLAGS="$(TARGET_CFLAGS) -I$(STAGING_DIR)/usr/include/pixman-1 \
		-DXKB_COMPILED_KEYMAP=\\\"$(WR_XORG_KEYMAP)\\\""
XSERVER_XORG_SERVER_CONF_ENV += \
	LDFLAGS="$(TARGET_LDFLAGS) -Wl,-z,stack-size=131072"
endif

# A US keyboard, compiled here from xkeyboard-config's sources: the target
# carries only the result.
define WR_XORG_BUILD_CMDS
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

# libinput keeps its generic quirks; those for named vendors' USB and
# Bluetooth devices and for other machines, which libinput parses at every
# start, 4 s here, cannot match the WikiReader's own input devices.
define WR_XORG_INSTALL_TARGET_CMDS
	find $(TARGET_DIR)/usr/share/libinput -name '[35]0-*.quirks' -delete
	rm -rf $(TARGET_DIR)/usr/share/X11/xkb
	$(INSTALL) -D -m 0644 $(@D)/wikireader.xkm $(TARGET_DIR)$(WR_XORG_KEYMAP)
	$(INSTALL) -D -m 0755 $(@D)/startx $(TARGET_DIR)/usr/bin/startx
	$(INSTALL) -D -m 0644 $(@D)/wikireader.twmrc \
		$(TARGET_DIR)/usr/share/X11/twm/wikireader.twmrc
	$(INSTALL) -D -m 0755 $(@D)/xkeyboard $(TARGET_DIR)/usr/bin/xkeyboard
	$(INSTALL) -D -m 0644 $(@D)/XVkbd-wikireader \
		$(TARGET_DIR)/usr/share/X11/app-defaults/XVkbd-wikireader
endef

# xorg-server selects the misc and cursor font files, 5.9 MB, which a
# libXfont2 of built-in fonts only cannot read.
define WR_XORG_REMOVE_FONT_FILES
	rm -rf $(TARGET_DIR)/usr/share/fonts/X11
endef
WR_XORG_TARGET_FINALIZE_HOOKS += WR_XORG_REMOVE_FONT_FILES

$(eval $(generic-package))
