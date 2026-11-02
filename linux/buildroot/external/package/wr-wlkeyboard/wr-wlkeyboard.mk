################################################################################
#
# wr-wlkeyboard
#
################################################################################

WR_WLKEYBOARD_VERSION = local
WR_WLKEYBOARD_SITE = $(BR2_EXTERNAL_WIKIREADER_PATH)/../../console
WR_WLKEYBOARD_SITE_METHOD = local
WR_WLKEYBOARD_DEPENDENCIES = host-pkgconf host-wayland wayland wayland-protocols

# The layer shell's code names xdg_popup, so xdg-shell's comes too.
WR_WLKEYBOARD_PROTOCOLS = \
	$(@D)/protocol/virtual-keyboard-unstable-v1.xml \
	$(@D)/protocol/wlr-layer-shell-unstable-v1.xml \
	$(STAGING_DIR)/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml

# Wayland listeners take arguments they need not use.
define WR_WLKEYBOARD_BUILD_CMDS
	for xml in $(WR_WLKEYBOARD_PROTOCOLS); do \
		name=$$(basename $$xml .xml); \
		$(HOST_DIR)/bin/wayland-scanner client-header $$xml \
			$(@D)/$$name-client-protocol.h && \
		$(HOST_DIR)/bin/wayland-scanner private-code $$xml \
			$(@D)/$$name-protocol.c || exit 1; \
	done
	$(TARGET_CC) $(TARGET_CFLAGS) -Wall -Wextra -Wno-unused-parameter -Werror \
		-I$(@D) $(@D)/wlkeyboard.c $(@D)/*-protocol.c -o $(@D)/wlkeyboard \
		$(TARGET_LDFLAGS) \
		$$($(PKG_CONFIG_HOST_BINARY) --cflags --libs wayland-client)
endef

define WR_WLKEYBOARD_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/wlkeyboard $(TARGET_DIR)/usr/bin/wlkeyboard
endef

$(eval $(generic-package))
