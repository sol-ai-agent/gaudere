AM_CPPFLAGS = -I$(top_srcdir)/include

lib_LTLIBRARIES = libgaudere.la libgaudere-persistence-sqlite.la
libgaudere_la_SOURCES = \
	scheduling/wake/Runtime.cpp \
	scheduling/wake/Scheduler.cpp \
	work/Runtime.cpp
libgaudere_la_LDFLAGS = -version-info 0:0:0

libgaudere_persistence_sqlite_la_SOURCES = \
	persistence/sqlite/ActionStore.cpp \
	persistence/sqlite/TaskStore.cpp
libgaudere_persistence_sqlite_la_LIBADD = libgaudere.la $(SQLITE3_LIBS)
libgaudere_persistence_sqlite_la_LDFLAGS = -version-info 0:0:0
