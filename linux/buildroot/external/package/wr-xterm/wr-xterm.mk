################################################################################
#
# wr-xterm
#
################################################################################

WR_XTERM_VERSION = local
WR_XTERM_SITE = $(BR2_EXTERNAL_WIKIREADER_PATH)/package/wr-xterm
WR_XTERM_SITE_METHOD = local
WR_XTERM_DEPENDENCIES = xterm

# Bitmap fonts from Xfbdev, no session manager, luit or Tektronix terminal.
XTERM_CONF_OPTS += --disable-session-mgt --disable-tek4014 --disable-luit \
	--disable-setuid --disable-setgid
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
