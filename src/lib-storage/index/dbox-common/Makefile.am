noinst_LTLIBRARIES = libstorage_dbox_common.la

AM_CPPFLAGS = \
	-I$(top_srcdir)/src/lib \
	-I$(top_srcdir)/src/lib-settings \
	-I$(top_srcdir)/src/lib-fs \
	-I$(top_srcdir)/src/lib-mail \
	-I$(top_srcdir)/src/lib-imap \
	-I$(top_srcdir)/src/lib-index \
	-I$(top_srcdir)/src/lib-storage \
	-I$(top_srcdir)/src/lib-storage/index \
	-I$(top_srcdir)/src/lib-var-expand

libstorage_dbox_common_la_SOURCES = \
	dbox-attachment.c \
	dbox-file.c \
	dbox-file-fix.c \
	dbox-mail.c \
	dbox-save.c \
	dbox-storage.c

headers = \
	dbox-attachment.h \
	dbox-file.h \
	dbox-mail.h \
	dbox-save.h \
	dbox-storage.h

pkginc_libdir=$(pkgincludedir)
pkginc_lib_HEADERS = $(headers)
