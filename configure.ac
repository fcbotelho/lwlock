dnl Process this file with autoconf to produce a configure script.
AC_INIT
AC_CONFIG_SRCDIR([Makefile.am])
AM_INIT_AUTOMAKE(lwlock, 1.0)
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_MACRO_DIR([m4])

dnl Checks for programs.
AC_PROG_AWK
AC_PROG_CC
AC_PROG_INSTALL
AC_PROG_LN_S
LT_INIT
AC_SYS_EXTRA_LARGEFILE
if test "x$ac_cv_sys_largefile_CFLAGS" = "xno" ; then
	ac_cv_sys_largefile_CFLAGS=""
fi
if test "x$ac_cv_sys_largefile_LDFLAGS" = "xno" ; then
	ac_cv_sys_largefile_LDFLAGS=""
fi
if test "x$ac_cv_sys_largefile_LIBS" = "xno" ; then
	ac_cv_sys_largefile_LIBS=""
fi
CFLAGS="$ac_cv_sys_largefile_CFLAGS $CFLAGS"
LDFLAGS="$ac_cv_sys_largefile_LDFLAGS $LDFLAGS"
LIBS="$LIBS $ac_cv_sys_largefile_LIBS"

dnl Checks for headers
AC_CHECK_HEADERS([getopt.h math.h])

dnl Checks for libraries.
LT_LIB_M  
LDFLAGS="$LIBM $LDFLAGS"
#CFLAGS="-Wall -g"

dnl Detect target arch type
AC_ARG_VAR([SED_PROG], [sed utility])
AC_CHECK_PROGS([SED_PROG],[sed])
test -z "${SED_PROG}" && AC_MSG_ERROR([sed is required])
AC_MSG_CHECKING([output architecture])
x_x86_target=no
x_amd64_target=no
OUTPUT_ARCH="$(${CC} ${CPPFLAGS} ${CFLAGS} ${LDFLAGS} -Wl,--verbose 2>&1 | ${SED_PROG} -n 's/^OUTPUT_ARCH(\([[^)]]*\)).*/\1/p')"
if test -z "${OUTPUT_ARCH}"; then
	AC_MSG_RESULT([unknown])
else
	AC_MSG_RESULT([${OUTPUT_ARCH}])
	case "${OUTPUT_ARCH}" in
	i386)
		x_x86_target=yes
		;;
	*x86-64 | *x86_64 | *amd64)
		if test "$ac_cv_sizeof_unsigned_long" = 4; then
			x_x86_target=yes
		else
			x_amd64_target=yes
		fi
		;;
	esac
fi

dnl Handle --disable-asm, --enable-asm=x86, --enable-asm=amd64
AC_ARG_ENABLE([asm],
	[AS_HELP_STRING([--disable-asm],[disable optimized x86/amd64/padlock/intelaes assembler code])],
	[x_enable_asm=$enableval], [x_enable_asm=autodetect])

AC_MSG_CHECKING([whether to use optimized 32 bit x86 assembler code])
if test "$x_enable_asm" = x86 ; then
	x_enable_x86asm=yes
elif test "$x_enable_asm" = autodetect ; then
	x_enable_x86asm=$x_x86_target
else
	x_enable_x86asm=no
fi
AC_MSG_RESULT([${x_enable_x86asm}])

AC_MSG_CHECKING([whether to use optimized 64 bit amd64 assembler code])
if test "$x_enable_asm" = amd64 ; then
	x_enable_amd64asm=yes
elif test "$x_enable_asm" = autodetect ; then
	x_enable_amd64asm=$x_amd64_target
else
	x_enable_amd64asm=no
fi
AC_MSG_RESULT([${x_enable_amd64asm}])

dnl Select x86 asm / amd64 asm / C-language sources
if test "$x_enable_x86asm" = yes ; then
	AC_DEFINE([X86_ASM], [1])
elif test "$x_enable_amd64asm" = yes ; then
	AC_DEFINE([AMD64_ASM], [1])
else
	# dont use any assembler code
fi

# Unit tests based on the check library. Disabled by default.
# We do not use pkg-config because it is inconvenient for all developers to
# have check library installed.
AC_ARG_ENABLE(check, AS_HELP_STRING(
     [--enable-check],
     [Build unit tests depending on check library (default: disabled)]))
AS_IF([test "x$enable_check" = "xyes"],
  [ AC_CHECK_LIB([check], [tcase_create])
    AS_IF([test "$ac_cv_lib_check_tcase_create" = yes], [CHECK_LIBS="-lcheck"],
          [AC_MSG_ERROR("Failed to find check library (http://check.sf.net).")])
    AC_CHECK_HEADER(check.h,[],
          [AC_MSG_ERROR("Failed to find check library header (http://check.sf.net).")])
  ])
AM_CONDITIONAL([USE_LIBCHECK], [test "$ac_cv_lib_check_tcase_create" = yes])
AC_SUBST(CHECK_LIBS)
AC_SUBST(CHECK_CFLAGS)

AC_CHECK_SPOON
AC_CONFIG_FILES([Makefile src/Makefile tests/Makefile examples/Makefile man/Makefile lwlock.pc])
AC_OUTPUT
