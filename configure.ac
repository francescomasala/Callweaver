# Main Autoconf configuration file for the OpnePBX.org project
# Copyright <c> 2005 by OpenPBX org
# For a list of Authors check the AUTHORS file in the same directory
# as this file
# Work on the automake stuff was partly sponsored by Halo Kwadrat Sp. z o.o.
#
#       This program is free software; you can redistribute it and/or
#       modify it under the terms of the GNU General Public License
#       as published by the Free Software Foundation; either version
#       2 of the License, or (at your option) any later version.     
# 
# Process this file with autoconf to produce a configure script.
#
######
#
# initialisation of the GNU AutoTools Packages
#
######
AC_PREREQ(2.59)
AC_INIT(OpenPBX.org,1.2-RC3,openpbx-dev@openpbx.org)
AM_INIT_AUTOMAKE(OpenPBX.org,1.2-RC3)
dnl define GNU Source to make sure every compiler and tool 
dnl  knows that we use GNU compliant macros etc
AC_GNU_SOURCE
AC_LANG_C
AC_C_CONST

######
#
# Define and check for prerequisite programs
#
######
AC_PATH_PROG([GROUPADD], [groupadd])
AC_PATH_PROGS(GREP, ggrep grep,[echo Missing grep so skipping but I doubt we will get anywhere])
AC_PATH_PROGS(SVN, svn, [echo Missing subversion so some stuff will be borked],${PATH}:/opt/csw/bin)
AC_PATH_PROGS(SHELL,bash sh,[echo No compatible shell found])
AC_PATH_PROGS(SH,bash sh,[echo No compatible shell found])
AC_PATH_PROGS(M4,gm4 m4,[echo No m4 found, who will process my macros now ?])
AC_PATH_PROGS(SED,gsed sed,[echo sed not found, doh!])
AC_PATH_PROGS(UNAME,uname,[echo uname not found so no version info will be available])
AC_PROG_CXX
AC_PROG_YACC
AC_PROG_CC
AM_PROG_CC_C_O
AC_PROG_CPP
AM_PROG_LEX
AX_SYS_PERLSHARPBANG
AC_PROG_INSTALL

dnl SVN Revision number, needed for OPBX_VERSION and inclusion into files version number
if test -n "$SVN" ; then
	AC_DEFINE_UNQUOTED([SVN_VERSION],"`${SVN} info | ${GREP} Revision | ${AWK} '{ print $2 }'`",[Which Subversion revision are we building here ?] )
	AC_SUBST([SVN_VERSION])
else
	AC_DEFINE_UNQUOTED([SVN_VERSION],"UNKNOWN",[Unknown Subversion because it's not on this machine.] )
	AC_SUBST([SVN_VERSION])
fi   

dnl ###########################################################################
dnl ###########################################################################
dnl ###
dnl ### Libtool definitions
dnl ###
dnl ###########################################################################
dnl ###########################################################################

dnl check for dlopen support (for shared modules)
AC_LIBTOOL_DLOPEN
AC_LIBLTDL_CONVENIENCE
dnl yes we will use libtool to create cygwin shared modules
AC_LIBTOOL_WIN32_DLL
dnl Disable static libraries by default. Can be enabled with --enable-static
AC_DISABLE_STATIC
AC_PROG_LIBTOOL   
AC_SUBST(INCLTDL)
AC_SUBST(LIBLTDL)

dnl ###########################################################################
dnl ###########################################################################
dnl ###
dnl ### OS Dependent Stuff
dnl ###
dnl ###########################################################################
dnl ###########################################################################

dnl Shamelessly stolen from editline configure.in to create the relevant cygwin stuff
dnl Platform-specific settings.  
case "${host}" in
  *-*-darwin*)
        AC_DEFINE([__Darwin__],,[Define if Darwin])
        AC_SUBST(__Darwin__)
	no_libcap=yes
        ;;
  *-*-freebsd*)
	no_libcap=yes
        ;;
  *-aix*)
    AC_DEFINE(AIX,,[Define if AIX])
     broken_types=yes
	no_libcap=yes
    ;;
  *-osf4*)
    AC_DEFINE(OSF1,,[Define if OSF1])
    tru64_types=yes
	no_libcap=yes
    ;;
  *-osf5.1*)
    AC_DEFINE(OSF1)
	no_libcap=yes
    ;;
  *-tru64*)
    AC_DEFINE(OSF1)
    tru64_types=yes
    no_libcap=yes
    ;;
  *-*-linux* )
        ;;
  *cygwin*)
        AC_DEFINE([_WIN32_WINNT],[0x0500],[Heya, it's windows])
        AC_DEFINE([INET_ADDRSTRLEN],[16],[cygwin detection does not work for that, anybody has an idea ?])
        AC_DEFINE([INET6_ADDRSTRLEN],[46],[cygwin detection does not work for that, anybody has an idea ?])
	AC_DEFINE([__CYGWIN__],[1],[make sure __CYGWIN__ is defined ...])
        ;;
  *-*-netbsd*)
	no_libcap=yes
        ;;
  *-*-solaris2*)
        AC_DEFINE([SOLARIS],[1],[needed for optional declarations to be visible])
	no_libcap=yes
        ;;
  *)
        AC_MSG_RESULT(Unsupported operating system: ${host})
	no_libcap=yes
        ;;
esac  

######
#
# Build paths and variables
#
######     


dnl Create the relevant Build variables

if test -n $BUILD_OS ; then
	AC_DEFINE_UNQUOTED([BUILD_DATE],"`date -u "+%Y-%m-%d %H:%M:%S"` UTC",[The date of this build])
	AC_DEFINE_UNQUOTED([BUILD_OS],"`${UNAME} -s`",[Operating System we are building on])
	AC_DEFINE_UNQUOTED([BUILD_MACHINE],"`${UNAME} -m`",[Machine we are building on])
	AC_DEFINE_UNQUOTED([BUILD_HOSTNAME],"`${UNAME} -n`",[Hostname of our Box])
	AC_DEFINE_UNQUOTED([BUILD_KERNEL],"`${UNAME} -r`",[Kernel version of this build])
	AC_SUBST([BUILD_DATE])
	AC_SUBST([BUILD_OS])
	AC_SUBST([BUILD_MACHINE])
	AC_SUBST([BUILD_HOSTNAME])
	AC_SUBST([BUILD_KERNEL])
fi

dnl check for host compiler when cross compiling
if test "$host" = "$build"; then
	HOST_CC="${CC}"
else
	HOST_CC="${HOST_CC-gcc}"
fi
AC_CHECK_PROG(have_host_cc, ${HOST_CC}, yes, no)
if test "$have_host_cc" = "no"; then
	AC_MSG_ERROR(No valid host compiler set with HOST_CC)
fi
AC_SUBST(HOST_CC)

dnl Installation directories
dnl Prefer modern Filesystem Hierarchy Standard over antiquated GNU standards.

AC_ARG_WITH([directory-layout],     
	[AC_HELP_STRING([--with-directory-layout],[Choose installation directory layout: singledir, lsb, fhs (default=singledir).])],,[with_directory_layout="singledir"])

case "${with_directory_layout}" in
     singledir)
	AC_MSG_NOTICE([You have chosen the SINGLEDIR directory layout...])
	ac_default_prefix='/opt/openpbx.org'
	;;
     lsb)
	AC_MSG_NOTICE([You have chosen the LSB directory layout...])
	ac_default_prefix='/usr/local'
        ;;
     fhs)
	AC_MSG_NOTICE([You have chosen the FHS directory layout...])
	ac_default_prefix='/opt/openpbx.org'
        ;;
     *)
	AC_MSG_ERROR([Unknown directory layout ${with_directory_layout}])
	;;
esac

if test -z "${opbxconfdir}"; then
   case "${with_directory_layout}" in
	singledir)
	   opbxconfdir='${prefix}/etc'
	   ;;
	lsb)
	   opbxconfdir='${sysconfdir}/openpbx.org'
	   ;;
        fhs)
           opbxconfdir='/etc${prefix}'
   esac
fi
AC_SUBST([opbxconfdir])

if test -z "${opbxconffile}"; then
	opbxconffile='${opbxconfdir}/openpbx.conf'
fi
AC_SUBST([opbxconffile])

if test -z "${opbxlibdir}"; then
   case "${with_directory_layout}" in
	singledir)
	   opbxlibdir='${libdir}'
	   ;;
	lsb)
	   opbxlibdir='${libdir}/openpbx.org'
	   ;;
        fhs)
           opbxlibdir='${libdir}'
           ;;
   esac
fi
AC_SUBST([opbxlibdir])

if test -z "${opbxmoddir}"; then
	opbxmoddir='${opbxlibdir}/modules'
fi
AC_SUBST([opbxmoddir])

if test -z "${opbxicdmoddir}"; then
	opbxicdmoddir='${opbxmoddir}/icd'
fi
AC_SUBST([opbxicdmoddir])

if test -z "${opbxvardir}"; then
   case "${with_directory_layout}" in
	singledir)
	   opbxvardir='${prefix}/var'
	   ;;
	lsb)
	   opbxvardir='${localstatedir}/lib/openpbx.org'
	   ;;
        fhs)
           opbxvardir='/var${prefix}'
           ;;
   esac
fi
AC_SUBST([opbxvardir])

if test -z "${opbxdbdir}"; then
	opbxdbdir='${opbxvardir}'
fi
AC_SUBST([opbxdbdir])

if test -z "${opbxdbfile}"; then
	opbxdbfile='opbxdb'
fi
AC_SUBST([opbxdbfile])

if test -z "${opbxtmpdir}"; then
	opbxtmpdir='${opbxvardir}/tmp'
fi
AC_SUBST([opbxtmpdir])

if test -z "${opbxrundir}"; then
   case "${with_directory_layout}" in
	singledir)
	   opbxrundir='${prefix}/run'
	   ;;
	lsb)
	   opbxrundir='${localstatedir}/run/openpbx.org'
	   ;;
        fhs)
           opbxrundir='${opbxvardir}/run'
           ;;
   esac
fi
AC_SUBST([opbxrundir])

if test -z "${opbxpidfile}"; then
	opbxpidfile='${opbxrundir}/openpbx.pid'
fi
AC_SUBST([opbxpidfile])

if test -z "${opbxsocketfile}"; then
	opbxsocketfile='${opbxrundir}/openpbx.ctl'
fi
AC_SUBST([opbxsocketfile])

if test -z "${opbxlogdir}"; then
   case "${with_directory_layout}" in
	singledir)
	   opbxlogdir='${prefix}/log'
	   ;;
	lsb)
	   opbxlogdir='${localstatedir}/log/openpbx.org'
	   ;;
        fhs)
           opbxlogdir='${opbxvardir}/log'
           ;;
   esac
fi
AC_SUBST([opbxlogdir])

if test -z "${opbxspooldir}"; then
   case "${with_directory_layout}" in
	singledir)
	   opbxspooldir='${opbxvardir}/spool'
	   ;;
	lsb)
	   opbxspooldir='${localstatedir}/spool/openpbx.org'
	   ;;
        fhs)
           opbxspooldir='${opbxvardir}/spool'
           ;;
   esac
fi
AC_SUBST([opbxspooldir])

if test -z "${opbxdatadir}"; then
   case "${with_directory_layout}" in
	singledir)
	   opbxdatadir='${opbxvardir}/data'
	   ;;
	lsb)
	   opbxdatadir='${datadir}/openpbx.org'
	   ;;
        fhs)
           opbxdatadir='${prefix}/share'
           ;;
   esac
fi
AC_SUBST([opbxdatadir])

if test -z "${opbxdocdir}"; then
	opbxdocdir='${opbxdatadir}/doc'
fi
AC_SUBST([opbxdocdir])

if test -z "${opbxkeydir}"; then
	opbxkeydir='${opbxdatadir}/keys'
fi
AC_SUBST([opbxkeydir])

if test -z "${opbxsqlitedir}"; then
	opbxsqlitedir='${opbxdbdir}'
fi
AC_SUBST([opbxsqlitedir])

if test -z "${opbxogidir}"; then
	opbxogidir='${opbxdatadir}/ogi'
fi
AC_SUBST([opbxogidir])

if test -z "${opbxsoundsdir}"; then
	opbxsoundsdir='${opbxdatadir}/sounds'
fi
AC_SUBST([opbxsoundsdir])

if test -z "${opbximagesdir}"; then
	opbximagesdir='${opbxdatadir}/images'
fi
AC_SUBST([opbximagesdir])

if test -z "${opbxmohdir}"; then
	opbxmohdir='${opbxdatadir}/moh'
fi
AC_SUBST([opbxmohdir])

if test -z "${opbxincludedir}"; then
	opbxincludedir='${includedir}/openpbx'
fi
AC_SUBST([opbxincludedir])

######
#
# Check for headers, structures, typedefs, libs etc that are not realted to specific add on packages
#
######
AC_ARG_ENABLE([builtin-sqlite3],
        [AC_HELP_STRING([--enable-builtin-sqlite3],[Enables the builtin thread-safe copy of SQLite library (default=yes)])],[builtin_sqlite3="$enableval"],[builtin_sqlite3="yes"])


dnl check for required libraries (also for addons)
dnl needed to move check for sqlite threadsafe here since it needs to check if we have a threadsafe sqlite3 BEFORE we check for libpthread)
if test "${builtin_sqlite3}" = "no"; then
	AC_CHECK_SQLITE3_THREADSAFE
fi
AC_CHECK_LIB([c], [main])
AC_CHECK_LIB([db], [main])
dnl # AC_CHECK_LIB([expat], [main]) # why would we need this one ?
AC_CHECK_LIB([ib], [main])
AC_CHECK_LIB([m], [main])
AC_CHECK_LIB([ncurses], [main],,[AC_CHECK_LIB([curses],[main])])
AC_CHECK_LIB([newt], [main],[AC_DEFINE([HAVE_LIBNEWT],["yes"],[yes there is newt])])
AM_CONDITIONAL([USE_NEWT],[test -n "${HAVE_LIBNEWT}"])
AC_CHECK_LIB([nsl], [main])
AC_CHECK_LIB([ossaudio], [main])
AC_CHECK_LIB([pthread], [main])
AC_CHECK_LIB([resolv], [main])
AC_CHECK_LIB([rt], [main])
AC_CHECK_LIB([socket], [main])
AC_CHECK_LIB([t], [main])
# AC_CHECK_LIB([termcap], [main])


dnl checks for library functions
AC_FUNC_ALLOCA
AC_CHECK_FUNC(usleep, [TARGET_CFLAGS="$TARGET_CFLAGS -DHAVE_USLEEP=1"])
AC_FUNC_CHOWN
AC_FUNC_CLOSEDIR_VOID
AC_FUNC_ERROR_AT_LINE
AC_FUNC_FORK
AC_PROG_GCC_TRADITIONAL
AC_FUNC_MALLOC
AC_FUNC_MEMCMP
AC_FUNC_MMAP
AC_FUNC_REALLOC
AC_FUNC_SELECT_ARGTYPES
AC_TYPE_SIGNAL
AC_FUNC_STAT
AC_FUNC_STRCOLL
AC_FUNC_STRFTIME
AC_FUNC_STRNLEN
AC_FUNC_STRTOD
AC_FUNC_UTIME_NULL
AC_FUNC_VPRINTF
AC_CHECK_FUNCS([atexit bzero dup2 endpwent floor ftruncate getcwd gethostbyname gethostname gettimeofday clock_getres inet_ntoa isascii localtime_r memchr memmove memset mkdir munmap pow putenv re_comp regcomp rint select setenv socket sqrt strsep strcasecmp strchr strcspn strdup strerror strerror_r strncasecmp strndup strrchr strspn strstr strtol strtoq unsetenv utime vasprintf])  
dnl Checks for header files.
AC_HEADER_DIRENT
AC_HEADER_STDC
AC_HEADER_STDBOOL
AC_HEADER_SYS_WAIT
AC_CHECK_HEADERS([netinet/in.h])
dnl This does not work currently .. some bug in cygwin autoconf
dnl AC_CHECK_HEADERS([w32api/windows.h])
dnl AC_CHECK_HEADERS([w32api/winsock2.h],[],[],
dnl [#if HAVE_W32API_WINDOWS_H
dnl # include <w32api/windows.h>
dnl # endif
dnl ])
dnl AC_CHECK_HEADERS([w32api/ws2tcpip.h],[],[],
dnl [#if HAVE_W32API_WINSOCK2_H
dnl # include <w32api/windows.h>
dnl # include <w32api/winsock2.h>
dnl # endif
dnl ])
AC_CHECK_HEADER([dlfcn.h],[AM_CONDITIONAL([NEED_DLFCN_H],[true = yes])])       

dnl Checks for structures
AC_STRUCT_TM
AC_STRUCT_TIMEZONE
AC_CHECK_MEMBERS([struct stat.st_blksize])  

dnl compiler specifics
AC_C_INLINE
AC_C_CONST
AC_C_VOLATILE

dnl Check for types
AC_TYPE_UID_T
AC_TYPE_MODE_T
AC_TYPE_OFF_T
AC_TYPE_PID_T
AC_TYPE_SIZE_T  
AC_CHECK_TYPES([ptrdiff_t])  

dnl Checks for declarations
AC_CHECK_DECLS(INET_ADDRSTRLEN,[],[],[#if HAVE_NETINET_IN_H
# include <netinet/in.h>
# endif
#if HAVE_W32API_WS32TCPIP_H
# include w32api/windows.h
# include w32api/winsock2.h  
# include w32api/ws2tcpip.h
#endif
])
AC_CHECK_DECLS(INET6_ADDRSTRLEN,[],[],[#if HAVE_NETINET_IN_H
# include <netinet/in.h>
# endif
#if HAVE_W32API_WS32TCPIP_H
# include w32api/windows.h
# include w32api/winsock2.h
# include w32api/ws2tcpip.h
#endif
])   
AC_CHECK_DECLS(__BEGIN_DECLS,[],AC_DEFINE(__BEGIN_DECLS,,[for stupid ast-db1]))
AC_CHECK_DECLS(__END_DECLS,[],AC_DEFINE(__END_DECLS,,[for stupid ast-db1])) 
# Now comes the types check, horrible shit
if test "$tru64_types" = "yes"; then
        AC_CHECK_TYPE(u_int8_t, unsigned char)
        AC_CHECK_TYPE(u_int16_t, unsigned short)
        AC_CHECK_TYPE(u_int32_t, unsigned int)
else
        if test "$broken_types" = "yes" ; then
                AC_CHECK_TYPE(u_int8_t, unsigned char)
                AC_CHECK_TYPE(u_int16_t, unsigned short)
                AC_CHECK_TYPE(u_int32_t, unsigned long int)
        else
                AC_CHECK_TYPE(u_int8_t, uint8_t)
                AC_CHECK_TYPE(u_int16_t, uint16_t)
                AC_CHECK_TYPE(u_int32_t, uint32_t)
        fi
fi

AC_C_BIGENDIAN(AC_DEFINE([__BYTE_ORDER],__BIG_ENDIAN,[Big Endian]),AC_DEFINE([__BYTE_ORDER],__LITTLE_ENDIAN,[Little Endian]))
AC_DEFINE([__LITTLE_ENDIAN],1234,[for the places where it is not defined])
AC_DEFINE([__BIG_ENDIAN],4321,[for the places where it is not defined])

dnl ###########################################################################

if test "${builtin_sqlite3}" = "yes"; then
	SQLITE3_THREADSAFE_LIBS='${top_builddir}/sqlite3-embedded/libopbxsqlite3.la -lpthread'
	SQLITE3_THREADSAFE_CFLAGS='-DTEMP_STORE=2 -DTHREADSAFE=1 -DSQLITE_OMIT_CURSOR -DNO_TCL -I${top_srcdir}/sqlite3-embedded -I${top_builddir}/sqlite3-embedded'
	case "${host}" in
	  *cygwin*)
	  	SQLITE3_THREADSAFE_CFLAGS="${SQLITE3_THREADSAFE_CFLAGS} -DOS_WIN"
		;;
	  *)
	  	SQLITE3_THREADSAFE_CFLAGS="${SQLITE3_THREADSAFE_CFLAGS} -DOS_UNIX"
		;;
	esac
	AC_SUBST([SQLITE3_THREADSAFE_LIBS])
	AC_SUBST([SQLITE3_THREADSAFE_CFLAGS])
	SQLITE3_SUBDIR=sqlite3-embedded
fi

AC_SUBST([SQLITE3_SUBDIR])

dnl ###########################################################################

AC_ARG_WITH([tiff-includes],
	[AC_HELP_STRING([--with-tiff-includes],[Location of libtiff include files.])],
	[CFLAGS="$CFLAGS -I${withval}"])

AC_ARG_WITH([tiff-libs],
	[AC_HELP_STRING([--with-tiff-libs],[Location of libtiff library files.])],
	[LDFLAGS="$LDFLAGS -L${withval}"])

AC_CHECK_HEADERS([tiffio.h],[HAVE_TIFFIO_H=1],[AC_MSG_ERROR(libtiff is required to compile OpenPBX.org.)])
AC_CHECK_LIB([tiff],[TIFFGetVersion],[HAVE_LIBTIFF=1],[AC_MSG_ERROR(libtiff is required to compile OpenPBX.org.)])

dnl ###########################################################################

AC_CHECK_LIB([rt], [timer_create],[AC_DEFINE([HAVE_POSIX_TIMERS],[1],[yes there are real time libraries])],[AC_DEFINE([USE_GENERIC_TIMERS],[1],[forcing generic timers.])])

dnl ###########################################################################

if test "${no_libcap}" != "yes"; then
AC_CHECK_HEADERS([sys/capability.h],[HAVE_PCAP_H=1],[AC_MSG_ERROR(libcap-dev is required to compile OpenPBX.org.)])
fi
dnl ###########################################################################

AC_ARG_WITH([spandsp-includes],
	[AC_HELP_STRING([--with-spandsp-includes],[Location of SpanDSP include files.])],
	[CFLAGS="$CFLAGS -I${withval}"])

AC_ARG_WITH([spandsp-libs],
	[AC_HELP_STRING([--with-spandsp-libs],[Location of SpanDSP library files.])],
	[LDFLAGS="$LDFLAGS -L${withval}"])

AC_CHECK_HEADERS([spandsp.h],[HAVE_SPANDSP_H=1],[AC_MSG_ERROR(SpanDSP version 0.0.3pre27 or newer is required to compile OpenPBX.org.)])

AC_CHECK_HEADERS([spandsp/t38_core.h],[HAVE_SPANDSP_T38_H=1],[AC_MSG_ERROR(SpanDSP does not appear to be new enough. You must have version 0.0.3pre27 or newer to compile OpenPBX.org.)],
[#ifdef HAVE_SPANDSP_H
#include <spandsp.h>
#endif
])

AC_CHECK_LIB([spandsp],[fax_set_transmit_on_idle],[HAVE_LIBSPANDSP=1],[AC_MSG_ERROR(SpanDSP does not appear to be new enough. You must have version 0.0.3pre27 or newer to comple OpenPBX.org.)],[-ltiff])

dnl ###########################################################################
dnl ###########################################################################
dnl ###
dnl ### Optional stuff
dnl ###
dnl ###########################################################################
dnl ###########################################################################

dnl Select which Busydetect routine to use
AC_ARG_ENABLE(busydetect,
	[AC_HELP_STRING([--enable-busydetect=TYPE],[select which busydetect version to use: none (default) toneonly martin compare_tone_and_silence])],,[enable_busydetect="none"])

AC_MSG_CHECKING([for busydetect type])
if test "x$enable_busydetect" != "xnone" ; then
   	case $enable_busydetect in
		toneonly)
			BUSYDETECT=BUSYDETECT_TONEONLY
			;;
		martin)
			BUSYDETECT=BUSYDETECT_MARTIN
			;;
		tone_and_silence)
			BUSYDETECT=BUSYDETECT_TONE_AND_SILENCE
			;;
		*)	
			AC_MSG_ERROR([Unknown busydetect type "$enable_busydetect"])
			;;
	esac
fi
AC_MSG_RESULT([$enable_busydetect])

AC_SUBST([BUSYDETECT])
AC_DEFINE_UNQUOTED([BUSYDETECT],"${BUSYDETECT}",[Select method for detecting busy signals.])

dnl Optimize for host cpu version
AC_ARG_ENABLE(optimization,     
	[AC_HELP_STRING([--disable-optimization],[no detection or tuning flags for cpu version])],
	[OPTIMIZECPU="$enableval"],[OPTIMIZECPU="yes"])

dnl Enable debugging
AC_ARG_ENABLE(debug,
	[AC_HELP_STRING([--enable-debug],[build with debug information])],,[enable_debug="no"])

if test "${enable_debug}" = "yes"; then
   AC_DEFINE([DEBUG],[],[Enable extra debugging.])
fi
AM_CONDITIONAL([WANT_DEBUG],[test "${enable_debug}" = "yes"])

dnl Enable debugging
AC_ARG_ENABLE(debug-scheduler,
	[AC_HELP_STRING([--enable-debug-scheduler],[build with scheduler debug information])],,[enable_debug_scheduler="no"])

if test "${enable_debug_scheduler}" = "yes"; then
   AC_DEFINE([DEBUG_SCHEDULER],[],[Enable scheduler debugging.])
fi

dnl Enable debugging
AC_ARG_ENABLE(stack-backtraces,
	[AC_HELP_STRING([--enable-stack-backtraces],[build with stack backtraces])],,[enable_stack_backtraces="no"])

if test "${enable_stack_backtraces}" = "yes"; then
   AC_DEFINE([STACK_BACKTRACES],[],[Enable generation of stack backtraces.])
fi

dnl
AC_ARG_ENABLE(do-crash,
	[AC_HELP_STRING([--enable-do-crash],[enable DO_CRASH])],,[enable_do_crash="no"])

if test "${enable_do_crash}" = "yes"; then
   AC_DEFINE([DO_CRASH],[],[Enable DO_CRASH.])
fi

dnl Enable frame tracing
AC_ARG_ENABLE(frame-tracing,
        [AC_HELP_STRING([--enable-frame-tracing],[Build with frame tracing.])],,[enable_frame_tracing="no"])

if test "${enable_frame_tracing}" = "yes"; then
   AC_DEFINE([TRACE_FRAMES],[],[Build with frame tracing.])
fi

dnl Enable valgrind shit
AC_ARG_ENABLE(valgrind,
        [AC_HELP_STRING([--enable-valgrind],[Build with valgrind information])],,[enable_valgrind="no"])

if test "${enable_valgrind}" = "yes"; then
   AC_DEFINE([VALGRIND],[],[Build with valgrind information.])
fi

dnl Enable profiling
AC_ARG_ENABLE(profiling,
	[AC_HELP_STRING([--enable-profiling],[build with profiling support])],,[enable_profiling="no"])

if test "${enable_profiling}" = "yes"; then
   AC_DEFINE([PROFILING],[],[Build with profiling support.])
fi

dnl Low memory support
AC_ARG_ENABLE(low_memory,
	[AC_HELP_STRING([--enable-low_memory],[enable low_memory support])],,[enable_low_memory="no"])

if test "${enable_low_memory}" = "yes"; then
   AC_DEFINE([LOW_MEMORY],[],[Low memory support.])
fi

dnl Select user and group
opbxrunuser="openpbx"
opbxrungroup="openpbx"

AC_ARG_WITH(openpbx-user,
	[AC_HELP_STRING([--with-openpbx-user=USER],[Run as USER (default=openpbx)])],,[with_openpbx_user="openpbx"])

if test "${with_openpbx_user}" != "no" -a "${with_openpbx_user}" != "yes"; then
	opbxrunuser="$with_openpbx_user"
fi

AC_ARG_WITH(openpbx-group,
	[AC_HELP_STRING([--with-openpbx-group=GROUP],[Run as GROUP (default=openpbx)])],,[with_openpbx_group="openpbx"])

if test "${with_openpbx_group}" != "no" -a "${with_openpbx_group}" != "yes"; then
	opbxrungroup="$with_openpbx_group"
fi
AC_SUBST(opbxrunuser)
AC_SUBST(opbxrungroup)  

dnl ###########################################################################

AC_ARG_ENABLE([jabber],
	[AC_HELP_STRING([--enable-jabber],[Enable jabber support. (default=no)])],,[enable_jabber="no"])

if test "${enable_jabber}" = "yes"; then
   PKG_CHECK_MODULES([LOUDMOUTH],
	[loudmouth-1.0 >= 1.0.0],
	[HAVE_LOUDMOUTH=1],
	[AC_MSG_WARN([loudmouth library not found ... jabber related features (res_jabber, app_icd) will not be built])])
fi

dnl ###########################################################################

AC_ARG_ENABLE(javascript,
        [AC_HELP_STRING([--enable-javascript],[Enable the use of javascript (default=no)])],,
	[enable_javascript="no"])

if test "${enable_javascript}" = "yes"; then
   AC_CHECK_LIB([js],[main],[HAVE_LIBJS=1])

   AC_CHECK_HEADERS([jstypes.h],[HAVE_JSSCRIPT_H=1],,
[#define XP_UNIX
])
   AC_CHECK_HEADERS([js/jstypes.h],[HAVE_JS_JSSCRIPT_H=1],,
[#define XP_UNIX
])

   if test -n "$HAVE_LIBJS" -a \( -n "${HAVE_JSSCRIPT_H}" -o -n "${HAVE_JS_JSSCRIPT_H}" \); then
      JS_CFLAGS="-DXP_UNIX -DSVR4 -DSYSV -D_BSD_SOURCE -DPOSIX_SOURCE -DHAVE_LOCALTIME_R -DX86_LINUX  -DDEBUG -DDEBUG_root -DJS_THREADSAFE -D_GNU_SOURCE"
      if test -n "${HAVE_JS_JSSCRIPT_H}"; then
	 JS_CFLAGS="${JSCFLAGS} -I/usr/include/js"
      fi
      JS_LDFLAGS="-ljs"
      AC_SUBST(JS_CFLAGS)
      AC_SUBST(JS_LDFLAGS)
   fi

   AC_PATH_PROG([NSPRCONFIG], [nspr-config])

   if test -n "${NSPRCONFIG}"; then
      HAVE_NSPR=1
      NSPR_CFLAGS=`${NSPRCONFIG} --cflags`
      NSPR_LDFLAGS=`${NSPRCONFIG} --libs`
      AC_SUBST(NSPR_CFLAGS)
      AC_SUBST(NSPR_LDFLAGS)
   else
      PKG_CHECK_MODULES([NSPR],
	[mozilla-nspr],
	[HAVE_NSPR=1])
      AC_SUBST(NSPR_CFLAGS)
      AC_SUBST(NSPR_LDFLAGS)
   fi
fi

dnl ###########################################################################

AC_ARG_ENABLE([odbc],
	[AC_HELP_STRING([--enable-odbc],[Enable support for ODBC (default=yes)])],,[enable_odbc="yes"])

if test "${enable_odbc}" = "yes"; then
   AC_CHECK_LIB([odbc], [main], [HAVE_LIBODBC=1])

   if test -n "${HAVE_LIBODBC}"; then
      ODBC_CFLAGS=""
      ODBC_LIBS="-lodbc"
   fi
fi

AC_SUBST([ODBC_CFLAGS])
AC_SUBST([ODBC_LIBS])

dnl ###########################################################################

AC_ARG_ENABLE([postgresql],
	[AC_HELP_STRING([--enable-postgresql],[Enable support for PostgreSQL. (default=no)])],,[enable_postgresql="no"])

if test "${enable_postgresql}" = "yes"; then
   if test "${with_pgsql_inc+set}" != set; then
      if test "${with_pgsql_lib+set}" != set; then
	 if test "${pg_prefix+set}" != set; then
	    with_pgsql_inc="/usr/include/postgresql/"
	    with_pgsql_lib="/usr/lib/"
	 fi
      fi
   fi
   CT_CHECK_POSTGRES_DB
fi

dnl ###########################################################################

AC_ARG_ENABLE([mysql],
	[AC_HELP_STRING([--enable-mysql],[Enable support for MySQL. (default=no)])],,[enable_mysql="no"])

if test "${enable_mysql}" = "yes"; then
   AC_PATH_PROG(MYSQL_CONFIG, mysql_config, no, $PATH)
   if test "$MYSQL_CONFIG" = "no"; then
      enable_mysql="no"
   else
      MYSQL_LIBS=`$MYSQL_CONFIG --libs`
      MYSQL_CFLAGS=`$MYSQL_CONFIG --include`

      AC_SUBST([MYSQL_LIBS])
      AC_SUBST([MYSQL_CFLAGS])
   fi
fi

dnl ###########################################################################

AC_ARG_ENABLE([srtp],
	[AC_HELP_STRING([--enable-srtp],[Enable support for SRTP.])],,[enable_srtp="no"])

if test "${enable_srtp}" = "yes"; then
   AC_CHECK_HEADER([srtp/srtp.h],,[enable_srtp="no"])
   AC_CHECK_LIB([srtp],[srtp_init],,[enable_srtp="no"])
fi

if test "${enable_srtp}" = "yes"; then
   AC_DEFINE([ENABLE_SRTP],,[Enable support for SRTP.])
fi
AM_CONDITIONAL([WANT_SRTP],[test "${enable_srtp}" = "yes"])

dnl ##########################################################################

AC_ARG_ENABLE([ssl],
	[AC_HELP_STRING([--enable-ssl],[Enable support for SSL (default=yes)])],,[enable_ssl="yes"])

if test "${enable_ssl}" = "yes"; then
   CHECK_SSL
fi

dnl ##########################################################################

AC_ARG_ENABLE([udpfromto],
       [AC_HELP_STRING([--enable-udpfromto],[Enable extended UDP support for multi-homed systems (default=no)])],,
       [enable_udpfromto="no"])
 
if test "${enable_udpfromto}" = "yes" ; then
   #
   # IP_PKTINFO (Linux)
   #
   AC_CHECK_DECL([IP_PKTINFO],[HAVE_IP_PKTINFO=1],,
      [#ifdef HAVE_NETINET_IN_H
       #include <netinet/in.h>
       #endif
      ])
   if test "${HAVE_IP_PKTINFO}" = "yes" ; then
      AC_DEFINE([HAVE_IP_PKTINFO],,[Needed for recv-/sendfromto on Linux])
   fi
 
   #
   # IP_RECVDSTADDR (*BSD)
   #
   AC_CHECK_DECL([IP_RECVDSTADDR],[HAVE_IP_RECVDSTADDR=1],,
      [#ifdef HAVE_NETINET_IN_H
       #include <netinet/in.h>
       #endif
      ])
   #
   # IP_SENDSRCADDR (*BSD)
   #
   AC_CHECK_DECL([IP_SENDSRCADDR],[HAVE_IP_SENDSRCADDR=1],,
      [#ifdef HAVE_NETINET_IN_H
       #include <netinet/in.h>
       #endif
      ])
   if test -n "${HAVE_IP_RECVDSTADDR}" -a -n "${HAVE_IP_SENDSRCADDR}"; then
      AC_DEFINE([HAVE_IP_RECVDSTADDR],,[Needed for recv-/sendfromto on *BSD])
      AC_DEFINE([HAVE_IP_SENDSRCADDR],,[Needed for recv-/sendfromto on *BSD])
   fi
fi     

dnl ##########################################################################

AC_ARG_ENABLE([visdn],
	[AC_HELP_STRING([--enable-visdn],[Enable support for vISDN (default=no)])],,[enable_visdn="no"])

if test "${enable_visdn}" = "yes"; then
    AC_CHECK_HEADERS([visdn/visdn.h], [HAVE_VISDN_VISDN_H=1])
    AC_CHECK_LIB([q931], [main], [HAVE_LIBQ931=1])
fi

dnl ##########################################################################

AC_ARG_ENABLE([zaptel],
	[AC_HELP_STRING([--enable-zaptel],[Enable support for Zaptel (default=yes)])],,[enable_zaptel="yes"])

if test "${enable_zaptel}" = "yes"; then
   AC_CHECK_HEADERS([zaptel/zaptel.h], [ZAPTEL_H="\"zaptel/zaptel.h\""])
   if test -z "${ZAPTEL_H}"; then
      AC_CHECK_HEADERS([zaptel.h], [ZAPTEL_H="\"zaptel.h\""])
   fi
   if test -z "${ZAPTEL_H}"; then
      AC_CHECK_HEADERS([linux/zaptel.h], [ZAPTEL_H="\"linux/zaptel.h\""])
   fi

   AC_CHECK_HEADERS([zaptel/tonezone.h], [TONEZONE_H="\"zaptel/tonezone.h\""])
   if test -z "${TONEZONE_H}"; then
      AC_CHECK_HEADERS([tonezone.h], [TONEZONE_H="\"tonezone.h\""])
   fi

   AC_DEFINE_UNQUOTED([ZAPTEL_H], [${ZAPTEL_H}], [Location of zaptel.h header file])
   AC_DEFINE_UNQUOTED([TONEZONE_H], [${TONEZONE_H}], [Location of tonezone.h header file])

   AC_CHECK_LIB([tonezone], [main], [HAVE_LIBTONEZONE=1])
   AC_CHECK_LIB([pri], [main], [HAVE_LIBPRI=1])
fi

AM_CONDITIONAL([HAVE_LIBPRI], [test -n "${HAVE_LIBPRI}"])

dnl ###########################################################################

dnl ###########################################################################
dnl ###
dnl ### Apps
dnl ###
dnl ###########################################################################
dnl ###########################################################################

dnl ###########################################################################

AC_ARG_WITH([app_adsiprog],
	[AC_HELP_STRING([--with-app_adsiprog],[Build and install app_adsiprog (default=yes)])],,
	[with_app_adsiprog="yes"])

AM_CONDITIONAL([WANT_APP_ADSIPROG],[test "${with_app_adsiprog}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_sms],
	[AC_HELP_STRING([--with-app_sms],[Build and install app_sms (default=yes)])],,
	[with_app_sms="yes"])

if test "${with_app_sms}"; then
   AC_CHECK_HEADERS([popt.h], [HAVE_POPT_H=1])
   AC_CHECK_LIB([popt], [poptStrerror], [HAVE_LIBPOPT=1])
fi

AM_CONDITIONAL([WANT_SMSQ],[test -n "${HAVE_POPT_H}" -a -n "${HAVE_LIBPOPT}"])
AM_CONDITIONAL([WANT_APP_SMS],[test "${with_app_sms}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_pipe],
   [AC_HELP_STRING([--with-app_pipe],[Build and install app_pipe (default=yes)])],,
   [with_app_pipe="yes"])

AM_CONDITIONAL([WANT_APP_PIPE],[test "${with_app_pipe}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_v110],
   [AC_HELP_STRING([--with-app_v110],[Build and install app_v110 [Requires chan_misdn] (default=no)])],,
   [with_app_v110="no"])

AM_CONDITIONAL([WANT_APP_V110],[test "${with_app_v110}" = "yes" -a test "${with_chan_misdn}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_authenticate],
	[AC_HELP_STRING([--with-app_authenticate],[Build and install app_authenticate (default=yes)])],,
	[with_app_authenticate="yes"])

AM_CONDITIONAL([WANT_APP_AUTHENTICATE],[test "${with_app_authenticate}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_cdr],
	[AC_HELP_STRING([--with-app_cdr],[Build and install app_cdr (default=yes)])],,
	[with_app_cdr="yes"])

AM_CONDITIONAL([WANT_APP_CDR],[test "${with_app_cdr}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_changrab],
	[AC_HELP_STRING([--with-app_changrab],[Build and install app_changrab (default=yes)])],,
	[with_app_changrab="yes"])

AM_CONDITIONAL([WANT_APP_CHANGRAB],[test "${with_app_changrab}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_chanisavail],
	[AC_HELP_STRING([--with-app_chanisavail],[Build and install app_chanisavail (default=yes)])],,
	[with_app_chanisavail="yes"])

AM_CONDITIONAL([WANT_APP_CHANISAVAIL],[test "${with_app_chanisavail}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_chanspy],
	[AC_HELP_STRING([--with-app_chanspy],[Build and install app_chanspy (default=yes)])],,
	[with_app_chanspy="yes"])

AM_CONDITIONAL([WANT_APP_CHANSPY],[test "${with_app_chanspy}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_controlplayback],
	[AC_HELP_STRING([--with-app_controlplayback],[Build and install app_controlplayback (default=yes)])],,
	[with_app_controlplayback="yes"])

AM_CONDITIONAL([WANT_APP_CONTROLPLAYBACK],[test "${with_app_controlplayback}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_db],
	[AC_HELP_STRING([--with-app_db],[Build and install app_db (default=yes)])],,
	[with_app_db="yes"])

AM_CONDITIONAL([WANT_APP_DB],[test "${with_app_db}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_setcallerpres],
	[AC_HELP_STRING([--with-app_setcallerpres],[Build and install app_setcallerpres (default=yes)])],,
	[with_app_setcallerpres="yes"])

AM_CONDITIONAL([WANT_APP_SETCALLERPRES],[test "${with_app_setcallerpres}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_dial],
	[AC_HELP_STRING([--with-app_dial],[Build and install app_dial (default=yes)])],,
	[with_app_dial="yes"])

AM_CONDITIONAL([WANT_APP_DIAL],[test "${with_app_dial}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_dictate],
	[AC_HELP_STRING([--with-app_dictate],[Build and install app_dictate (default=yes)])],,
	[with_app_dictate="yes"])

AM_CONDITIONAL([WANT_APP_DICTATE],[test "${with_app_dictate}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_directed_pickup],
	[AC_HELP_STRING([--with-app_directed_pickup],[Build and install app_directed_pickup (default=yes)])],,
	[with_app_directed_pickup="yes"])

AM_CONDITIONAL([WANT_APP_DIRECTED_PICKUP],[test "${with_app_directed_pickup}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_directory],
	[AC_HELP_STRING([--with-app_directory],[Build and install app_directory (default=yes)])],,
	[with_app_directory="yes"])

AM_CONDITIONAL([WANT_APP_DIRECTORY],[test "${with_app_directory}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_disa],
	[AC_HELP_STRING([--with-app_disa],[Build and install app_disa (default=yes)])],,
	[with_app_disa="yes"])

AM_CONDITIONAL([WANT_APP_DISA],[test "${with_app_disa}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_dumpchan],
	[AC_HELP_STRING([--with-app_dumpchan],[Build and install app_dumpchan (default=yes)])],,
	[with_app_dumpchan="yes"])

AM_CONDITIONAL([WANT_APP_DUMPCHAN],[test "${with_app_dumpchan}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_echo],
	[AC_HELP_STRING([--with-app_echo],[Build and install app_echo (default=yes)])],,
	[with_app_echo="yes"])

AM_CONDITIONAL([WANT_APP_ECHO],[test "${with_app_echo}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_enumlookup],
	[AC_HELP_STRING([--with-app_enumlookup],[Build and install app_enumlookup (default=yes)])],,
	[with_app_enumlookup="yes"])

AM_CONDITIONAL([WANT_APP_ENUMLOOKUP],[test "${with_app_enumlookup}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_eval],
	[AC_HELP_STRING([--with-app_eval],[Build and install app_eval (default=yes)])],,
	[with_app_eval="yes"])

AM_CONDITIONAL([WANT_APP_EVAL],[test "${with_app_eval}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_exec],
	[AC_HELP_STRING([--with-app_exec],[Build and install app_exec (default=yes)])],,
	[with_app_exec="yes"])

AM_CONDITIONAL([WANT_APP_EXEC],[test "${with_app_exec}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_faxdetect],
	[AC_HELP_STRING([--with-app_faxdetect],[Build and install app_faxdetect (default=yes)])],,
	[with_app_faxdetect="yes"])

AM_CONDITIONAL([WANT_APP_FAXDETECT], [test "${with_app_faxdetect}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_flash],
	[AC_HELP_STRING([--with-app_flash],[Build and install app_flash (default=yes)])],,
	[with_app_flash="yes"])

AM_CONDITIONAL([WANT_APP_FLASH], [test "${enable_zaptel}" = "yes" -a "${with_app_flash}" = "yes" -a -n "${ZAPTEL_H}"])

dnl ###########################################################################

AC_ARG_WITH([app_forkcdr],
	[AC_HELP_STRING([--with-app_forkcdr],[Build and install app_forkcdr (default=yes)])],,
	[with_app_forkcdr="yes"])

AM_CONDITIONAL([WANT_APP_FORKCDR],[test "${with_app_forkcdr}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_getcpeid],
	[AC_HELP_STRING([--with-app_getcpeid],[Build and install app_getcpeid (default=yes)])],,
	[with_app_getcpeid="yes"])

AM_CONDITIONAL([WANT_APP_GETCPEID],[test "${with_app_getcpeid}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_groupcount],
	[AC_HELP_STRING([--with-app_groupcount],[Build and install app_groupcount (default=yes)])],,
	[with_app_groupcount="yes"])

AM_CONDITIONAL([WANT_APP_GROUPCOUNT],[test "${with_app_groupcount}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_hasnewvoicemail],
	[AC_HELP_STRING([--with-app_hasnewvoicemail],[Build and install app_hasnewvoicemail (default=yes)])],,
	[with_app_hasnewvoicemail="yes"])

AM_CONDITIONAL([WANT_APP_HASNEWVOICEMAIL],[test "${with_app_hasnewvoicemail}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_lookupblacklist],
	[AC_HELP_STRING([--with-app_lookupblacklist],[Build and install app_lookupblacklist (default=yes)])],,
	[with_app_lookupblacklist="yes"])

AM_CONDITIONAL([WANT_APP_LOOKUPBLACKLIST],[test "${with_app_lookupblacklist}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_lookupcidname],
	[AC_HELP_STRING([--with-app_lookupcidname],[Build and install app_lookupcidname (default=yes)])],,
	[with_app_lookupcidname="yes"])

AM_CONDITIONAL([WANT_APP_LOOKUPCIDNAME],[test "${with_app_lookupcidname}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_macro],
	[AC_HELP_STRING([--with-app_macro],[Build and install app_macro (default=yes)])],,
	[with_app_macro="yes"])

AM_CONDITIONAL([WANT_APP_MACRO],[test "${with_app_macro}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_proc],
	[AC_HELP_STRING([--with-app_proc],[Build and install app_proc (default=yes)])],,
	[with_app_proc="yes"])

AM_CONDITIONAL([WANT_APP_PROC],[test "${with_app_proc}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_meetme],
	[AC_HELP_STRING([--with-app_meetme],[Build and install app_meetme (default=yes)])],,
	[with_app_meetme="yes"])

AM_CONDITIONAL([WANT_APP_MEETME], [test "${enable_zaptel}" = "yes" -a "${with_app_meetme}" = "yes" -a -n "${ZAPTEL_H}"])

dnl ###########################################################################

AC_ARG_WITH([app_milliwatt],
	[AC_HELP_STRING([--with-app_milliwatt],[Build and install app_milliwatt (default=yes)])],,
	[with_app_milliwatt="yes"])

AM_CONDITIONAL([WANT_APP_MILLIWATT],[test "${with_app_milliwatt}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_muxmon],
	[AC_HELP_STRING([--with-app_muxmon],[Build and install app_muxmon (default=yes)])],,
	[with_app_muxmon="yes"])

AM_CONDITIONAL([WANT_APP_MUXMON],[test "${with_app_muxmon}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_osplookup],
	[AC_HELP_STRING([--with-app_osplookup],[Build and install app_osplookup (default=no)])],,
	[with_app_osplookup="no"])

AM_CONDITIONAL([WANT_APP_OSPLOOKUP],[test "${with_app_osplookup}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_playback],
	[AC_HELP_STRING([--with-app_playback],[Build and install app_playback (default=yes)])],,
	[with_app_playback="yes"])

AM_CONDITIONAL([WANT_APP_PLAYBACK],[test "${with_app_playback}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_privacy],
	[AC_HELP_STRING([--with-app_privacy],[Build and install app_privacy (default=yes)])],,
	[with_app_privacy="yes"])

AM_CONDITIONAL([WANT_APP_PRIVACY],[test "${with_app_privacy}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_queue],
	[AC_HELP_STRING([--with-app_queue],[Build and install app_queue (default=yes)])],,
	[with_app_queue="yes"])

AM_CONDITIONAL([WANT_APP_QUEUE],[test "${with_app_queue}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_random],
	[AC_HELP_STRING([--with-app_random],[Build and install app_random (default=yes)])],,
	[with_app_random="yes"])

AM_CONDITIONAL([WANT_APP_RANDOM],[test "${with_app_random}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_read],
	[AC_HELP_STRING([--with-app_read],[Build and install app_read (default=yes)])],,
	[with_app_read="yes"])

AM_CONDITIONAL([WANT_APP_READ],[test "${with_app_read}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_record],
	[AC_HELP_STRING([--with-app_record],[Build and install app_record (default=yes)])],,
	[with_app_record="yes"])

AM_CONDITIONAL([WANT_APP_RECORD],[test "${with_app_record}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_rxfax],
	[AC_HELP_STRING([--with-app_rxfax],[Build and install app_rxfax (default=yes)])],,
	[with_app_rxfax="yes"])

AM_CONDITIONAL([WANT_APP_RXFAX],
	[test "${with_app_rxfax}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_sayunixtime],
	[AC_HELP_STRING([--with-app_sayunixtime],[Build and install app_sayunixtime (default=yes)])],,
	[with_app_sayunixtime="yes"])

AM_CONDITIONAL([WANT_APP_SAYUNIXTIME],[test "${with_app_sayunixtime}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_senddtmf],
	[AC_HELP_STRING([--with-app_senddtmf],[Build and install app_senddtmf (default=yes)])],,
	[with_app_senddtmf="yes"])

AM_CONDITIONAL([WANT_APP_SENDDTMF],[test "${with_app_senddtmf}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_sendtext],
	[AC_HELP_STRING([--with-app_sendtext],[Build and install app_sendtext (default=yes)])],,
	[with_app_sendtext="yes"])

AM_CONDITIONAL([WANT_APP_SENDTEXT],[test "${with_app_sendtext}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_setcdruserfield],
	[AC_HELP_STRING([--with-app_setcdruserfield],[Build and install app_setcdruserfield (default=yes)])],,
	[with_app_setcdruserfield="yes"])

AM_CONDITIONAL([WANT_APP_SETCDRUSERFIELD],[test "${with_app_setcdruserfield}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_setrdnis],
	[AC_HELP_STRING([--with-app_setrdnis],[Build and install app_setrdnis (default=yes)])],,
	[with_app_setrdnis="yes"])

AM_CONDITIONAL([WANT_APP_SETRDNIS],[test "${with_app_setrdnis}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_settransfercapability],
	[AC_HELP_STRING([--with-app_settransfercapability],[Build and install app_settransfercapability (default=yes)])],,
	[with_app_settransfercapability="yes"])

AM_CONDITIONAL([WANT_APP_SETTRANSFERCAPABILITY],[test "${with_app_settransfercapability}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_softhangup],
	[AC_HELP_STRING([--with-app_softhangup],[Build and install app_softhangup (default=yes)])],,
	[with_app_softhangup="yes"])

AM_CONDITIONAL([WANT_APP_SOFTHANGUP],[test "${with_app_softhangup}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_sql_mysql],
        [AC_HELP_STRING([--with-app_sql_mysql],[Build and install app_sql_mysql (default=yes)])],,
        [with_app_sql_mysql="yes"])
		
AM_CONDITIONAL([WANT_APP_SQL_MYSQL],[test "${enable_mysql}" = "yes" -a "$with_app_sql_mysql" = "yes"])
		
dnl ###########################################################################

AC_ARG_WITH([app_sql_postgres],
        [AC_HELP_STRING([--with-app_sql_postgres],[Build and install app_sql_postgres (default=yes)])],,
        [with_app_sql_postgres="yes"])
		
AM_CONDITIONAL([WANT_APP_SQL_POSTGRES],[test "${enable_postgresql}" = "yes" -a "$with_app_sql_postgres" = "yes"])
		
dnl ###########################################################################
		
AC_ARG_WITH([app_system],
	[AC_HELP_STRING([--with-app_system],[Build and install app_system (default=yes)])],,
	[with_app_system="yes"])

AM_CONDITIONAL([WANT_APP_SYSTEM],[test "${with_app_system}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_backgrounddetect],
	[AC_HELP_STRING([--with-app_backgrounddetect],[Build and install app_backgrounddetect (default=yes)])],,
	[with_app_backgrounddetect="yes"])

AM_CONDITIONAL([WANT_APP_BACKGROUNDDETECT],[test "${with_app_backgrounddetect}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_transfer],
	[AC_HELP_STRING([--with-app_transfer],[Build and install app_transfer (default=yes)])],,
	[with_app_transfer="yes"])

AM_CONDITIONAL([WANT_APP_TRANSFER],[test "${with_app_transfer}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_txfax],
	[AC_HELP_STRING([--with-app_txfax],[Build and install app_txfax (default=yes)])],,
	[with_app_txfax="yes"])

AM_CONDITIONAL([WANT_APP_TXFAX],
	[test "${with_app_txfax}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_userevent],
	[AC_HELP_STRING([--with-app_userevent],[Build and install app_userevent (default=yes)])],,
	[with_app_userevent="yes"])

AM_CONDITIONAL([WANT_APP_USEREVENT],[test "${with_app_userevent}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_valetparking],
	[AC_HELP_STRING([--with-app_valetparking],[Build and install app_valetparking (default=no)])],,
	[with_app_valetparking="no"])

AM_CONDITIONAL([WANT_APP_VALETPARKING],[test "${with_app_valetparking}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_verbose],
	[AC_HELP_STRING([--with-app_verbose],[Build and install app_verbose (default=yes)])],,
	[with_app_verbose="yes"])

AM_CONDITIONAL([WANT_APP_VERBOSE],[test "${with_app_verbose}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_visdn_ppp],
	[AC_HELP_STRING([--with-app_visdn_ppp],[Build and install app_visdn_ppp (default=yes)])],,
	[with_app_visdn_ppp="yes"])

AM_CONDITIONAL([WANT_APP_VISDN_PPP],[test "${with_app_visdn_ppp}" = "yes" -a "${enable_visdn}" -a -n "${HAVE_VISDN_VISDN_H}" -a -n "${HAVE_LIBQ931}"])

dnl ###########################################################################

AC_ARG_WITH([app_voicemail],
	[AC_HELP_STRING([--with-app_voicemail],[Build and install app_voicemail (default=yes)])],,
	[with_app_voicemail="yes"])

AM_CONDITIONAL([WANT_APP_VOICEMAIL],[test "${with_app_voicemail}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_waitforring],
	[AC_HELP_STRING([--with-app_waitforring],[Build and install app_waitforring (default=yes)])],,
	[with_app_waitforring="yes"])

AM_CONDITIONAL([WANT_APP_WAITFORRING],[test "${with_app_waitforring}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_waitforsilence],
	[AC_HELP_STRING([--with-app_waitforsilence],[Build and install app_waitforsilence (default=yes)])],,
	[with_app_waitforsilence="yes"])

AM_CONDITIONAL([WANT_APP_WAITFORSILENCE],[test "${with_app_waitforsilence}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_while],
	[AC_HELP_STRING([--with-app_while],[Build and install app_while (default=yes)])],,
	[with_app_while="yes"])

AM_CONDITIONAL([WANT_APP_WHILE],[test "${with_app_while}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_zapateller],
	[AC_HELP_STRING([--with-app_zapateller],[Build and install app_zapateller (default=yes)])],,
	[with_app_zapateller="yes"])

AM_CONDITIONAL([WANT_APP_ZAPATELLER],[test "${with_app_zapateller}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_ldap],
	[AC_HELP_STRING([--with-app_ldap],[Build and install app_ldap (default=no)])],,
	[with_app_ldap="no"])

AM_CONDITIONAL([WANT_APP_LDAP],[test "${with_app_ldap}" = "yes"])

if test "${with_app_ldap}" = "yes"; then
   AC_CHECK_LIB(ldap, ldap_open, [], [
		      echo "LDAP libraries not found. Install openldap-dev"
		      exit -1
		      ])

   AC_CHECK_LIB(lber, ber_alloc, [], [
		      echo "BER libraries not found. Install openldap-dev"
		      exit -1
		      ])

fi

dnl ###########################################################################

AC_ARG_WITH([app_devstate],
	[AC_HELP_STRING([--with-app_devstate],[Build and install app_devstate (default=yes)])],,
	[with_app_devstate="yes"])

AM_CONDITIONAL([WANT_APP_DEVSTATE],[test "${with_app_devstate}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_stack],
	[AC_HELP_STRING([--with-app_stack],[Build and install app_stack (default=yes)])],,
	[with_app_stack="yes"])

AM_CONDITIONAL([WANT_APP_STACK],[test "${with_app_stack}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_getdevstate],
	[AC_HELP_STRING([--with-app_getdevstate],[Build and install app_getdevstate (default=yes)])],,
	[with_app_getdevstate="yes"])

AM_CONDITIONAL([WANT_APP_GETDEVSTATE],[test "${with_app_getdevstate}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([app_getextstate],
	[AC_HELP_STRING([--with-app_getextstate],[Build and install app_getextstate (default=yes)])],,
	[with_app_getextstate="yes"])

AM_CONDITIONAL([WANT_APP_GETEXTSTATE],[test "${with_app_getextstate}" = "yes"])

dnl ###########################################################################
dnl ###########################################################################
dnl ###
dnl ### Channels
dnl ###
dnl ###########################################################################
dnl ###########################################################################


AC_ARG_WITH([chan_alsa],
        [AC_HELP_STRING([--with-chan_alsa],[Build and install chan_alsa (default=no)])],,
        [with_chan_alsa="no"])

if test "${with_chan_alsa}" = "yes"; then
   PKG_CHECK_MODULES([ALSA], [alsa], [HAVE_ALSA=1])
fi

AC_SUBST(ALSA_CFLAGS)
AC_SUBST(ALSA_LIBS)

AM_CONDITIONAL([WANT_CHAN_ALSA],[test "${with_chan_alsa}" = "yes" -a -n "${HAVE_ALSA}"])

AC_ARG_WITH([chan_agent],
	[AC_HELP_STRING([--with-chan_agent],[Build and install chan_agent (default=yes)])],,[with_chan_agent="yes"])

AM_CONDITIONAL([WANT_CHAN_AGENT],[test "${with_chan_agent}" = "yes"])

dnl ###########################################################################

MISDN_CFLAGS=-I/usr/include/mISDNuser
MISDN_LDFLAGS=

AC_ARG_WITH([misdn-includes],
	[AC_HELP_STRING([--with-misdn-includes],[Location of mISDN include files.])],
	[MISDN_CFLAGS="-I${withval}"])

AC_ARG_WITH([misdn-libs],
	[AC_HELP_STRING([--with-misdn-libs],[Location of mISDN library files.])],
	[MISDN_LDFLAGS="-L${withval}"])

AC_SUBST(MISDN_CFLAGS)
AC_SUBST(MISDN_LDFLAGS)

AC_ARG_WITH([chan_misdn],
	[AC_HELP_STRING([--with-chan_misdn],[Build and install chan_misdn (default=no)])],,[with_chan_misdn="no"])

AM_CONDITIONAL([WANT_CHAN_MISDN],[test "${with_chan_misdn}" = "yes"])

dnl ###########################################################################
	
AC_ARG_WITH([chan_capi],
	[AC_HELP_STRING([--with-chan_capi],[Build and install chan_capi (default=no)])],,[with_chan_capi="no"])

if test "${with_chan_capi}" = "yes" ; then
   AC_CHECK_HEADERS([capi20.h], [HAVE_CAPI20_H=1])
   AC_CHECK_HEADERS([capiutils.h],[HAVE_CAPIUTILS_H=1])

	if test -n "${HAVE_CAPI20_H}" -a -n "${HAVE_CAPIUTILS_H}"
	then
		WANT_CHAN_CAPI="yes"
	fi
fi

AM_CONDITIONAL([WANT_CHAN_CAPI],[test "${with_chan_capi}" = "yes" -a -n "${HAVE_CAPI20_H}" -a -n "${HAVE_CAPIUTILS_H}"])

dnl ###########################################################################

AC_ARG_WITH([chan_fax],
	[AC_HELP_STRING([--with-chan_fax],[Build and install chan_fax (default=no)])],,[want_chan_fax="no"])

if test "${with_chan_fax}" = "yes"; then
   CHAN_FAX_LIBS="-lspandsp -ltiff"
   AC_CHECK_HEADERS([pty.h], [HAVE_PTY_H=1])
   AC_CHECK_LIB([util], [openpty], [HAVE_OPENPTY=1 CHAN_FAX_LIBS="${CHAN_FAX_LIBS} -lutil"],
	[AC_CHECK_LIB([c], [openpty], [HAVE_OPENPTY=1])])
fi
AC_SUBST(CHAN_FAX_LIBS)

AM_CONDITIONAL([WANT_CHAN_FAX],
	[test "${with_chan_fax}" = "yes" -a -n "${HAVE_OPENPTY}"])

dnl ###########################################################################

AC_ARG_WITH([chan_features],
	[AC_HELP_STRING([--with-chan_features],[Build and install chan_features (default=yes)])],,[with_chan_features="yes"])

AM_CONDITIONAL([WANT_CHAN_FEATURES],[test "${with_chan_features}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([chan_iax2],
	[AC_HELP_STRING([--with-chan_iax2],[Build and install chan_iax2 (default=yes)])],,[with_chan_iax2="yes"])

AM_CONDITIONAL([WANT_CHAN_IAX2],[test "${with_chan_iax2}" = "yes"])

dnl ###########################################################################

AC_ARG_ENABLE([iax-trunking],
        [AC_HELP_STRING([--enable-iax-trunking],[Enables the IAX trunking feature (default=no)])],,[enable_iax_trunking="no"])

if test "${enable_iax_trunking}" = "yes"; then
   AC_DEFINE([IAX_TRUNKING],,[Enable IAX trunking feature])
fi


dnl ###########################################################################
if test "${enable_iax_trunking}" = "yes"; then
   AC_DEFINE([IAX_TRUNKING],,[Enable IAX trunking feature])
   fi
   
AC_ARG_WITH([chan_local],
	[AC_HELP_STRING([--with-chan_local],[Build and install chan_local (default=yes)])],,[with_chan_local="yes"])

AM_CONDITIONAL([WANT_CHAN_LOCAL],[test "${with_chan_local}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([chan_mgcp],
	[AC_HELP_STRING([--with-chan_mgcp],[Build and install chan_mgcp (default=yes)])],,[with_chan_mgcp="yes"])

AM_CONDITIONAL([WANT_CHAN_MGCP],[test "${with_chan_mgcp}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([chan_sip],
	[AC_HELP_STRING([--with-chan_sip],[Build and install chan_sip (default=yes)])],,[with_chan_sip="yes"])

AM_CONDITIONAL([WANT_CHAN_SIP],[test "${with_chan_sip}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([chan_sccp],
	[AC_HELP_STRING([--with-chan_sccp],[Build and install chan_sccp (default=yes)])],,[with_chan_sccp="no"])

AM_CONDITIONAL([WANT_CHAN_SCCP],[test "${with_chan_sccp}" = "yes"])

AC_ARG_ENABLE([sccp-park],
	[AC_HELP_STRING([--disable-sccp-park],[Disable sccp call parking support (default=enabled)])],,[enable_sccp_park="yes"])

if test "x${enable_sccp_park}" != "xno"; then
	AC_DEFINE([CS_SCCP_PARK],[1],[chan_sccp call parking support])
fi

AC_ARG_ENABLE([sccp-pickup],
	[AC_HELP_STRING([--disable-sccp-pickup],[Disable sccp call pickup support (default=enabled)])],,[enable_sccp_pickup="yes"])

if test "x${enable_sccp_pickup}" != "xno"; then
	AC_DEFINE([CS_SCCP_PICKUP],[1],[chan_sccp call pickup support])
fi

dnl ###########################################################################

AC_ARG_WITH(chan_unicall,
	[AC_HELP_STRING([--with-chan_unicall],[Build and install chan_unicall (default=no).])],,[with_chan_unicall="no"])

if test "${with_chan_unicall}" = "yes"; then
   AC_CHECK_LIB([unicall], [main], [HAVE_LIBUNICALL=1])
fi

AM_CONDITIONAL([WANT_CHAN_UNICALL],[test "${with_chan_unicall}" = "yes" -a -n "${HAVE_LIBUNICALL}"])

dnl ###########################################################################

AC_ARG_WITH([chan_visdn],
	[AC_HELP_STRING([--with-chan_visdn],[Build and install chan_visdn (default=no)])],,[with_chan_visdn="no"])

AM_CONDITIONAL([WANT_CHAN_VISDN],[test "${with_chan_visdn}" = "yes" -a "${enable_visdn}" -a -n "${HAVE_VISDN_VISDN_H}" -a -n "${HAVE_LIBQ931}"])

dnl ###########################################################################

AC_ARG_WITH([chan_woomera],
	[AC_HELP_STRING([--with-chan_woomera],[Build and install chan_woomera (default=yes)])],,
	[with_chan_woomera="yes"])

AM_CONDITIONAL([WANT_CHAN_WOOMERA], [test "${with_chan_woomera}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([chan_bluetooth],
	[AC_HELP_STRING([--with-chan_bluetooth],[Build and install chan_bluetoot (default=no)])],,
	[with_chan_bluetooth="no"])

if test "${with_chan_bluetooth}" = "yes" ; then
	AC_CHECK_HEADERS([bluetooth/bluetooth.h], [HAVE_BLUETOOTH_H=1])
	AC_CHECK_LIB([bluetooth], [main], [HAVE_LIBBLUETOOTH=1])
fi


AM_CONDITIONAL([WANT_CHAN_BLUETOOTH], [test "${with_chan_bluetooth}" = "yes" -a -n "${HAVE_BLUETOOTH_H}" -a -n "${HAVE_LIBBLUETOOTH}"])

dnl ###########################################################################

AC_ARG_WITH([chan_zap],
	[AC_HELP_STRING([--with-chan_zap],[Build and install chan_zap (default=yes) (requires --enable-zaptel)])],,
	[with_chan_zap="yes"])

AM_CONDITIONAL([WANT_CHAN_ZAP], [test "${enable_zaptel}" = "yes" -a "${with_chan_zap}" = "yes" -a -n "${HAVE_LIBTONEZONE}"])

dnl app_icd
#app_icd = "no"
#AC_ARG_WITH(app_icd,
#	[ --with-app_icd=no			use app_icd, a full blown inteligent call distribution system (default=no) *** HIGHLY EXPERIMENTAL FOR THE MOMENT ****],,[with_app_icd="no"])
#
#

dnl ###########################################################################
dnl ###########################################################################
dnl ###
dnl ### CDR
dnl ###
dnl ###########################################################################
dnl ###########################################################################

AC_ARG_WITH(cdr_odbc,
	[AC_HELP_STRING([--with-cdr_odbc],[Build and install cdr_odbc (default=no) (requires --enable-odbc])],,[with_cdr_odbc="no"])

AM_CONDITIONAL([WANT_CDR_ODBC],[test "${enable_odbc}" = "yes" -a "${with_cdr_odbc}" = "yes" -a -n "${HAVE_LIBODBC}"])

dnl ###########################################################################

AC_ARG_WITH(cdr_mysql,
	[AC_HELP_STRING([--with-cdr_mysql],[Build and install cdr_mysql (default=no) (requires --enable-mysql])],,[with_cdr_mysql="no"])

AM_CONDITIONAL([WANT_CDR_MYSQL],[test "${enable_mysql}" = "yes" -a "$with_cdr_mysql" = "yes"])

dnl ###########################################################################

AC_ARG_WITH(cdr_pgsql,
	[AC_HELP_STRING([--with-cdr_pgsql],[Build and install cdr_pgsql (default=no) (requires --enable-postgresql])],,[with_cdr_pgsql="no"])

AM_CONDITIONAL([WANT_CDR_PGSQL],[test "${enable_postgresql}" = "yes" -a "$with_cdr_pgsql" = "yes"])

dnl ###########################################################################
dnl ###########################################################################
dnl ###
dnl ### Codecs
dnl ###
dnl ###########################################################################
dnl ###########################################################################

AC_ARG_WITH([codec_speex],
	[AC_HELP_STRING([--with-codec_speex],[Build and install codec_speex (default=yes)])],,[with_codec_speex="yes"])

if test "${with_codec_speex}"; then
   AC_CHECK_LIB([speex], [main], [HAVE_LIBSPEEX=1])
   
   AC_CHECK_HEADER([speex/speex.h],[HAVE_SPEEX_H=1 AC_SUBST([SPEEX_CFLAGS],[-I/usr/include/speex])])
   AC_CHECK_HEADER([speex.h],[HAVE_SPEEX_H=1])
fi

AM_CONDITIONAL([WANT_CODEC_SPEEX],[test "${with_codec_speex}" = "yes" -a -n "${HAVE_LIBSPEEX}" -a -n "${HAVE_SPEEX_H}"])

dnl ###########################################################################
dnl ###########################################################################
dnl ###
dnl ### Formats
dnl ###
dnl ###########################################################################
dnl ###########################################################################

AC_ARG_WITH([format_jpeg],
        [AC_HELP_STRING([--with-format_jpeg],[Build and install format_jpeg (default=yes)])],,
        [with_format_jpeg="yes"])

AM_CONDITIONAL([WANT_FORMAT_JPEG],[test "${with_format_jpeg}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([format_oggvorbis],
        [AC_HELP_STRING([--with-format_oggvorbis],[Build and install format_oggvorbis (default=yes)])],,
        [with_format_oggvorbis="yes"])

if test "${with_format_oggvorbis}" = "yes"; then
	AC_CHECK_LIB([ogg], [main], [HAVE_LIBOGG=1 OGG_LIBS="-logg"])
	AC_CHECK_LIB([vorbis], [main], [HAVE_LIBVORBIS=1 VORBIS_LIBS="-lvorbis"])
	AC_CHECK_LIB([vorbisenc], [main], [HAVE_LIBVORBISENC=1 VORBIS_LIBS="${VORBIS_LIBS} -lvorbisenc"])

	AC_CHECK_HEADER([ogg/ogg.h], [HAVE_OGG_H=1])
	AC_CHECK_HEADER([vorbis/vorbisenc.h], [HAVE_VORBIS_H=1])

	if test -n "${HAVE_LIBVORBIS}" -a -n "${HAVE_LIBVORBISENC}" -a -n "${HAVE_LIBOGG}"; then
		AC_SUBST([VORBIS_LIBS], [${VORBIS_LIBS}])
		AC_SUBST([OGG_LIBS], [${OGG_LIBS}])
	fi
fi

AM_CONDITIONAL([WANT_FORMAT_OGGVORBIS],
	[test "${with_format_oggvorbis}" = "yes" -a -n "${HAVE_LIBVORBIS}" -a -n "${HAVE_LIBVORBISENC}" -a -n "${HAVE_LIBOGG}" -a -n "${HAVE_OGG_H}" -a -n "${HAVE_VORBIS_H}"])

dnl ###########################################################################
dnl ###########################################################################
dnl ###
dnl ### Functions
dnl ###
dnl ###########################################################################
dnl ###########################################################################

AC_ARG_WITH([func_callerid],
        [AC_HELP_STRING([--with-func_callerid],[Build and install func_callerid (default=yes)])],,
        [with_func_callerid="yes"])

AM_CONDITIONAL([WANT_FUNC_CALLERID],[test "${with_func_callerid}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([func_cdr],
        [AC_HELP_STRING([--with-func_cdr],[Build and install func_cdr (default=yes)])],,
        [with_func_cdr="yes"])

AM_CONDITIONAL([WANT_FUNC_CDR],[test "${with_func_cdr}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([func_db],
        [AC_HELP_STRING([--with-func_db],[Build and install func_db (default=yes)])],,
        [with_func_db="yes"])

AM_CONDITIONAL([WANT_FUNC_DB],[test "${with_func_db}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([func_enum],
        [AC_HELP_STRING([--with-func_enum],[Build and install func_enum (default=yes)])],,
        [with_func_enum="yes"])

AM_CONDITIONAL([WANT_FUNC_ENUM],[test "${with_func_enum}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([func_env],
        [AC_HELP_STRING([--with-func_env],[Build and install func_env (default=yes)])],,
        [with_func_env="yes"])

AM_CONDITIONAL([WANT_FUNC_ENV],[test "${with_func_env}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([func_fileexists],
        [AC_HELP_STRING([--with-func_fileexists],[Build and install func_fileexists (default=yes)])],,
        [with_func_fileexists="yes"])

AM_CONDITIONAL([WANT_FUNC_FILEEXISTS],[test "${with_func_fileexists}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([func_groupcount],
        [AC_HELP_STRING([--with-func_groupcount],[Build and install func_groupcount (default=yes)])],,
        [with_func_groupcount="yes"])

AM_CONDITIONAL([WANT_FUNC_GROUPCOUNT],[test "${with_func_groupcount}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([func_language],
        [AC_HELP_STRING([--with-func_language],[Build and install func_language (default=yes)])],,
        [with_func_language="yes"])

AM_CONDITIONAL([WANT_FUNC_LANGUAGE],[test "${with_func_language}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([func_logic],
        [AC_HELP_STRING([--with-func_logic],[Build and install func_logic (default=yes)])],,
        [with_func_logic="yes"])

AM_CONDITIONAL([WANT_FUNC_LOGIC],[test "${with_func_logic}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([func_math],
        [AC_HELP_STRING([--with-func_math],[Build and install func_math (default=yes)])],,
        [with_func_math="yes"])

AM_CONDITIONAL([WANT_FUNC_MATH],[test "${with_func_math}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([func_md5],
        [AC_HELP_STRING([--with-func_md5],[Build and install func_md5 (default=yes)])],,
        [with_func_md5="yes"])

AM_CONDITIONAL([WANT_FUNC_MD5],[test "${with_func_md5}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([func_moh],
        [AC_HELP_STRING([--with-func_moh],[Build and install func_moh (default=yes)])],,
        [with_func_moh="yes"])

AM_CONDITIONAL([WANT_FUNC_MOH],[test "${with_func_moh}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([func_strings],
        [AC_HELP_STRING([--with-func_strings],[Build and install func_strings (default=yes)])],,
        [with_func_strings="yes"])

AM_CONDITIONAL([WANT_FUNC_STRINGS],[test "${with_func_strings}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([func_timeout],
        [AC_HELP_STRING([--with-func_timeout],[Build and install func_timeout (default=yes)])],,
        [with_func_timeout="yes"])

AM_CONDITIONAL([WANT_FUNC_TIMEOUT],[test "${with_func_timeout}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([func_uri],
        [AC_HELP_STRING([--with-func_uri],[Build and install func_uri (default=yes)])],,
        [with_func_uri="yes"])

AM_CONDITIONAL([WANT_FUNC_URI],[test "${with_func_uri}" = "yes"])

dnl ###########################################################################
dnl ###########################################################################
dnl ###
dnl ### PBX
dnl ###
dnl ###########################################################################
dnl ###########################################################################

AC_ARG_WITH([pbx_dundi],
	[AC_HELP_STRING([--with-pbx_dundi],[Build and install pbx_dundi (default=yes)])],,
	[with_pbx_dundi="yes"])

if test "${with_pbx_dundi}" = "yes"; then
   AC_CHECK_LIB([z], [main], [HAVE_LIBZ=1])
fi
   
AM_CONDITIONAL([WANT_PBX_DUNDI],[test "${with_pbx_dundi}" = "yes" -a -n "${HAVE_LIBZ}"])

dnl ###########################################################################

AC_ARG_WITH([pbx_realtime],
	[AC_HELP_STRING([--with-pbx_realtime],[Build and install pbx_realtime (default=yes)])],,
	[with_pbx_realtime="yes"])

AM_CONDITIONAL([WANT_PBX_REALTIME],[test "${with_pbx_realtime}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([pbx_ael],
	[AC_HELP_STRING([--with-pbx_ael],[Build and install pbx_ael (Experimental) (default=no)])],,
	[with_pbx_ael="no"])

AM_CONDITIONAL([WANT_PBX_AEL],[test "${with_pbx_ael}" = "yes"])

dnl ###########################################################################
dnl ###########################################################################
dnl ###
dnl ### Resources
dnl ###
dnl ###########################################################################
dnl ###########################################################################

AC_ARG_WITH([res_config_odbc],
        [AC_HELP_STRING([--with-res_config_odbc],[Build and install res_config_odbc (default=no) (requires --enable-odbc and --with-res_odbc)])],,[with_res_config_odbc="no"])

AM_CONDITIONAL([WANT_RES_CONFIG_ODBC],[test "${enable_odbc}" = "yes" -a "${with_res_config_odbc}" = "yes" -a "${with_res_odbc}" = "yes" -a -n "${HAVE_LIBODBC}"])

dnl ###########################################################################

AC_ARG_WITH([res_config_curl],
        [AC_HELP_STRING([--with-res_config_curl],[Build and install res_config_curl (default=no)])],,[with_res_config_curl="no"])

AM_CONDITIONAL([WANT_RES_CONFIG_CURL],[test "${with_res_config_curl}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH(res_config_mysql,
	[AC_HELP_STRING([--with-res_config_mysql],[Build and install res_config_mysql (default=no) (requires --enable-mysql])],,[with_res_config_mysql="no"])

AM_CONDITIONAL([WANT_RES_CONFIG_MYSQL],[test "${enable_mysql}" = "yes" -a "${with_res_config_mysql}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH(res_config_pgsql,
	[AC_HELP_STRING([--with-res_config_pgsql],[Build and install res_config_pgsql (default=no) (requires --enable-postgresql])],,[with_res_config_pgsql="no"])

AM_CONDITIONAL([WANT_RES_CONFIG_PGSQL],[test "${enable_postgresql}" = "yes" -a "${with_res_config_pgsql}" = "yes"])

dnl ###########################################################################

AC_ARG_WITH([res_jabber],
	[AC_HELP_STRING([--with-res_jabber],[Build and install res_jabber (default=no) (requires --enable-jabber])],,[with_res_jabber="no"])

AM_CONDITIONAL([WANT_RES_JABBER],[test "${enable_jabber}" = "yes" -a "${with_res_jabber}" = "yes" -a -n "${HAVE_LOUDMOUTH}"])

dnl ###########################################################################

AC_ARG_WITH([res_js],
	[AC_HELP_STRING([--with-res_js],[Build and install res_js (default=no) (requires --enable-javascript)])],,[with_res_js="no"])

AM_CONDITIONAL([WANT_RES_JS],[test "${enable_javascript}" = "yes" -a "${with_res_js}" = "yes" -a -n "${HAVE_LIBJS}" -a \( -n "${HAVE_JSSCRIPT_H}" -o -n "${HAVE_JS_JSSCRIPT_H}" \) -a -n "${HAVE_NSPR}"])  

dnl ###########################################################################

AC_ARG_WITH([res_odbc],
	[AC_HELP_STRING([--with-res_odbc],[Build and install res_odbc (default=no) (requires --enable-odbc)])],,
	[with_res_odbc="no"])

AM_CONDITIONAL([WANT_RES_ODBC],[test "${enable_odbc}" = "yes" -a "${with_res_odbc}" = "yes" -a -n "${HAVE_LIBODBC}"])

dnl ###########################################################################

dnl res_osp ... open settlement protocol handling
dnl look for Transnexus OSP Toolkit (stolen from the OpenH323 configure.ac)

dnl MSWIN_DISPLAY    transnexus_osp,Transnexus OSP Toolkit
dnl MSWIN_CHECK_FILE transnexus_osp,include/osp/ospcryptowrap.h,H323_TRANSNEXUS_OSP=1
dnl MSWIN_DIR_SYMBOL transnexus_osp,H323_TRANSNEXUS_OSP_DIR
dnl MSWIN_CHECK_DIR  transnexus_osp,lib\
dnl MSWIN_CHECK_DIR  transnexus_osp,pthread\lib
dnl MSWIN_CHECK_DIR  transnexus_osp,pthread\include

AC_ARG_ENABLE(transnexusosp,
       [AC_HELP_STRING([--disable-transnexusosp],[disable support for the Transnexus OSP toolkit])],,[enable_transnexusosp="no"])

if test "${enable_transnexusosp}z" = "z" ; then
  enable_transnexusosp="yes"
fi

if test "x$enable_transnexusosp" = "xno" ; then
  AC_MSG_NOTICE(Disabling support for the Transnexus OSP toolkit)
else
  AC_CHECK_HEADERS(osp/osp.h, HAS_TRANSNEXUS_OSP=1)
  if test "${HAS_TRANSNEXUS_OSP}x" = "1x" ; then
    AC_CHECK_LIB(osp, OSPPInit, HAS_TRANSNEXUS_OSP=1, HAS_TRANSNEXUS_OSP=, -lssl)
    if test "${HAS_TRANSNEXUS_OSP}x" = "1x" ; then
      TRANSNEXUS_OSP=1
      AC_DEFINE([TRANSNEXUS_OSP],"1",[yes we have osp ...])
      OSP_LIBS="-losp"
      OSP_INCLUDE="#include <osp/osp.h>"
    else
      unset ac_cv_lib_osp_OSPPInit
      AC_CHECK_LIB(osp, OSPPInit, HAS_TRANSNEXUS_OSP=1, HAS_TRANSNEXUS_OSP=, -lssl)
      if test "${HAS_TRANSNEXUS_OSP}x" = "x" ; then
         unset TRANSNEXUS_OSP
      else
        TRANSNEXUS_OSP=1
        AC_DEFINE([TRANSNEXUS_OSP],"1",[yes we have OSP])
        OSP_CFLAGS="-I/usr/local/include"
        OSP_LDLIBS="${LDLIBS} -losp"
        OSP_INCLUDE='#include "/usr/local/include/osp/osp.h"'
      fi
    fi
  fi
fi

AC_SUBST(TRANSNEXUS_OSP)
AC_SUBST(OSP_LIBS)
AC_SUBST(OSP_INCLUDE)
AC_SUBST(OSP_CFLAGS)
AM_CONDITIONAL([WANT_OSP],[test -n "${TRANSNEXUS_OSP}"])

dnl ###########################################################################

AC_ARG_WITH(res_sqlite,
        [AC_HELP_STRING([--with-res_sqlite],[Build and install res_sqlite (default=no)])],,[with_res_sqlite="no"])

AM_CONDITIONAL([WANT_RES_SQLITE],[test "${with_res_sqlite}" = "yes"])

dnl ###########################################################################

AC_SUBST(SVN)
AC_SUBST(GREP)
AC_SUBST(GROUPADD)
AC_SUBST(SOLINK)
AC_SUBST(ASTLINK)
AC_SUBST(CPU_OPTIONS)
AC_SUBST(all_debug)
AC_SUBST(DEBUG_THREADS)
AC_SUBST(MALLOC_DEBUG)
AC_SUBST(LOW_MEMORY)
AC_SUBST(prefix)

AC_SUBST_FILE(substitute)
substitute=${srcdir}/substitute.mak

AM_CONFIG_HEADER(include/confdefs.h)  

######
#
# Define in which subdirectories we have to run configure as well
#
######
AC_CONFIG_SUBDIRS(libltdl)

AC_OUTPUT([Makefile
		 include/Makefile
		 include/openpbx/Makefile
		 libs/Makefile
                 ogi/Makefile
                 apps/Makefile
                 apps/icd/Makefile
		 apps/nconference/Makefile
                 cdr/Makefile
                 channels/Makefile
                 channels/iax2/Makefile
                 channels/sccp/Makefile
                 channels/fax/Makefile
                 channels/misdn/Makefile
                 codecs/Makefile
 		 corelib/Makefile
 		 corelib/jitterbuffer/Makefile
                 formats/Makefile
                 funcs/Makefile
                 pbx/Makefile
                 res/Makefile
                 stdtime/Makefile
                 utils/Makefile
		 configs/musiconhold.conf.sample
		 configs/Makefile
 		 ogi/ogi-test.ogi
		 ogi/fastogi-test
])

if test "${builtin_sqlite3}" = "yes"; then
	AC_OUTPUT([	 sqlite3-embedded/Makefile
			 sqlite3-embedded/mkopcodec.awk
			 sqlite3-embedded/mkopcodeh.awk
	])
fi


echo ""
echo "   ************************"
echo "   *   Configure status   *"
echo "   ************************"
echo ""

echo "Loudmouth/Jabber Support:  ${HAVE_LIBLOUDMOUTH}"
if test "x${HAVE_LIBLOUDMOUTH}" = "xyes" ; then
        echo "Loudmouth CFLAGS: ${LOUDMOUTH_CFLAGS}"
        echo "Loudmouth LIBS:     ${LOUDMOUTH_LIBS}"
fi     

echo "CAPI Channel module: ${WANT_CHAN_CAPI}"

echo ""

echo "CFLAGS: 		$CFLAGS"
echo "LIBS: 		$LIBS"
echo "LDFLAGS: 		$LDFLAGS"
echo "BUSYDETECT: 	${BUSYDETECT}"
echo "CPU_OPTIONS: 	${CPU_OPTIONS}"
echo "BUILD_OS: 	${BUILD_OS}"
echo "BUILD_MACHINE: 	${BUILD_MACHINE}"
echo "BUILD_HOST: 	${BUILD_HOST}"
echo "SVN:		${SVN}"
echo "SVN_VERSION:	${SVN_VERSION}"
echo ""
echo "prefix:           ${prefix}"
echo "exec_prefix:      ${exec_prefix}"
echo ""
echo "bindir:           ${bindir}"
echo "sbindir:          ${sbindir}"
echo "libdir:           ${libdir}"
echo "sysconfdir:       ${sysconfdir}"
echo "localstatedir:    ${localstatedir}"
echo "datadir:          ${datadir}"
echo "includedir:       ${includedir}"
echo ""
echo "opbxconfdir:      ${opbxconfdir}"
echo "opbxconffile:     ${opbxconffile}"
echo "opbxlibdir:       ${opbxlibdir}"
echo "opbxmoddir:       ${opbxmoddir}"
echo "opbxvardir:       ${opbxvardir}"
echo "opbxdbdir:        ${opbxdbdir}"
echo "opbxdbfile:       ${opbxdbfile}"
echo "opbxtmpdir:       ${opbxtmpdir}"
echo "opbxrundir:       ${opbxrundir}"
echo "opbxpidfile:      ${opbxpidfile}"
echo "opbxsocketfile:   ${opbxsocketfile}"
echo "opbxlogdir:       ${opbxlogdir}"
echo "opbxspooldir:     ${opbxspooldir}"
echo "opbxdatadir:      ${opbxdatadir}"
echo "opbxdocdir:       ${opbxdocdir}"
echo "opbxkeydir:       ${opbxkeydir}"
echo "opbxsqlitedir:    ${opbxsqlitedir}"
echo "opbxogidir:       ${opbxogidir}"
echo "opbxsoundsdir:    ${opbxsoundsdir}"
echo "opbximagesdir:    ${opbximagesdir}"
echo "opbxmohdir:       ${opbxmohdir}"
echo "opbxincludedir:   ${opbxincludedir}"
