################################################################################
#
# wikireader-system
#
################################################################################

WIKIREADER_SYSTEM_VERSION = local
WIKIREADER_SYSTEM_SITE = $(BR2_EXTERNAL_WIKIREADER_PATH)/../../initramfs
WIKIREADER_SYSTEM_SITE_METHOD = local
# Installed after BusyBox, whose own inittab this replaces.
WIKIREADER_SYSTEM_DEPENDENCIES = busybox

define WIKIREADER_SYSTEM_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -Wall -Werror -fno-unwind-tables \
		-fno-asynchronous-unwind-tables -ffunction-sections \
		-fdata-sections -I$(BR2_EXTERNAL_WIKIREADER_PATH)/../../console \
		$(@D)/wr-selftest.c \
		-o $(@D)/wr-selftest $(TARGET_LDFLAGS) -Wl,--gc-sections
	$(TARGET_CC) $(TARGET_CFLAGS) -Wall -Werror $(@D)/wr-core.c \
		-o $(@D)/wr-core $(TARGET_LDFLAGS)
endef

# No seed ships in the image: every copy would credit the same one.  A
# device's first boot starts without, and seedrng saves its own (late).
define WIKIREADER_SYSTEM_SEED_DIR
	mkdir -p -m 0700 $(TARGET_DIR)/var/lib/seedrng
endef

define WIKIREADER_SYSTEM_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $(@D)/inittab $(TARGET_DIR)/etc/inittab
	$(INSTALL) -D -m 0644 $(@D)/fstab $(TARGET_DIR)/etc/fstab
	$(INSTALL) -D -m 0644 $(@D)/profile $(TARGET_DIR)/etc/profile
	$(INSTALL) -D -m 0755 $(@D)/rcS $(TARGET_DIR)/etc/init.d/rcS
	$(INSTALL) -D -m 0755 $(@D)/selftest $(TARGET_DIR)/etc/init.d/selftest
	$(INSTALL) -D -m 0755 $(@D)/late $(TARGET_DIR)/etc/init.d/late
	$(INSTALL) -D -m 0755 $(@D)/busybox-test \
		$(TARGET_DIR)/etc/init.d/busybox-test
	$(INSTALL) -D -m 0755 $(@D)/wr-selftest \
		$(TARGET_DIR)/usr/libexec/wr-selftest
	$(INSTALL) -D -m 0755 $(@D)/wr-core \
		$(TARGET_DIR)/usr/libexec/wr-core
	mkdir -p $(TARGET_DIR)/mnt/sd
	mkdir -p -m 0700 $(TARGET_DIR)/root
	$(WIKIREADER_SYSTEM_SEED_DIR)
endef

# Runs after every target package is installed and before stripping. FDPIC
# is excluded from Buildroot's BR2_STRIP_strip option; strip only DWARF here.
define WIKIREADER_SYSTEM_SAVE_SYMBOLS
	python3 $(BR2_EXTERNAL_WIKIREADER_PATH)/../../debug/export.py \
		$(TARGET_DIR) $(BINARIES_DIR)/symbols $(TARGET_CROSS)strip
endef
TARGET_FINALIZE_HOOKS += WIKIREADER_SYSTEM_SAVE_SYMBOLS

$(eval $(generic-package))
