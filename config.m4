PHP_ARG_ENABLE(SPX, whether to enable SPX extension,
[ --enable-spx   Enable SPX extension])

PHP_ARG_ENABLE(SPX-DEV, whether to enable SPX developer build flags,
[  --enable-spx-dev   Compile SPX with debugging symbols])

if test -z "$PHP_ZLIB_DIR"; then
PHP_ARG_WITH(zlib-dir, for ZLIB,
[  --with-zlib-dir[=DIR]   Set the path to ZLIB install prefix.], no)
fi

PHP_ARG_WITH(spx-assets-dir, for assets path,
[  --with-spx-assets-dir[=DIR]   Set the installation path of assets.], $prefix/share/misc/php-spx/assets)

PHP_ARG_WITH(spx-pgsql, for PostgreSQL support in SPX,
[  --with-spx-pgsql[=DIR]   Include PostgreSQL support in SPX], no, no)

if test "$PHP_SPX" = "yes"; then
    AC_DEFINE(HAVE_SPX, 1, [spx])
    AC_MSG_CHECKING([for assets directory])
    AC_MSG_RESULT([ $PHP_SPX_ASSETS_DIR ])
    AC_DEFINE_UNQUOTED([SPX_HTTP_UI_ASSETS_DIR], [ "$PHP_SPX_ASSETS_DIR/web-ui" ], [path of web-ui assets directory])
    PHP_SUBST([PHP_SPX_ASSETS_DIR])

    CFLAGS="-Werror -Wall -O3 -pthread -std=gnu90"

    if test "$(uname -s 2>/dev/null)" = "Darwin"
    then
        # see discussion here https://github.com/NoiseByNorthwest/php-spx/pull/270
        CFLAGS="$CFLAGS -Wno-typedef-redefinition"
    fi

    if test "$PHP_SPX_DEV" = "yes"
    then
        CFLAGS="$CFLAGS -g"
    fi

    AC_MSG_CHECKING([for zlib header])
    if test "$PHP_ZLIB_DIR" != "no" && test "$PHP_ZLIB_DIR" != "yes"; then
        if test -f "$PHP_ZLIB_DIR/include/zlib/zlib.h"; then
            PHP_ZLIB_DIR="$PHP_ZLIB_DIR"
            PHP_ZLIB_INCDIR="$PHP_ZLIB_DIR/include/zlib"
        elif test -f "$PHP_ZLIB_DIR/include/zlib.h"; then
            PHP_ZLIB_DIR="$PHP_ZLIB_DIR"
            PHP_ZLIB_INCDIR="$PHP_ZLIB_DIR/include"
        else
            AC_MSG_ERROR([Can't find ZLIB headers under "$PHP_ZLIB_DIR"])
        fi
    else
        for i in /usr/local /usr /opt/local; do
            if test -f "$i/include/zlib/zlib.h"; then
                PHP_ZLIB_DIR="$i"
                PHP_ZLIB_INCDIR="$i/include/zlib"
            elif test -f "$i/include/zlib.h"; then
                PHP_ZLIB_DIR="$i"
                PHP_ZLIB_INCDIR="$i/include"
            fi
        done
    fi

    AC_MSG_CHECKING([for zlib location])
    if test "$PHP_ZLIB_DIR" != "no" && test "$PHP_ZLIB_DIR" != "yes"; then
        AC_MSG_RESULT([$PHP_ZLIB_DIR])
        PHP_ADD_LIBRARY_WITH_PATH(z, $PHP_ZLIB_DIR/$PHP_LIBDIR, SPX_SHARED_LIBADD)
        PHP_ADD_INCLUDE($PHP_ZLIB_INCDIR)
    else
        AC_MSG_ERROR([spx support requires ZLIB. Use --with-zlib-dir=<DIR> to specify the prefix where ZLIB headers and library are located])
    fi

    dnl PostgreSQL support
    if test "$PHP_SPX_PGSQL" != "no"; then
        AC_DEFINE(SPX_STORAGE_POSTGRESQL_ENABLED, 1, [PostgreSQL storage support])

        if test "$PHP_SPX_PGSQL" = "yes"; then
            dnl Try to use pg_config if available
            AC_PATH_PROG(PG_CONFIG, pg_config, no)
            if test "$PG_CONFIG" != "no"; then
                PHP_SPX_PGSQL_INCDIR=`$PG_CONFIG --includedir`
                PHP_SPX_PGSQL_LIBDIR=`$PG_CONFIG --libdir`
                if test -f "$PHP_SPX_PGSQL_INCDIR/libpq-fe.h"; then
                    PHP_SPX_PGSQL_DIR=`dirname $PHP_SPX_PGSQL_INCDIR`
                fi
            fi

            dnl Fallback to manual search if pg_config didn't work
            if test -z "$PHP_SPX_PGSQL_DIR"; then
                for i in /usr /usr/local /opt /opt/local; do
                    if test -f "$i/include/libpq-fe.h"; then
                        PHP_SPX_PGSQL_DIR="$i"
                        break
                    elif test -f "$i/include/postgresql/libpq-fe.h"; then
                        PHP_SPX_PGSQL_DIR="$i"
                        PHP_SPX_PGSQL_INCDIR="$i/include/postgresql"
                        break
                    fi
                done
            fi
        else
            PHP_SPX_PGSQL_DIR="$PHP_SPX_PGSQL"
        fi

        if test -z "$PHP_SPX_PGSQL_DIR"; then
            AC_MSG_ERROR([libpq-fe.h not found. Please install PostgreSQL development libraries or specify correct path with --with-spx-pgsql=<DIR>])
        fi

        AC_MSG_CHECKING([for PostgreSQL support])

        dnl Check multiple possible locations for libpq-fe.h
        PHP_SPX_PGSQL_HEADER_FOUND="no"
        CHECKED_PATHS=""

        if test -z "$PHP_SPX_PGSQL_INCDIR"; then
            dnl Check standard include subdirectory
            if test -f "$PHP_SPX_PGSQL_DIR/include/libpq-fe.h"; then
                PHP_SPX_PGSQL_INCDIR="$PHP_SPX_PGSQL_DIR/include"
                PHP_SPX_PGSQL_HEADER_FOUND="yes"
            fi
            CHECKED_PATHS="$CHECKED_PATHS $PHP_SPX_PGSQL_DIR/include/libpq-fe.h"

            dnl Check direct path (for cases like /usr/include/postgresql)
            if test "$PHP_SPX_PGSQL_HEADER_FOUND" = "no" && test -f "$PHP_SPX_PGSQL_DIR/libpq-fe.h"; then
                PHP_SPX_PGSQL_INCDIR="$PHP_SPX_PGSQL_DIR"
                PHP_SPX_PGSQL_HEADER_FOUND="yes"
            fi
            CHECKED_PATHS="$CHECKED_PATHS $PHP_SPX_PGSQL_DIR/libpq-fe.h"

            dnl Check postgresql subdirectory
            if test "$PHP_SPX_PGSQL_HEADER_FOUND" = "no" && test -f "$PHP_SPX_PGSQL_DIR/include/postgresql/libpq-fe.h"; then
                PHP_SPX_PGSQL_INCDIR="$PHP_SPX_PGSQL_DIR/include/postgresql"
                PHP_SPX_PGSQL_HEADER_FOUND="yes"
            fi
            CHECKED_PATHS="$CHECKED_PATHS $PHP_SPX_PGSQL_DIR/include/postgresql/libpq-fe.h"
        else
            dnl Include directory was already determined (e.g., by pg_config)
            if test -f "$PHP_SPX_PGSQL_INCDIR/libpq-fe.h"; then
                PHP_SPX_PGSQL_HEADER_FOUND="yes"
            fi
            CHECKED_PATHS="$PHP_SPX_PGSQL_INCDIR/libpq-fe.h"
        fi

        if test "$PHP_SPX_PGSQL_HEADER_FOUND" = "yes"; then
            AC_MSG_RESULT([yes])
            PHP_ADD_INCLUDE($PHP_SPX_PGSQL_INCDIR)

            dnl Determine library directory if not already set
            if test -z "$PHP_SPX_PGSQL_LIBDIR"; then
                if test -d "$PHP_SPX_PGSQL_DIR/$PHP_LIBDIR"; then
                    PHP_SPX_PGSQL_LIBDIR="$PHP_SPX_PGSQL_DIR/$PHP_LIBDIR"
                elif test -d "$PHP_SPX_PGSQL_DIR/lib"; then
                    PHP_SPX_PGSQL_LIBDIR="$PHP_SPX_PGSQL_DIR/lib"
                else
                    PHP_SPX_PGSQL_LIBDIR="$PHP_SPX_PGSQL_DIR/$PHP_LIBDIR"
                fi
            fi

            PHP_CHECK_LIBRARY(pq, PQconnectdb,
            [
                PHP_ADD_LIBRARY_WITH_PATH(pq, $PHP_SPX_PGSQL_LIBDIR, SPX_SHARED_LIBADD)
            ], [
                AC_MSG_ERROR([PostgreSQL library 'libpq' not found in $PHP_SPX_PGSQL_LIBDIR])
            ], [
                -L$PHP_SPX_PGSQL_LIBDIR
            ])
        else
            AC_MSG_ERROR([libpq-fe.h not found. Checked paths:$CHECKED_PATHS])
        fi
    fi

    SPX_SOURCES="src/php_spx.c               \
        src/spx_profiler.c          \
        src/spx_profiler_tracer.c   \
        src/spx_profiler_sampler.c  \
        src/spx_reporter_full.c     \
        src/spx_reporter_fp.c       \
        src/spx_reporter_trace.c    \
        src/spx_metric.c            \
        src/spx_resource_stats.c    \
        src/spx_hmap.c              \
        src/spx_str_builder.c       \
        src/spx_output_stream.c     \
        src/spx_php.c               \
        src/spx_stdio.c             \
        src/spx_config.c            \
        src/spx_utils.c             \
        src/spx_fmt.c               \
        src/spx_storage.c           \
        src/spx_storage_fs.c"

    if test "$PHP_SPX_PGSQL" != "no"; then
        SPX_SOURCES="$SPX_SOURCES src/spx_storage_pgsql.c"
    fi

    PHP_NEW_EXTENSION(spx, $SPX_SOURCES, $ext_shared)

    PHP_ADD_MAKEFILE_FRAGMENT
fi
