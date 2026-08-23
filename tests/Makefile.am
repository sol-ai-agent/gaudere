AM_CPPFLAGS = -I$(top_srcdir)/include

check_PROGRAMS = \
	wake_scheduler_test \
	wake_runtime_test \
	wake_intent_runtime_test \
	work_runtime_test \
	sqlite_action_store_test \
	sqlite_budget_store_test \
	sqlite_task_store_test \
	sqlite_wake_intent_store_test \
	sqlite_wake_intent_scope_inspection_test
TESTS = $(check_PROGRAMS)

wake_scheduler_test_SOURCES = wake_scheduler_test.cpp
wake_scheduler_test_LDADD = $(top_builddir)/src/libgaudere.la

wake_runtime_test_SOURCES = wake_runtime_test.cpp
wake_runtime_test_LDADD = $(top_builddir)/src/libgaudere.la

wake_intent_runtime_test_SOURCES = wake_intent_runtime_test.cpp
wake_intent_runtime_test_LDADD = $(top_builddir)/src/libgaudere.la

work_runtime_test_SOURCES = work_runtime_test.cpp
work_runtime_test_LDADD = $(top_builddir)/src/libgaudere.la

sqlite_action_store_test_SOURCES = sqlite_action_store_test.cpp
sqlite_action_store_test_LDADD = \
	$(top_builddir)/src/libgaudere-persistence-sqlite.la \
	$(top_builddir)/src/libgaudere.la \
	$(SQLITE3_LIBS)

sqlite_budget_store_test_SOURCES = sqlite_budget_store_test.cpp
sqlite_budget_store_test_LDADD = \
	$(top_builddir)/src/libgaudere-persistence-sqlite.la \
	$(top_builddir)/src/libgaudere.la \
	$(SQLITE3_LIBS)

sqlite_task_store_test_SOURCES = sqlite_task_store_test.cpp
sqlite_task_store_test_LDADD = \
	$(top_builddir)/src/libgaudere-persistence-sqlite.la \
	$(top_builddir)/src/libgaudere.la \
	$(SQLITE3_LIBS)

sqlite_wake_intent_store_test_SOURCES = sqlite_wake_intent_store_test.cpp
sqlite_wake_intent_store_test_LDADD = \
	$(top_builddir)/src/libgaudere-persistence-sqlite.la \
	$(top_builddir)/src/libgaudere.la \
	$(SQLITE3_LIBS)

sqlite_wake_intent_scope_inspection_test_SOURCES = \
	sqlite_wake_intent_scope_inspection_test.cpp
sqlite_wake_intent_scope_inspection_test_LDADD = \
	$(top_builddir)/src/libgaudere-persistence-sqlite.la \
	$(top_builddir)/src/libgaudere.la \
	$(SQLITE3_LIBS)
