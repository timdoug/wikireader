################################################################################
#
# wr-console
#
################################################################################

WR_CONSOLE_VERSION = local
WR_CONSOLE_SITE = $(BR2_EXTERNAL_WIKIREADER_PATH)/../../console
WR_CONSOLE_SITE_METHOD = local

# The terminal's escape handling is tested on the build machine first.
define WR_CONSOLE_BUILD_CMDS
	$(HOSTCC) $(HOST_CFLAGS) -o $(@D)/terminal-test $(@D)/terminal-test.c
	$(@D)/terminal-test
	for p in wr-console wr-keyboard; do \
		$(TARGET_CC) $(TARGET_CFLAGS) -Wall -Wextra -Werror \
			-fno-unwind-tables -fno-asynchronous-unwind-tables \
			-ffunction-sections -fdata-sections \
			$(@D)/$$p.c -o $(@D)/$$p \
			$(TARGET_LDFLAGS) || exit 1; \
	done
endef

define WR_CONSOLE_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/wr-console $(TARGET_DIR)/sbin/wr-console
	$(INSTALL) -D -m 0755 $(@D)/wr-keyboard $(TARGET_DIR)/sbin/wr-keyboard
endef

$(eval $(generic-package))
