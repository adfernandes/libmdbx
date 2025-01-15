/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2025

#define xMDBX_ALLOY 1  /* alloyed build */
#include "internals.h" /* must be included first */

#include "api-cold.c"
#include "api-copy.c"
#include "api-cursor.c"
#include "api-dbi.c"
#include "api-env.c"
#include "api-extra.c"
#include "api-key-transform.c"
#include "api-misc.c"
#include "api-opts.c"
#include "api-range-estimate.c"
#include "api-txn-data.c"
#include "api-txn.c"
#include "audit.c"
#include "chk.c"
#include "cogs.c"
#include "coherency.c"
#include "cursor.c"
#include "dbi.c"
#include "dpl.c"
#include "dxb.c"
#include "env.c"
#include "gc-get.c"
#include "gc-put.c"
#include "global.c"
#include "lck-posix.c"
#include "lck-windows.c"
#include "lck.c"
#include "logging_and_debug.c"
#include "meta.c"
#include "mvcc-readers.c"
#include "node.c"
#include "osal.c"
#include "page-get.c"
#include "page-iov.c"
#include "page-ops.c"
#include "pnl.c"
#include "refund.c"
#include "spill.c"
#include "table.c"
#include "tls.c"
#include "tree-ops.c"
#include "tree-search.c"
#include "txl.c"
#include "txn.c"
#include "utils.c"
#include "version.c"
#include "walk.c"
#include "windows-import.c"
