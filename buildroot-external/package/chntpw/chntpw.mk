################################################################################
#
# chntpw
#
################################################################################

CHNTPW_VERSION = 140201
CHNTPW_SOURCE = chntpw-source-$(CHNTPW_VERSION).zip
CHNTPW_SITE = https://pogostick.net/~pnh/ntpasswd
CHNTPW_LICENSE = GPL-2.0, LGPL-2.0+
CHNTPW_LICENSE_FILES = GPL.txt LGPL.txt

CHNTPW_CFLAGS = $(TARGET_CFLAGS) -Os -I$(@D) -Wall -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64

define CHNTPW_EXTRACT_CMDS
	unzip -q $(CHNTPW_DL_DIR)/$(CHNTPW_SOURCE) -d $(@D)
	if [ ! -f $(@D)/chntpw.c ]; then \
		srcdir="$$(find $(@D) -mindepth 1 -maxdepth 1 -type d | head -n 1)"; \
		if [ -n "$$srcdir" ] && [ -f "$$srcdir/chntpw.c" ]; then \
			cp -a "$$srcdir"/. $(@D)/; \
		fi; \
	fi
endef

define CHNTPW_BUILD_CMDS
	$(TARGET_CC) $(CHNTPW_CFLAGS) -c -o $(@D)/ntreg.o $(@D)/ntreg.c
	$(TARGET_CC) $(CHNTPW_CFLAGS) -c -o $(@D)/edlib.o $(@D)/edlib.c
	$(TARGET_CC) $(CHNTPW_CFLAGS) -c -o $(@D)/libsam.o $(@D)/libsam.c
	$(TARGET_CC) $(CHNTPW_CFLAGS) -c -o $(@D)/chntpw.o $(@D)/chntpw.c
	$(TARGET_CC) $(CHNTPW_CFLAGS) -c -o $(@D)/reged.o $(@D)/reged.c
	$(TARGET_CC) $(CHNTPW_CFLAGS) -c -o $(@D)/sampasswd.o $(@D)/sampasswd.c
	$(TARGET_CC) $(CHNTPW_CFLAGS) -c -o $(@D)/samusrgrp.o $(@D)/samusrgrp.c
	$(TARGET_CC) $(TARGET_LDFLAGS) -s -o $(@D)/chntpw $(@D)/chntpw.o $(@D)/ntreg.o $(@D)/edlib.o $(@D)/libsam.o
	$(TARGET_CC) $(TARGET_LDFLAGS) -s -o $(@D)/reged $(@D)/reged.o $(@D)/ntreg.o $(@D)/edlib.o
	$(TARGET_CC) $(TARGET_LDFLAGS) -s -o $(@D)/sampasswd $(@D)/sampasswd.o $(@D)/ntreg.o $(@D)/libsam.o
	$(TARGET_CC) $(TARGET_LDFLAGS) -s -o $(@D)/samusrgrp $(@D)/samusrgrp.o $(@D)/ntreg.o $(@D)/libsam.o
endef

define CHNTPW_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/chntpw $(TARGET_DIR)/usr/sbin/chntpw
	$(INSTALL) -D -m 0755 $(@D)/reged $(TARGET_DIR)/usr/sbin/reged
	$(INSTALL) -D -m 0755 $(@D)/sampasswd $(TARGET_DIR)/usr/sbin/sampasswd
	$(INSTALL) -D -m 0755 $(@D)/samusrgrp $(TARGET_DIR)/usr/sbin/samusrgrp
endef

$(eval $(generic-package))
