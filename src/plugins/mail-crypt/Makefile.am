AM_CPPFLAGS = \
	-I$(top_srcdir)/src/lib \
	-I$(top_srcdir)/src/lib-test \
	-I$(top_srcdir)/src/lib-settings \
	-I$(top_srcdir)/src/lib-master \
	-I$(top_srcdir)/src/lib-mail \
	-I$(top_srcdir)/src/lib-dict \
	-I$(top_srcdir)/src/lib-index \
	-I$(top_srcdir)/src/lib-storage/index \
	-I$(top_srcdir)/src/lib-storage \
	-I$(top_srcdir)/src/lib-dcrypt \
	-I$(top_srcdir)/src/lib-fs \
	-I$(top_srcdir)/src/lib-doveadm \
	-I$(top_srcdir)/src/lib-var-expand \
	-I$(top_srcdir)/src/doveadm \
	-I$(top_srcdir)/src/plugins/acl

doveadm_moduledir = $(moduledir)/doveadm

NOPLUGIN_LDFLAGS =

module_LTLIBRARIES = \
	lib10_mail_crypt_plugin.la \
	lib05_mail_crypt_acl_plugin.la \
	libfs_crypt.la

doveadm_module_LTLIBRARIES = \
	libdoveadm_mail_crypt_plugin.la

lib10_mail_crypt_plugin_la_LDFLAGS = -module -avoid-version
lib10_mail_crypt_plugin_la_LIBADD = \
	$(LIBDCRYPT_LIBS) \
	$(LIBDOVECOT)

lib05_mail_crypt_acl_plugin_la_LDFLAGS = -module -avoid-version
if DOVECOT_PLUGIN_DEPS
lib05_mail_crypt_acl_plugin_la_LIBADD = \
	$(LIBDCRYPT_LIBS) \
	lib10_mail_crypt_plugin.la
endif

lib10_mail_crypt_plugin_la_SOURCES = \
	crypt-settings.c \
	mail-crypt-global-key.c \
	mail-crypt-key.c \
	mail-crypt-plugin.c

lib05_mail_crypt_acl_plugin_la_SOURCES = \
	mail-crypt-acl-plugin.c

libfs_crypt_la_SOURCES = fs-crypt.c \
	crypt-settings.c \
	mail-crypt-global-key.c

libfs_crypt_la_LIBADD = $(LIBDOVECOT)
libfs_crypt_la_DEPENDENCIES = $(LIBDOVECOT_DEPS)
libfs_crypt_la_LDFLAGS = -module -avoid-version

libdoveadm_mail_crypt_plugin_la_SOURCES = \
	doveadm-mail-crypt.c
libdoveadm_mail_crypt_plugin_la_LIBADD = $(LIBDOVECOT)
libdoveadm_mail_crypt_plugin_la_DEPENDENCIES = $(LIBDOVECOT_DEPS)
libdoveadm_mail_crypt_plugin_la_LDFLAGS = -module -avoid-version

test_programs = \
	test-fs-crypt \
	test-mail-global-key \
	test-mail-key

test_fs_crypt_SOURCES = \
	test-fs-crypt.c
test_fs_crypt_LDADD = $(LIBDOVECOT) \
	crypt-settings.lo \
	fs-crypt.lo \
	mail-crypt-global-key.lo
test_fs_crypt_DEPENDENCIES = $(LIBDOVECOT_DEPS) \
	crypt-settings.lo \
	fs-crypt.lo \
	mail-crypt-global-key.lo
test_fs_crypt_LDFLAGS = $(DOVECOT_BINARY_LDFLAGS)
test_fs_crypt_CFLAGS = $(AM_CFLAGS) $(DOVECOT_BINARY_CFLAGS) -Dtop_builddir=\"$(top_builddir)\"

test_mail_global_key_SOURCES = \
	test-mail-global-key.c \
	crypt-settings.c \
	mail-crypt-global-key.c
test_mail_global_key_LDADD = $(LIBDOVECOT)
test_mail_global_key_DEPENDENCIES = $(LIBDOVECOT_DEPS)
test_mail_global_key_LDFLAGS = $(DOVECOT_BINARY_LDFLAGS)
test_mail_global_key_CFLAGS = $(AM_CFLAGS) $(DOVECOT_BINARY_CFLAGS) -Dtop_builddir=\"$(top_builddir)\"

test_mail_key_SOURCES = \
	test-mail-key.c \
	crypt-settings.c \
	mail-crypt-key.c \
	mail-crypt-global-key.c

test_mail_key_LDADD = $(LIBDOVECOT_STORAGE) $(LIBDOVECOT)
test_mail_key_DEPENDENCIES = $(LIBDOVECOT_DEPS)  $(LIBDOVECOT_STORAGE_DEPS)
test_mail_key_LDFLAGS = $(DOVECOT_BINARY_LDFLAGS)
test_mail_key_CFLAGS = $(AM_CFLAGS) $(DOVECOT_BINARY_CFLAGS) -Dtop_builddir=\"$(top_builddir)\"

noinst_HEADERS = \
	crypt-settings.h \
	mail-crypt-plugin.h \
	mail-crypt-common.h \
	mail-crypt-global-key.h \
	mail-crypt-key.h

check-local:
	for bin in $(test_programs); do \
	  if ! $(RUN_TEST) ./$$bin; then exit 1; fi; \
	done

noinst_PROGRAMS = $(test_programs)
