AM_CPPFLAGS = -I$(top_srcdir)/include

check_PROGRAMS = wake_scheduler_test wake_runtime_test
TESTS = $(check_PROGRAMS)

wake_scheduler_test_SOURCES = wake_scheduler_test.cpp
wake_scheduler_test_LDADD = $(top_builddir)/src/libgaudere.la

wake_runtime_test_SOURCES = wake_runtime_test.cpp
wake_runtime_test_LDADD = $(top_builddir)/src/libgaudere.la
