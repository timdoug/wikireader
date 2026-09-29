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
		-fdata-sections $(@D)/wr-selftest.c \
		-o $(@D)/wr-selftest $(TARGET_LDFLAGS) -Wl,--gc-sections
endef

# A fresh image's first boot credits a seed made here, so no boot waits for
# the kernel's random pool; rcS replaces it at once.  Whoever built the
# image knows this one seed, which costs a device with neither an MMU nor
# a network nothing.
define WIKIREADER_SYSTEM_SEED
	mkdir -p -m 0700 $(TARGET_DIR)/var/lib/seedrng
	head -c 256 /dev/urandom >$(TARGET_DIR)/var/lib/seedrng/seed.credit
	chmod 0600 $(TARGET_DIR)/var/lib/seedrng/seed.credit
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
	mkdir -p $(TARGET_DIR)/mnt/sd
	mkdir -p -m 0700 $(TARGET_DIR)/root
	$(WIKIREADER_SYSTEM_SEED)
endef

$(eval $(generic-package))
