AC_PREREQ([2.69])
AC_INIT([whisperlib], [1.0])
AM_INIT_AUTOMAKE([foreign color-tests dist-bzip2 parallel-tests subdir-objects])

AC_CONFIG_FILES([Makefile])
AC_CONFIG_HEADERS([config.h])

AC_PROG_CC
AC_PROG_CXX

AC_PROG_YACC
AC_PROG_LEX
AC_PROG_RANLIB

AC_LANG(C++)

# Checks for programs.
AC_PROG_CXX
AC_PROG_CC

AX_CXX_COMPILE_STDCXX_11([noext],[mandatory])
CXX_FLAG_CHECK([-stdlib=libc++])

AC_C_BIGENDIAN

# Checks for libraries.

# Checks for header files.
AC_CHECK_HEADERS([arpa/inet.h fcntl.h float.h inttypes.h limits.h memory.h netdb.h netinet/in.h stddef.h stdint.h stdlib.h string.h strings.h sys/param.h sys/socket.h sys/time.h unistd.h gflags/gflags.h glog/logging.h nameser8_compat.h endian.h])

AC_CHECK_HEADERS([unordered_set tr1/unordered_set ext/hash_set unordered_map tr1/unordered_map ext/hash_map functional_hash.h tr1/functional_hash.h ext/hash_fun.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_CHECK_HEADER_STDBOOL
AC_C_INLINE
AC_TYPE_INT16_T
AC_TYPE_INT32_T
AC_TYPE_INT64_T
AC_TYPE_INT8_T
AC_TYPE_MODE_T
AC_TYPE_OFF_T
AC_TYPE_PID_T
AC_TYPE_SIZE_T
AC_TYPE_SSIZE_T
AC_STRUCT_TIMEZONE
AC_TYPE_UINT16_T
AC_TYPE_UINT32_T
AC_TYPE_UINT64_T
AC_TYPE_UINT8_T
AC_CHECK_TYPES([ptrdiff_t])

# Checks for library functions.
AC_FUNC_ERROR_AT_LINE
AC_FUNC_FORK
AC_FUNC_LSTAT_FOLLOWS_SLASHED_SYMLINK
AC_FUNC_MALLOC
AC_FUNC_MKTIME
AC_FUNC_MMAP
AC_FUNC_REALLOC
AC_FUNC_STRTOD
AC_CHECK_FUNCS([atexit bzero clock_gettime ftruncate getcwd gethostbyname gettimeofday localeconv localtime_r memmove memset mkdir regcomp rmdir setlocale socket strcasecmp strchr strdup strerror strncasecmp strpbrk strrchr strtol strtoul strtoull])

AC_OUTPUT

AC_OUTPUT