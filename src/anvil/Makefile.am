pkglibexecdir = $(libexecdir)/dovecot

pkglibexec_PROGRAMS = anvil

AM_CPPFLAGS = \
	-I$(top_srcdir)/src/lib \
	-I$(top_srcdir)/src/lib-test \
	-I$(top_srcdir)/src/lib-settings \
	-I$(top_srcdir)/src/lib-master \
	-I$(top_srcdir)/src/lib-var-expand \
	$(BINARY_CFLAGS)

anvil_LDADD = \
	$(LIBDOVECOT) \
	$(RAND_LIBS) \
	$(BINARY_LDFLAGS)

anvil_DEPENDENCIES = $(LIBDOVECOT_DEPS)

anvil_SOURCES = \
	main.c \
	admin-client.c \
	admin-client-pool.c \
	anvil-connection.c \
	anvil-settings.c \
	connect-limit.c \
	penalty.c

noinst_HEADERS = \
	admin-client.h \
	admin-client-pool.h \
	anvil-connection.h \
	common.h \
	connect-limit.h \
	penalty.h

test_programs = \
	test-connect-limit \
	test-penalty

noinst_PROGRAMS = $(test_programs)

test_libs = \
	../lib-test/libtest.la \
	../lib/liblib.la

test_connect_limit_SOURCES = test-connect-limit.c
test_connect_limit_LDADD = connect-limit.o $(test_libs)
test_connect_limit_DEPENDENCIES = $(pkglibexec_PROGRAMS) $(test_libs)

test_penalty_SOURCES = test-penalty.c
test_penalty_LDADD = penalty.o $(test_libs)
test_penalty_DEPENDENCIES = $(pkglibexec_PROGRAMS) $(test_libs)

check-local:
	for bin in $(test_programs); do \
	  if ! $(RUN_TEST) ./$$bin; then exit 1; fi; \
	done
