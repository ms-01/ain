dnl Copyright (c) 2026 The DeFi Blockchain developers
dnl Distributed under the MIT software license, see the accompanying
dnl file LICENSE or http://www.opensource.org/licenses/mit-license.php.

dnl DEFI_FIND_SQLITE
dnl --------------
dnl Detect the SQLite library that is used for the additional wallet database
dnl backend. SQLite is optional: if it cannot be found the wallet only supports
dnl the legacy Berkeley DB backend.
dnl
dnl Defines: SQLITE_CPPFLAGS, SQLITE_LIBS and the automake conditional
dnl ENABLE_SQLITE as well as the preprocessor define USE_SQLITE.

AC_DEFUN([DEFI_FIND_SQLITE],[
  AC_ARG_VAR(SQLITE_CFLAGS, [C compiler flags for SQLite, bypasses autodetection])
  AC_ARG_VAR(SQLITE_LIBS, [Linker flags for SQLite, bypasses autodetection])

  AC_ARG_WITH([sqlite],
    AS_HELP_STRING([--with-sqlite@<:@=yes|no|auto@:>@],
      [use SQLite as an additional wallet database backend (default: auto-detect)]),
    [use_sqlite=$withval],
    [use_sqlite=auto])

  SQLITE_CPPFLAGS=""
  SQLITE_LIBS=""
  have_sqlite=no

  if test "x$enable_wallet" != "xno"; then
    if test "x$use_sqlite" != "xno"; then
      if test "x$SQLITE_CFLAGS" != "x" || test "x$SQLITE_LIBS" != "x"; then
        have_sqlite=yes
      else
        AC_CHECK_HEADERS([sqlite3.h], [
          AC_CHECK_LIB([sqlite3], [sqlite3_open_v2], [
            SQLITE_LIBS="-lsqlite3 -lpthread -ldl -lm"
            have_sqlite=yes
          ], [have_sqlite=no])
        ], [have_sqlite=no])
      fi
    fi

    if test "x$have_sqlite" = "xno"; then
      if test "x$use_sqlite" = "xyes"; then
        AC_MSG_ERROR([libsqlite3 missing, ]AC_PACKAGE_NAME[ requires this library for the SQLite wallet backend (--without-sqlite to build without it)])
      fi
      AC_MSG_WARN([SQLite wallet backend disabled (libsqlite3 not found)])
    fi
  fi

  if test "x$have_sqlite" = "xyes"; then
    use_sqlite=yes
    AC_DEFINE([USE_SQLITE], [1], [Define if the wallet supports the SQLite database backend])
  else
    use_sqlite=no
  fi

  AC_SUBST(SQLITE_CPPFLAGS)
  AC_SUBST(SQLITE_LIBS)
  AM_CONDITIONAL([ENABLE_SQLITE], [test x$use_sqlite = xyes])
])
