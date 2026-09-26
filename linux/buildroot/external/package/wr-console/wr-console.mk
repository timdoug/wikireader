################################################################################
#
# wr-console
#
################################################################################

WR_CONSOLE_VERSION = local
WR_CONSOLE_SITE = $(BR2_EXTERNAL_WIKIREADER_PATH)/../../console
WR_CONSOLE_SITE_METHOD = local

define WR_CONSOLE_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -Wall -Wextra -Werror \
		-fno-unwind-tables -fno-asynchronous-unwind-tables \
		-ffunction-sections -fdata-sections \
		$(@D)/wr-console.c -o $(@D)/wr-console \
		$(TARGET_LDFLAGS) -Wl,-elf2flt=--shared-text
endef

define WR_CONSOLE_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/wr-console $(TARGET_DIR)/sbin/wr-console
endef

$(eval $(generic-package))
