################################################################################
#
# boot1oot
#
################################################################################

BOOT1OOT_VERSION = 0.1
BOOT1OOT_SITE = $(BR2_EXTERNAL_BOOT1OOT_PATH)/../src
BOOT1OOT_SITE_METHOD = local

BOOT1OOT_CPPFLAGS =
ifeq ($(BOOT1OOT_EXPORT_CONFIG),1)
ifneq ($(strip $(BOOT1OOT_BITLOCKER_RECOVERY_KEY)),)
BOOT1OOT_CPPFLAGS += -DBOOT1OOT_BUILTIN_BITLOCKER_RECOVERY_KEY=\"$(BOOT1OOT_BITLOCKER_RECOVERY_KEY)\"
endif
endif

define BOOT1OOT_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -Os -s \
		$(BOOT1OOT_CPPFLAGS) \
		-o $(@D)/boot1oot $(@D)/boot1oot.c
endef

define BOOT1OOT_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/boot1oot $(TARGET_DIR)/usr/sbin/boot1oot
endef

$(eval $(generic-package))
