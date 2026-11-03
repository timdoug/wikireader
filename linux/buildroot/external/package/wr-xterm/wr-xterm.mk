################################################################################
#
# wr-xterm
#
################################################################################

WR_XTERM_VERSION = local
WR_XTERM_SITE = $(BR2_EXTERNAL_WIKIREADER_PATH)/package/wr-xterm
WR_XTERM_SITE_METHOD = local
WR_XTERM_DEPENDENCIES = xterm

# Bitmap fonts from the server, no session manager, luit or Tektronix terminal.
# No FreeType: xorg-server's libXft brings it into the build, and xterm would
# load it and fontconfig, 680 KB of text and 280 KB of heap, to draw 5x8.
XTERM_CONF_OPTS += --disable-session-mgt --disable-tek4014 --disable-luit \
	--disable-setuid --disable-setgid --disable-freetype
# No run path to the sysroot: see external.mk.
XTERM_CONF_OPTS += --disable-rpath-hack

define WR_XTERM_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -Wall -Wextra -Werror \
		$(@D)/wr-xterm.c -o $(@D)/wr-xterm $(TARGET_LDFLAGS)
endef

define WR_XTERM_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(XTERM_DIR)/xterm \
		$(TARGET_DIR)/usr/libexec/xterm
	$(INSTALL) -m 0755 $(@D)/wr-xterm $(TARGET_DIR)/usr/bin/xterm
endef

$(eval $(generic-package))
