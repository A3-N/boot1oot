################################################################################
#
# dislocker
#
################################################################################

DISLOCKER_VERSION = 4ff070f0ea9e56948ab316fb76b91f54dd6727aa
DISLOCKER_SITE = https://github.com/Aorimn/dislocker.git
DISLOCKER_SITE_METHOD = git
DISLOCKER_LICENSE = GPL-2.0+
DISLOCKER_LICENSE_FILES = LICENSE.txt
DISLOCKER_DEPENDENCIES = host-pkgconf libfuse3 mbedtls

DISLOCKER_CONF_OPTS = \
	-DWARN_FLAGS="-Wall -Wextra" \
	-DWITH_RUBY=OFF \
	-DWITH_FUSE=ON \
	-DFUSE_INCLUDE_DIRS="$(STAGING_DIR)/usr/include/fuse3" \
	-DFUSE_LIBRARIES="$(STAGING_DIR)/usr/lib/libfuse3.so" \
	-DMbedTLS_DIR="$(STAGING_DIR)/usr/lib/cmake/MbedTLS" \
	-DMBEDTLS_INCLUDE_DIR="$(STAGING_DIR)/usr/include" \
	-DMBEDTLS_LIBRARY="$(STAGING_DIR)/usr/lib/libmbedtls.so" \
	-DMBEDX509_LIBRARY="$(STAGING_DIR)/usr/lib/libmbedx509.so" \
	-DMBEDCRYPTO_LIBRARY="$(STAGING_DIR)/usr/lib/libmbedcrypto.so" \
	-DCMAKE_BUILD_TYPE=MinSizeRel

define DISLOCKER_INSTALL_TARGET_CMDS
	mkdir -p $(TARGET_DIR)/usr/lib
	if [ -d "$(@D)/buildroot-build/src" ]; then builddir="$(@D)/buildroot-build/src"; else builddir="$(@D)/src"; fi; \
	cp -a "$$builddir"/libdislocker.so* $(TARGET_DIR)/usr/lib/; \
	$(INSTALL) -D -m 0755 "$$builddir"/dislocker-fuse $(TARGET_DIR)/usr/sbin/dislocker-fuse; \
	$(INSTALL) -D -m 0755 "$$builddir"/dislocker-metadata $(TARGET_DIR)/usr/sbin/dislocker-metadata
endef

$(eval $(cmake-package))
