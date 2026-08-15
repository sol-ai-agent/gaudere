AC_PREREQ([2.69])
AC_INIT([Gaudere], [0.1.0], [], [gaudere])
AC_CONFIG_SRCDIR([README.md])
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_MACRO_DIRS([m4])
AM_INIT_AUTOMAKE([foreign subdir-objects])
AC_LANG([C++])
AC_PROG_CXX
LT_INIT

CXXFLAGS="$CXXFLAGS -std=c++17"
AC_MSG_CHECKING([whether $CXX supports C++17])
AC_COMPILE_IFELSE(
    [AC_LANG_PROGRAM([], [[
#if __cplusplus < 201703L
#error "C++17 support is required"
#endif
    ]])],
    [AC_MSG_RESULT([yes])],
    [AC_MSG_FAILURE([a C++17-compatible compiler is required])])

AC_SEARCH_LIBS(
    [pthread_create],
    [pthread],
    [],
    [AC_MSG_FAILURE([POSIX threads support is required])])

AC_CONFIG_FILES([
    Makefile
    include/Makefile
    include/gaudere/Makefile
    include/gaudere/scheduling/Makefile
    include/gaudere/scheduling/wake/Makefile
    src/Makefile
    tests/Makefile
])
AC_OUTPUT
