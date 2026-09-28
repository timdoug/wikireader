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

# The diagnostics are freestanding and linked by the bare-metal compiler,
# which linux/toolchain.sh installs in the same prefix as this toolchain.
WIKIREADER_SYSTEM_ELF_CROSS = \
	$(call qstrip,$(BR2_TOOLCHAIN_EXTERNAL_PATH))/bin/c33-epson-elf-
# The diagnostics' libc check runs this ordinary uClibc program.
WIKIREADER_SYSTEM_SMOKE = $(BR2_EXTERNAL_WIKIREADER_PATH)/../../uclibc/smoke.c

define WIKIREADER_SYSTEM_BUILD_CMDS
	$(@D)/build-diag.sh $(WIKIREADER_SYSTEM_ELF_CROSS) $(@D)/out
	$(TARGET_CC) $(TARGET_CFLAGS) -fno-unwind-tables \
		-fno-asynchronous-unwind-tables -ffunction-sections \
		-fdata-sections $(WIKIREADER_SYSTEM_SMOKE) \
		-o $(@D)/out/uclibc-smoke $(TARGET_LDFLAGS) -Wl,--gc-sections \
		-Wl,-elf2flt=--shared-text
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
	$(INSTALL) -D -m 0755 $(@D)/rcS $(TARGET_DIR)/etc/init.d/rcS
	$(INSTALL) -D -m 0755 $(@D)/selftest $(TARGET_DIR)/etc/init.d/selftest
	$(INSTALL) -D -m 0755 $(@D)/late $(TARGET_DIR)/etc/init.d/late
	$(INSTALL) -D -m 0755 $(@D)/busybox-test \
		$(TARGET_DIR)/etc/init.d/busybox-test
	$(INSTALL) -m 0755 $(@D)/out/diag-init $(@D)/out/diag-test \
		$(@D)/out/child $(@D)/out/uclibc-smoke $(TARGET_DIR)/
	mkdir -p $(TARGET_DIR)/mnt/sd
	$(WIKIREADER_SYSTEM_SEED)
endef

$(eval $(generic-package))
