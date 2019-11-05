#
# Copyright (C) Mellanox Technologies Ltd. 2001-2014.  ALL RIGHTS RESERVED.
# Copyright (C) UT-Battelle, LLC. 2014-2015. ALL RIGHTS RESERVED.
# Copyright (C) The University of Tennessee and the University of Tennessee Research Foundation. 2016. ALL RIGHTS RESERVED.
#
# See file LICENSE for terms.
#


AC_ARG_WITH([verbs],
        [AC_HELP_STRING([--with-verbs(=DIR)],
            [Build OpenFabrics support, adding DIR/include, DIR/lib, and DIR/lib64 to the search path for headers and libraries])],
        [],
        [with_verbs=/usr])

AS_IF([test "x$with_verbs" == "xyes"], [with_verbs=/usr])
AS_IF([test -d "$with_verbs"], [with_ib=yes; str="with verbs support from $with_verbs"], [with_ib=no; str="without verbs support"])
AS_IF([test -d "$with_verbs/lib64"],[libsuff="64"],[libsuff=""])

AC_MSG_NOTICE([Compiling $str])

#
# Check basic IB support: User wanted at least one IB transport, and we found
# verbs header file and library.
#
AS_IF([test "x$with_ib" == xyes],
        [
        save_LDFLAGS="$LDFLAGS"
        save_CFLAGS="$CFLAGS"
        save_CPPFLAGS="$CPPFLAGS"
        AS_IF([test x/usr == "x$with_verbs"],
          [],
          [verbs_incl="-I$with_verbs/include"
           verbs_libs="-L$with_verbs/lib$libsuff"])
        LDFLAGS="$verbs_libs $LDFLAGS"
        CFLAGS="$verbs_incl $CFLAGS"
        CPPFLAGS="$verbs_incl $CPPFLAGS"
        AC_CHECK_HEADER([infiniband/verbs.h], [],
                        [AC_MSG_WARN([ibverbs header files not found]); with_ib=no])
        AC_CHECK_LIB([ibverbs], [ibv_get_device_list],
            [
            AC_SUBST(IBVERBS_LIBS,  ["$verbs_libs -libverbs -lmlx5"])
            AC_SUBST(IBVERBS_DIR,      ["$with_verbs"])
            AC_SUBST(IBVERBS_CPPFLAGS, ["$verbs_incl"])
            AC_SUBST(IBVERBS_CFLAGS,   ["$verbs_incl"])
            ],
            [AC_MSG_WARN([libibverbs not found]); with_ib=no])

        have_ib_funcs=yes
        LDFLAGS="$LDFLAGS $IBVERBS_LDFLAGS"
        AC_CHECK_DECLS([ibv_wc_status_str, \
                        ibv_event_type_str, \
                        ibv_query_gid, \
                        ibv_get_device_name, \
                        ibv_create_srq, \
                        ibv_get_async_event],
                       [],
                       [have_ib_funcs=no],
                       [#include <infiniband/verbs.h>])
        AS_IF([test "x$have_ib_funcs" != xyes],
              [AC_MSG_WARN([Some IB verbs are not found. Please make sure OFED version is 1.5 or above.])
               with_ib=no])

        LDFLAGS="$save_LDFLAGS"
        CFLAGS="$save_CFLAGS"
        CPPFLAGS="$save_CPPFLAGS"
        ],[:])

#
# For automake
#
AM_CONDITIONAL([HAVE_IB],      [test "x$with_ib" != xno])
