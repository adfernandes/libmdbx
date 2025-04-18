/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2025
/// \copyright SPDX-License-Identifier: Apache-2.0

#include "test.h++"

const char *testcase2str(const actor_testcase testcase) {
  switch (testcase) {
  default:
    assert(false);
    return "?!";
  case ac_none:
    return "none";
  case ac_hill:
    return "hill";
  case ac_deadread:
    return "dead.reader";
  case ac_deadwrite:
    return "dead.writer";
  case ac_jitter:
    return "jitter";
  case ac_try:
    return "try";
  case ac_copy:
    return "copy";
  case ac_append:
    return "append";
  case ac_ttl:
    return "ttl";
  case ac_nested:
    return "nested";
#if !defined(_WIN32) && !defined(_WIN64)
  case ac_forkread:
    return "fork.reader";
  case ac_forkwrite:
    return "fork.writer";
#endif /* Windows */
  }
}

const char *status2str(actor_status status) {
  switch (status) {
  default:
    assert(false);
    return "?!";
  case as_debugging:
    return "debugging";
  case as_running:
    return "running";
  case as_successful:
    return "successful";
  case as_killed:
    return "killed";
  case as_failed:
    return "failed";
  case as_coredump:
    return "coredump";
  }
}

const char *keygencase2str(const keygen_case keycase) {
  switch (keycase) {
  default:
    assert(false);
    return "?!";
  case kc_random:
    return "random";
  case kc_dashes:
    return "dashes";
  case kc_custom:
    return "custom";
  }
}

//-----------------------------------------------------------------------------

int testcase::hsr_callback(const MDBX_env *env, const MDBX_txn *txn, mdbx_pid_t pid, mdbx_tid_t tid, uint64_t laggard,
                           unsigned gap, size_t space, int retry) MDBX_CXX17_NOEXCEPT {
  (void)txn;
  testcase *self = (testcase *)mdbx_env_get_userctx(env);

  if (retry == 0)
    log_notice("hsr_callback: waitfor pid %lu, thread %" PRIuPTR ", txn #%" PRIu64 ", gap %d, space %zu", (long)pid,
               (size_t)tid, laggard, gap, space);

  MDBX_envinfo info;
  int rc = mdbx_env_info_ex(env, txn, &info, sizeof(info));
  if (rc != MDBX_SUCCESS)
    return rc;

  if (self->should_continue(true) &&
      (space > size_t(info.mi_geo.grow) * 2 || info.mi_geo.current >= info.mi_geo.upper)) {
    osal_yield();
    if (retry > 0)
      osal_udelay(retry * size_t(100));
    return MDBX_RESULT_FALSE /* retry / wait until reader done */;
  }

  /* allow growth or MDBX_MAP_FULL */
  return MDBX_RESULT_TRUE;
}

void testcase::db_prepare() {
  log_trace(">> db_prepare");
  assert(!db_guard);

  MDBX_env *env = nullptr;
  int rc = mdbx_env_create(&env);
  if (unlikely(rc != MDBX_SUCCESS))
    failure_perror("mdbx_env_create()", rc);

  assert(env != nullptr);
  db_guard.reset(env);

  rc = mdbx_env_set_userctx(env, this);
  if (unlikely(rc != MDBX_SUCCESS))
    failure_perror("mdbx_env_set_userctx()", rc);

  rc = mdbx_env_set_maxreaders(env, config.params.max_readers);
  if (unlikely(rc != MDBX_SUCCESS))
    failure_perror("mdbx_env_set_maxreaders()", rc);

  rc = mdbx_env_set_maxdbs(env, config.params.max_tables);
  if (unlikely(rc != MDBX_SUCCESS))
    failure_perror("mdbx_env_set_maxdbs()", rc);

  rc = mdbx_env_set_hsr(env, testcase::hsr_callback);
  if (unlikely(rc != MDBX_SUCCESS))
    failure_perror("mdbx_env_set_hsr()", rc);

  rc = mdbx_env_set_geometry(env, config.params.size_lower, config.params.size_now, config.params.size_upper,
                             config.params.growth_step, config.params.shrink_threshold, config.params.pagesize);
  if (unlikely(rc != MDBX_SUCCESS))
    failure_perror("mdbx_env_set_mapsize()", rc);

  log_trace("<< db_prepare");
}

void testcase::db_open() {
  log_trace(">> db_open");

  if (!db_guard)
    db_prepare();

  jitter_delay(true);

  MDBX_env_flags_t mode = config.params.mode_flags;
  if (config.params.random_writemap && flipcoin())
    mode ^= MDBX_WRITEMAP;

  int rc = mdbx_env_open(db_guard.get(), config.params.pathname_db.c_str(), mode, 0640);
  if (unlikely(rc != MDBX_SUCCESS))
    failure_perror("mdbx_env_open()", rc);

  unsigned env_flags_proxy;
  rc = mdbx_env_get_flags(db_guard.get(), &env_flags_proxy);
  if (unlikely(rc != MDBX_SUCCESS))
    failure_perror("mdbx_env_get_flags()", rc);
  actual_env_mode = MDBX_env_flags_t(env_flags_proxy);

  rc = mdbx_env_set_syncperiod(db_guard.get(), unsigned(0.042 * 65536));
  if (unlikely(rc != MDBX_SUCCESS) && rc != MDBX_BUSY)
    failure_perror("mdbx_env_set_syncperiod()", rc);

  rc = mdbx_env_set_syncbytes(db_guard.get(), INT_MAX / 421);
  if (unlikely(rc != MDBX_SUCCESS) && rc != MDBX_BUSY)
    failure_perror("mdbx_env_set_syncbytes()", rc);

  log_trace("<< db_open");
}

void testcase::db_close() {
  log_trace(">> db_close");
  cursor_guard.reset();
  txn_guard.reset();
  db_guard.reset();
  log_trace("<< db_close");
}

void testcase::txn_begin(bool readonly, MDBX_txn_flags_t flags) {
  assert((flags & MDBX_TXN_RDONLY) == 0);
  log_trace(">> txn_begin(%s, 0x%04X)", readonly ? "read-only" : "read-write", flags);
  assert(!txn_guard);

  MDBX_txn *txn = nullptr;
  int rc = mdbx_txn_begin(db_guard.get(), nullptr, readonly ? flags | MDBX_TXN_RDONLY : flags, &txn);
  if (unlikely(rc != MDBX_SUCCESS))
    failure_perror("mdbx_txn_begin()", rc);
  txn_guard.reset(txn);
  need_speculum_assign = config.params.speculum && !readonly;

  log_trace("<< txn_begin(%s, 0x%04X)", readonly ? "read-only" : "read-write", flags);

  if (flipcoin_n(5)) {
    const unsigned mask = unsigned(MDBX_warmup_default | MDBX_warmup_force | MDBX_warmup_oomsafe | MDBX_warmup_lock |
                                   MDBX_warmup_touchlimit);
    static unsigned counter;
    MDBX_warmup_flags_t warmup_flags = MDBX_warmup_flags_t((counter > MDBX_warmup_release) ? prng64() & mask : counter);
    counter += 1;
    int err = mdbx_env_warmup(db_guard.get(), txn, warmup_flags, 0);
    log_trace("== counter %u, env_warmup(flags %u), rc %d", counter, warmup_flags, err);
  }

  if (readonly && flipcoin())
    txn_probe_parking();
}

int testcase::breakable_commit() {
  log_trace(">> txn_commit");
  assert(txn_guard);

  /* CLANG/LLVM C++ library could stupidly copy std::set<> item-by-item,
   * i.e. with insertion(s) & comparison(s), which will cause null dereference
   * during call mdbx_cmp() with zero txn. So it is the workaround for this:
   *  - explicitly make copies of the `speculums`;
   *  - explicitly move relevant copy after transaction commit. */
  SET speculum_committed_copy(ItemCompare(this)), speculum_copy(ItemCompare(this));
  if (need_speculum_assign) {
    speculum_committed_copy = speculum_committed;
    speculum_copy = speculum;
  }

  MDBX_txn *txn = txn_guard.release();
  txn_inject_writefault(txn);
  int rc = mdbx_txn_commit(txn);
  if (unlikely(rc != MDBX_SUCCESS) && (rc != MDBX_MAP_FULL || !config.params.ignore_dbfull))
    failure_perror("mdbx_txn_commit()", rc);

  if (need_speculum_assign) {
    need_speculum_assign = false;
    if (unlikely(rc != MDBX_SUCCESS))
      speculum = std::move(speculum_committed_copy);
    else
      speculum_committed = std::move(speculum_copy);
  }

  log_trace("<< txn_commit: %s", rc ? "failed" : "Ok");
  return rc;
}

unsigned testcase::txn_underutilization_x256(MDBX_txn *txn) const {
  if (txn) {
    MDBX_txn_info info;
    int err = mdbx_txn_info(txn, &info, false);
    if (unlikely(err != MDBX_SUCCESS))
      failure_perror("mdbx_txn_info()", err);
    const size_t left = size_t(info.txn_space_leftover);
    const size_t total = size_t(info.txn_space_leftover) + size_t(info.txn_space_dirty);
    return (unsigned)(left / (total >> 8));
  }
  return 0;
}

void testcase::txn_end(bool abort) {
  log_trace(">> txn_end(%s)", abort ? "abort" : "commit");
  assert(txn_guard);

  if (flipcoin())
    txn_probe_parking();

  MDBX_txn *txn = txn_guard.release();
  if (abort) {
    int err = mdbx_txn_abort(txn);
    if (unlikely(err != MDBX_SUCCESS))
      failure_perror("mdbx_txn_abort()", err);
    if (need_speculum_assign)
      speculum = speculum_committed;
  } else {
    txn_inject_writefault(txn);
    int err = mdbx_txn_commit(txn);
    if (unlikely(err != MDBX_SUCCESS))
      failure_perror("mdbx_txn_commit()", err);
    if (need_speculum_assign)
      speculum_committed = speculum;
  }

  log_trace("<< txn_end(%s)", abort ? "abort" : "commit");
}

void testcase::cursor_open(MDBX_dbi handle) {
  log_trace(">> cursor_open(%u)", handle);
  assert(!cursor_guard);
  assert(txn_guard);

  MDBX_cursor *cursor = nullptr;
  int rc = mdbx_cursor_open(txn_guard.get(), handle, &cursor);
  if (unlikely(rc != MDBX_SUCCESS))
    failure_perror("mdbx_cursor_open()", rc);
  cursor_guard.reset(cursor);

  log_trace("<< cursor_open(%u)", handle);
}

void testcase::cursor_close() {
  log_trace(">> cursor_close()");
  assert(cursor_guard);
  MDBX_cursor *cursor = cursor_guard.release();
  mdbx_cursor_close(cursor);
  log_trace("<< cursor_close()");
}

void testcase::cursor_renew() {
  log_trace(">> cursor_renew()");
  assert(cursor_guard);
  int err = mdbx_cursor_renew(txn_guard.get(), cursor_guard.get());
  if (unlikely(err != MDBX_SUCCESS))
    failure_perror("mdbx_cursor_renew()", err);
  log_trace("<< cursor_renew()");
}

int testcase::breakable_restart() {
  int rc = MDBX_SUCCESS;
  if (txn_guard)
    rc = breakable_commit();
  if (flipcoin()) {
    txn_begin(true);
    txn_probe_parking();
    int err = mdbx_txn_abort(txn_guard.release());
    if (unlikely(err != MDBX_SUCCESS))
      failure_perror("mdbx_txn_abort()", err);
  }
  txn_begin(false, MDBX_TXN_READWRITE);
  if (cursor_guard)
    cursor_renew();
  return rc;
}

void testcase::txn_restart(bool abort, bool readonly, MDBX_txn_flags_t flags) {
  if (txn_guard)
    txn_end(abort);
  txn_begin(readonly, flags);
  if (cursor_guard)
    cursor_renew();
}

void testcase::txn_inject_writefault(void) {
  if (txn_guard)
    txn_inject_writefault(txn_guard.get());
}

void testcase::txn_inject_writefault(MDBX_txn *txn) {
  if (config.params.inject_writefaultn && txn) {
    if (config.params.inject_writefaultn <= nops_completed &&
        (MDBX_txn_flags_t(mdbx_txn_flags(txn)) & MDBX_TXN_RDONLY) == 0) {
      log_verbose("== txn_inject_writefault(): got %u nops or more, inject FAULT", config.params.inject_writefaultn);
      log_flush();
#if defined(_WIN32) || defined(_WIN64) || defined(_WINDOWS)
      TerminateProcess(GetCurrentProcess(), 42);
#else
      raise(SIGKILL);
#endif
    }
  }
}

bool testcase::wait4start() {
  if (config.wait4id) {
    log_trace(">> wait4start(%u)", config.wait4id);
    assert(!global::singlemode);
    int rc = osal_waitfor(config.wait4id);
    if (rc) {
      log_trace("<< wait4start(%u), failed %s", config.wait4id, test_strerror(rc));
      return false;
    }
  } else {
    log_trace("== skip wait4start: not needed");
  }

  if (config.params.delaystart) {
    int rc = osal_delay(config.params.delaystart);
    if (rc) {
      log_trace("<< delay(%u), failed %s", config.params.delaystart, test_strerror(rc));
      return false;
    }
  } else {
    log_trace("== skip delay: not needed");
  }

  return true;
}

void testcase::kick_progress(bool active) const {
  if (!global::config::progress_indicator)
    return;
  logging::progress_canary(active);
}

void testcase::report(size_t nops_done) {
  assert(nops_done > 0);
  if (!nops_done)
    return;

  nops_completed += nops_done;
  log_debug("== complete +%" PRIuPTR " iteration, total %" PRIu64 " done", nops_done, nops_completed);

  kick_progress(true);

  if (config.signal_nops && !signalled && config.signal_nops <= nops_completed) {
    log_trace(">> signal(n-ops %" PRIu64 ")", nops_completed);
    if (!global::singlemode)
      osal_broadcast(config.actor_id);
    signalled = true;
    log_trace("<< signal(n-ops %" PRIu64 ")", nops_completed);
  }
}

void testcase::signal() {
  if (!signalled) {
    log_trace(">> signal(forced)");
    if (!global::singlemode)
      osal_broadcast(config.actor_id);
    signalled = true;
    log_trace("<< signal(forced)");
  }
}

bool testcase::setup() {
  db_prepare();
  if (!wait4start())
    return false;

  start_timestamp = chrono::now_monotonic();
  nops_completed = 0;
  return true;
}

bool testcase::teardown() {
  log_trace(">> testcase::teardown");
  signal();
  db_close();
  log_trace("<< testcase::teardown");
  return true;
}

bool testcase::should_continue(bool check_timeout_only) const {
  bool result = true;

  if (config.params.test_duration) {
    chrono::time since;
    since.fixedpoint = chrono::now_monotonic().fixedpoint - start_timestamp.fixedpoint;
    if (since.seconds() >= config.params.test_duration)
      result = false;
  }

  if (!check_timeout_only && config.params.test_nops && nops_completed >= config.params.test_nops)
    result = false;

  if (result)
    kick_progress(false);

  return result;
}

void testcase::fetch_canary() {
  MDBX_canary canary_now;
  log_trace(">> fetch_canary");

  int rc = mdbx_canary_get(txn_guard.get(), &canary_now);
  if (unlikely(rc != MDBX_SUCCESS))
    failure_perror("mdbx_canary_get()", rc);

  if (canary_now.v < last.canary.v)
    failure("fetch_canary: %" PRIu64 "(canary-now.v) < %" PRIu64 "(canary-last.v)", canary_now.v, last.canary.v);
  if (canary_now.y < last.canary.y)
    failure("fetch_canary: %" PRIu64 "(canary-now.y) < %" PRIu64 "(canary-last.y)", canary_now.y, last.canary.y);

  last.canary = canary_now;
  log_trace("<< fetch_canary: db-sequence %" PRIu64 ", db-sequence.txnid %" PRIu64, last.canary.y, last.canary.v);
}

void testcase::update_canary(uint64_t increment) {
  MDBX_canary canary_now = last.canary;

  log_trace(">> update_canary: sequence %" PRIu64 " += %" PRIu64, canary_now.y, increment);
  canary_now.y += increment;

  int rc = mdbx_canary_put(txn_guard.get(), &canary_now);
  if (unlikely(rc != MDBX_SUCCESS))
    failure_perror("mdbx_canary_put()", rc);

  log_trace("<< update_canary: sequence = %" PRIu64, canary_now.y);
}

bool testcase::is_handle_created_in_current_txn(const MDBX_dbi handle, MDBX_txn *txn) {
  unsigned flags, state;
  int err = mdbx_dbi_flags_ex(txn, handle, &flags, &state);
  if (unlikely(err != MDBX_SUCCESS))
    failure_perror("mdbx_dbi_flags_ex()", err);
  return (state & MDBX_DBI_CREAT) != 0;
}

int testcase::db_open__begin__table_create_open_clean(MDBX_dbi &handle) {
  db_open();

  int err, retry_left = 42;
  for (;;) {
    txn_begin(false);
    handle = db_table_open(true);

    if (is_handle_created_in_current_txn(handle, txn_guard.get()))
      return MDBX_SUCCESS;
    db_table_clear(handle);
    err = breakable_commit();
    if (likely(err == MDBX_SUCCESS)) {
      txn_begin(false);
      return MDBX_SUCCESS;
    }
    if (--retry_left == 0)
      break;
    jitter_delay(true);
  }
  log_notice("db_begin_table_create_open_clean: bailout due '%s'", mdbx_strerror(err));
  return err;
}

const char *testcase::db_tablename(tablename_buf &buffer, const char *suffix) const {
  const char *tablename = nullptr;
  if (config.space_id) {
    int rc = snprintf(buffer, sizeof(buffer), "TBL%04u%s", config.space_id, suffix);
    if (rc < 4 || rc >= (int)sizeof(tablename_buf) - 1)
      failure("snprintf(tablename): %d", rc);
    tablename = buffer;
  }
  log_debug("use %s table", tablename ? tablename : "MAINDB");
  return tablename;
}

MDBX_dbi testcase::db_table_open(bool create, bool expect_failure) {
  log_trace(">> testcase::db_table_%s%s", create ? "create" : "open", expect_failure ? "(expect_failure)" : "");

  tablename_buf buffer;
  const char *tablename = db_tablename(buffer);

  MDBX_dbi handle = 0;
  int rc = mdbx_dbi_open(txn_guard.get(), tablename,
                         create ? (MDBX_CREATE | config.params.table_flags)
                                : (flipcoin() ? MDBX_DB_ACCEDE : MDBX_DB_DEFAULTS | config.params.table_flags),
                         &handle);
  if (unlikely(expect_failure != (rc != MDBX_SUCCESS))) {
    char act[64];
    snprintf(act, sizeof(act), "mdbx_dbi_open(create=%s,expect_failure=%s)", create ? "true" : "false",
             expect_failure ? "true" : "false");
    failure_perror(act, rc);
  }

  log_trace("<< testcase::db_table_%s%s, handle %u", create ? "create" : "open",
            expect_failure ? "(expect_failure)" : "", handle);
  return handle;
}

void testcase::db_table_drop(MDBX_dbi handle) {
  log_trace(">> testcase::db_table_drop, handle %u", handle);

  if (config.params.drop_table) {
    int rc = mdbx_drop(txn_guard.get(), handle, true);
    if (unlikely(rc != MDBX_SUCCESS))
      failure_perror("mdbx_drop(delete=true)", rc);
    speculum.clear();
    log_trace("<< testcase::db_table_drop");
  } else {
    log_trace("<< testcase::db_table_drop: not needed");
  }
}

void testcase::db_table_clear(MDBX_dbi handle, MDBX_txn *txn) {
  log_trace(">> testcase::db_table_clear, handle %u", handle);
  int err = mdbx_drop(txn ? txn : txn_guard.get(), handle, false);
  if (unlikely(err != MDBX_SUCCESS))
    failure_perror("mdbx_drop(delete=false)", err);
  speculum.clear();
  log_trace("<< testcase::db_table_clear");
}

void testcase::db_table_close(MDBX_dbi handle) {
  log_trace(">> testcase::db_table_close, handle %u", handle);
  assert(!txn_guard);
  int err = mdbx_dbi_close(db_guard.get(), handle);
  if (unlikely(err != MDBX_SUCCESS))
    failure_perror("mdbx_dbi_close()", err);
  log_trace("<< testcase::db_table_close");
}

bool testcase::checkdata(const char *step, MDBX_dbi handle, MDBX_val key2check, MDBX_val expected_valued) {
  MDBX_val actual_value = expected_valued;
  int err = mdbx_get_equal_or_great(txn_guard.get(), handle, &key2check, &actual_value);
  if (unlikely(err != MDBX_SUCCESS)) {
    if (!config.params.speculum || err != MDBX_RESULT_TRUE)
      failure_perror(step, (err == MDBX_RESULT_TRUE) ? MDBX_NOTFOUND : err);
    return false;
  }
  if (!is_samedata(&actual_value, &expected_valued))
    failure("%s data mismatch", step);
  return true;
}

//-----------------------------------------------------------------------------

#ifdef _MSC_VER

#include "dbghelp.h"
#pragma comment(lib, "Dbghelp.lib")

static void dump_stack(CONTEXT *ctx, FILE *out) {
  const int MaxNameLen = 256;

  BOOL result;
  HANDLE process;
  HANDLE thread;
  HMODULE hModule;
  STACKFRAME64 stack;
  ULONG frame;
  DWORD64 displacement;
  DWORD disp;

  char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR)];
  char module[MaxNameLen];
  PSYMBOL_INFO pSymbol = (PSYMBOL_INFO)buffer;

  // On x64, StackWalk64 modifies the context record, that could
  // cause crashes, so we create a copy to prevent it
  CONTEXT ctxCopy;
  memcpy(&ctxCopy, ctx, sizeof(CONTEXT));
  memset(&stack, 0, sizeof(STACKFRAME64));

  process = GetCurrentProcess();
  thread = GetCurrentThread();
  displacement = 0;
#if defined(_M_IX86)
  stack.AddrPC.Offset = (*ctx).Eip;
  stack.AddrPC.Mode = AddrModeFlat;
  stack.AddrStack.Offset = (*ctx).Esp;
  stack.AddrStack.Mode = AddrModeFlat;
  stack.AddrFrame.Offset = (*ctx).Ebp;
  stack.AddrFrame.Mode = AddrModeFlat;
#endif /* _M_IX86 */

  SymInitialize(process, NULL, TRUE);

  for (frame = 0;; frame++) {
    // get next call from stack
    result = StackWalk64(
#if defined(_M_AMD64)
        IMAGE_FILE_MACHINE_AMD64
#elif defined(_M_ARM64)
        IMAGE_FILE_MACHINE_ARM64
#elif defined(_M_ARM)
        IMAGE_FILE_MACHINE_ARM
#elif defined(_M_IX86)
        IMAGE_FILE_MACHINE_I386
#else
#error "FIXME"
#endif
        ,
        process, thread, &stack, &ctxCopy, NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL);

    if (!result)
      break;

    // get symbol name for address
    pSymbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    pSymbol->MaxNameLen = MAX_SYM_NAME;
    SymFromAddr(process, (ULONG64)stack.AddrPC.Offset, &displacement, pSymbol);

    IMAGEHLP_LINE64 line;
    line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);

    // try to get line
    if (SymGetLineFromAddr64(process, stack.AddrPC.Offset, &disp, &line)) {
      fprintf(out, "\tat %s in %s: line: %lu: address: 0x%0" PRIx64 "\n", pSymbol->Name, line.FileName, line.LineNumber,
              pSymbol->Address);
    } else {
      // failed to get line
      fprintf(out, "\tat %s, address 0x%0" PRIx64 ".\n", pSymbol->Name, pSymbol->Address);
      hModule = NULL;
      lstrcpyA(module, "");
      GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        (LPCTSTR)(stack.AddrPC.Offset), &hModule);

      // at least print module name
      if (hModule != NULL)
        GetModuleFileNameA(hModule, module, MaxNameLen);

      fprintf(out, "in %s\n", module);
    }
  }
  fflush(stderr);
}

static LONG seh_filter(struct _EXCEPTION_POINTERS *ExInfo, FILE *out) {
  const char *caption = "";
  switch (ExInfo->ExceptionRecord->ExceptionCode) {
  case EXCEPTION_BREAKPOINT:
    caption = "BREAKPOINT";
    break;
  case EXCEPTION_SINGLE_STEP:
    caption = "SINGLE STEPT";
    break;
  case STATUS_CONTROL_C_EXIT:
    caption = "CONTROL-C";
    break;
  case /* STATUS_INTERRUPTED */ 0xC0000515L:
    caption = "INTERRUPTED";
    break;
  case EXCEPTION_ACCESS_VIOLATION:
    caption = "ACCESS VIOLATION";
    break;
  case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    caption = "ARRAY BOUNDS EXCEEDED";
    break;
  case EXCEPTION_DATATYPE_MISALIGNMENT:
    caption = "MISALIGNMENT";
    break;
  case EXCEPTION_STACK_OVERFLOW:
    caption = "STACK OVERFLOW";
    break;
  case EXCEPTION_INVALID_DISPOSITION:
    caption = "INVALID DISPOSITION";
    break;
  case EXCEPTION_ILLEGAL_INSTRUCTION:
    caption = "ILLEGAL INSTRUCTION";
    break;
  case EXCEPTION_NONCONTINUABLE_EXCEPTION:
    caption = "NONCONTINUABLE EXCEPTION";
    break;
  case /* STATUS_STACK_BUFFER_OVERRUN, STATUS_BUFFER_OVERFLOW_PREVENTED */
      0xC0000409L:
    caption = "BUFFER OVERRUN";
    break;
  case /* STATUS_ASSERTION_FAILURE */ 0xC0000420L:
    caption = "ASSERTION FAILURE";
    break;
  case /* STATUS_HEAP_CORRUPTION */ 0xC0000374L:
    caption = "HEAP CORRUPTION";
    break;
  case /* STATUS_CONTROL_STACK_VIOLATION */ 0xC00001B2L:
    caption = "CONTROL STACK VIOLATION";
    break;
  case EXCEPTION_FLT_DIVIDE_BY_ZERO:
    caption = "FLT DIVIDE BY ZERO";
    break;
  default:
    caption = "(unknown)";
    break;
  }
  PVOID CodeAdress = ExInfo->ExceptionRecord->ExceptionAddress;
  fprintf(out, "****************************************************\n");
  fprintf(out, "*** A Program Fault occurred:\n");
  fprintf(out, "*** Error code %08X: %s\n", ExInfo->ExceptionRecord->ExceptionCode, caption);
  fprintf(out, "****************************************************\n");
  fprintf(out, "***   Address: %08zX\n", (intptr_t)CodeAdress);
  fprintf(out, "***     Flags: %08X\n", ExInfo->ExceptionRecord->ExceptionFlags);
  dump_stack(ExInfo->ContextRecord, out);
  return EXCEPTION_EXECUTE_HANDLER;
}
#endif /* _MSC_VER */

static bool execute_thunk(const actor_config *const_config, const mdbx_pid_t pid) {
  actor_config config = *const_config;
  try {
    if (global::singlemode) {
      logging::setup(format("single_%s", testcase2str(config.testcase)));
    } else {
      logging::setup((logging::loglevel)config.params.loglevel,
                     format("child_%u.%u", config.actor_id, config.space_id));
      log_trace(">> wait4barrier");
      osal_wait4barrier();
      log_trace("<< wait4barrier");
    }

    std::unique_ptr<testcase> test(registry::create_actor(config, pid));
    size_t iter = 0;
    do {
      if (iter) {
        prng_salt(iter);
        log_verbose("turn PRNG to %u", config.params.prng_seed);
      }
      iter++;
      if (!test->setup()) {
        log_notice("test setup failed");
        return false;
      }
      if (!test->run()) {
        log_notice("test failed");
        return false;
      }
      if (!test->teardown()) {
        log_notice("test teardown failed");
        return false;
      }

      if (config.params.nrepeat == 1)
        log_verbose("test successfully");
      else {
        if (config.params.nrepeat)
          log_verbose("test successfully (iteration %zi of %zi)", iter, size_t(config.params.nrepeat));
        else
          log_verbose("test successfully (iteration %zi)", iter);
      }

    } while (config.params.nrepeat == 0 || iter < config.params.nrepeat);
    return true;
  } catch (const std::exception &pipets) {
    failure("***** Exception: %s *****", pipets.what());
    return false;
  }
}

bool test_execute(const actor_config &config) {
#ifdef _MSC_VER
  __try {
#endif
    return execute_thunk(&config, osal_getpid());
#ifdef _MSC_VER
  } __except (seh_filter(GetExceptionInformation(), stderr)) {
    fprintf(stderr, "Exception\n");
    return false;
  }
#endif
}

//-----------------------------------------------------------------------------

enum speculum_cursors : int { lowerbound = 0, prev = 1, prev_prev = 2, next = 3, next_next = 4, seek_check = 5 };

bool testcase::is_same(const Item &a, const Item &b) const {
  if (!is_samedata(dataview2iov(a.first), dataview2iov(b.first)))
    return false;
  if ((config.params.table_flags & MDBX_DUPSORT) && !is_samedata(dataview2iov(a.second), dataview2iov(b.second)))
    return false;
  return true;
}

bool testcase::is_same(const testcase::SET::const_iterator &it, const MDBX_val &k, const MDBX_val &v) const {

  return is_samedata(dataview2iov(it->first), k) && is_samedata(dataview2iov(it->second), v);
}

void testcase::verbose(const char *where, const char *stage, const testcase::SET::const_iterator &it) const {
  if (it == speculum.end())
    log_verbose("speculum-%s: %s expect END", where, stage);
  else {
    char dump_key[32], dump_value[32];
    MDBX_val it_key = dataview2iov(it->first);
    MDBX_val it_data = dataview2iov(it->second);
    log_verbose("speculum-%s: %s expect {%s, %s}", where, stage, mdbx_dump_val(&it_key, dump_key, sizeof(dump_key)),
                mdbx_dump_val(&it_data, dump_value, sizeof(dump_value)));
  }
}

void testcase::verbose(const char *where, const char *stage, const MDBX_val &k, const MDBX_val &v, int err) const {
  char dump_key[32], dump_value[32];
  if (err != MDBX_SUCCESS && err != MDBX_RESULT_TRUE)
    log_verbose("speculum-%s: %s cursor {%d, %s}", where, stage, err, mdbx_strerror(err));
  else
    log_verbose("speculum-%s: %s cursor {%s, %s}", where, stage, mdbx_dump_val(&k, dump_key, sizeof(dump_key)),
                mdbx_dump_val(&v, dump_value, sizeof(dump_value)));
}

bool testcase::speculum_check_iterator(const char *where, const char *stage, const testcase::SET::const_iterator &it,
                                       const MDBX_val &k, const MDBX_val &v, MDBX_cursor *cursor) const {
  char dump_key[32], dump_value[32];
  MDBX_val it_key = dataview2iov(it->first);
  MDBX_val it_data = dataview2iov(it->second);
  // log_verbose("speculum-%s: %s expect {%s, %s}", where, stage,
  //             mdbx_dump_val(&it_key, dump_key, sizeof(dump_key)),
  //             mdbx_dump_val(&it_data, dump_value, sizeof(dump_value)));
  if (!is_samedata(it_key, k)) {
    speculum_render(it, cursor);
    return failure("speculum-%s: %s key mismatch %s (must) != %s", where, stage,
                   mdbx_dump_val(&it_key, dump_key, sizeof(dump_key)),
                   mdbx_dump_val(&k, dump_value, sizeof(dump_value)));
  }
  if (!is_samedata(it_data, v)) {
    speculum_render(it, cursor);
    return failure("speculum-%s: %s data mismatch %s (must) != %s", where, stage,
                   mdbx_dump_val(&it_data, dump_key, sizeof(dump_key)),
                   mdbx_dump_val(&v, dump_value, sizeof(dump_value)));
  }
  return true;
}

bool testcase::failure(const char *fmt, ...) const {
  va_list ap;
  va_start(ap, fmt);
  fflush(nullptr);
  logging::output_nocheckloglevel_ap(logging::failure, fmt, ap);
  va_end(ap);
  fflush(nullptr);
  if (txn_guard)
    mdbx_txn_commit(const_cast<testcase *>(this)->txn_guard.release());
  exit(EXIT_FAILURE);
  return false;
}

#if SPECULUM_CURSORS

static void speculum_render_cursor(const MDBX_val &ikey, const MDBX_val &ival, const MDBX_cursor *cursor,
                                   const MDBX_cursor *ref) {
  scoped_cursor_guard guard(mdbx_cursor_create(nullptr));
  if (!guard)
    failure("mdbx_cursor_create()");
  /* работаем с копией курсора, чтобы не влиять на состояние оригинала. */
  int err = mdbx_cursor_copy(cursor, guard.get());
  if (err)
    failure("mdbx_cursor_copy(), err %d", err);

  MDBX_cursor *const clone = guard.get();
  char status[10], *s = status;
  if (cursor == ref) {
    *s++ = '_';
    *s++ = '_';
  }

  if (mdbx_cursor_eof(clone) == MDBX_RESULT_TRUE)
    *s++ = 'e';
  if (mdbx_cursor_on_first(clone) == MDBX_RESULT_TRUE)
    *s++ = 'F';
  if (mdbx_cursor_on_first_dup(clone) == MDBX_RESULT_TRUE)
    *s++ = 'f';
  if (mdbx_cursor_on_last(clone) == MDBX_RESULT_TRUE)
    *s++ = 'L';
  if (mdbx_cursor_on_last_dup(clone) == MDBX_RESULT_TRUE)
    *s++ = 'l';

  MDBX_val ckey, cval;
  if (mdbx_cursor_get(clone, &ckey, &cval, MDBX_GET_CURRENT) != MDBX_SUCCESS)
    *s++ = '!';
  else {
    const int kcmp = mdbx_cmp(mdbx_cursor_txn(clone), mdbx_cursor_dbi(clone), &ikey, &ckey);
    if (kcmp < 0)
      *s++ = '<';
    else if (kcmp > 0)
      *s++ = '>';
    else {
      *s++ = '=';
      const int vcmp = mdbx_dcmp(mdbx_cursor_txn(clone), mdbx_cursor_dbi(clone), &ival, &cval);
      if (vcmp < 0)
        *s++ = '<';
      else if (vcmp > 0)
        *s++ = '>';
      else
        *s++ = '=';
    }
  }

  if (clone == ref) {
    *s++ = '_';
    *s++ = '_';
  }
  *s = '\0';

  printf(" | %-10.10s", status);
}

void testcase::speculum_render(const testcase::SET::const_iterator &it, const MDBX_cursor *ref) const {
  char dump_key[32], dump_value[32];

  auto top = it;
  int offset = 0;
  while (offset > -5 && top != speculum.begin()) {
    --top;
    --offset;
  }
  printf("##  %-20.20s %-20.20s | %-10.10s | %-10.10s | %-10.10s | %-10.10s | "
         "%-10.10s | %-10.10s |\n",
         "k0_1_2_3_4_5_6_7_8_9", "v0_1_2_3_4_5_6_7_8_9", "prev-prev", "prev", "seek", "lowerbound", "next",
         "next-next");
  while (offset < 5 && top != speculum.end()) {
    const MDBX_val ikey = dataview2iov(top->first);
    const MDBX_val idata = dataview2iov(top->second);
    printf("%+d) %20.20s %20.20s", offset, mdbx_dump_val(&ikey, dump_key, sizeof(dump_key)),
           mdbx_dump_val(&idata, dump_value, sizeof(dump_value)));

    speculum_render_cursor(ikey, idata, speculum_cursors[prev_prev].get(), ref);
    speculum_render_cursor(ikey, idata, speculum_cursors[prev].get(), ref);
    speculum_render_cursor(ikey, idata, speculum_cursors[seek_check].get(), ref);
    speculum_render_cursor(ikey, idata, speculum_cursors[lowerbound].get(), ref);
    speculum_render_cursor(ikey, idata, speculum_cursors[next].get(), ref);
    speculum_render_cursor(ikey, idata, speculum_cursors[next_next].get(), ref);

    printf(" %s\n", "|");
    ++top;
    ++offset;
  }
}

bool testcase::speculum_check_cursor(const char *where, const char *stage, const testcase::SET::const_iterator &it,
                                     int cursor_err, const MDBX_val &cursor_key, const MDBX_val &cursor_data,
                                     MDBX_cursor *cursor) const {
  // verbose(where, stage, cursor_key, cursor_data, cursor_err);
  // verbose(where, stage, it);
  if (cursor_err != MDBX_SUCCESS && cursor_err != MDBX_NOTFOUND && cursor_err != MDBX_RESULT_TRUE &&
      cursor_err != MDBX_ENODATA) {
    speculum_render(it, cursor);
    return failure("speculum-%s: %s %s %d %s", where, stage, "cursor-get", cursor_err, mdbx_strerror(cursor_err));
  }

  char dump_key[32], dump_value[32];
  if (it == speculum.end() && cursor_err != MDBX_NOTFOUND && cursor_err != MDBX_ENODATA) {
    speculum_render(it, cursor);
    return failure("speculum-%s: %s extra pair {%s, %s}", where, stage,
                   mdbx_dump_val(&cursor_key, dump_key, sizeof(dump_key)),
                   mdbx_dump_val(&cursor_data, dump_value, sizeof(dump_value)));
  } else if (it != speculum.end() && (cursor_err == MDBX_NOTFOUND || cursor_err == MDBX_ENODATA)) {
    speculum_render(it, cursor);
    MDBX_val it_key = dataview2iov(it->first);
    MDBX_val it_data = dataview2iov(it->second);
    return failure("speculum-%s: %s lack pair {%s, %s}", where, stage,
                   mdbx_dump_val(&it_key, dump_key, sizeof(dump_key)),
                   mdbx_dump_val(&it_data, dump_value, sizeof(dump_value)));
  } else if (cursor_err == MDBX_SUCCESS || cursor_err == MDBX_RESULT_TRUE)
    return speculum_check_iterator(where, stage, it, cursor_key, cursor_data, cursor);
  else {
    assert(it == speculum.end() && (cursor_err == MDBX_NOTFOUND || cursor_err == MDBX_ENODATA));
    return true;
  }
}

bool testcase::speculum_check_cursor(const char *where, const char *stage, const testcase::SET::const_iterator &it,
                                     MDBX_cursor *cursor, const MDBX_cursor_op op) const {
  MDBX_val cursor_key = {0, 0};
  MDBX_val cursor_data = {0, 0};
  int err = mdbx_cursor_get(cursor, &cursor_key, &cursor_data, op);
  return speculum_check_cursor(where, stage, it, err, cursor_key, cursor_data, cursor);
}

void testcase::speculum_prepare_cursors(const Item &item) {
  int err;
  assert(config.params.speculum);
  if (speculum_cursors[lowerbound])
    for (auto &guard : speculum_cursors) {
      if (txn_guard.get() != mdbx_cursor_txn(guard.get()) || dbi != mdbx_cursor_dbi(guard.get())) {
        err = mdbx_cursor_bind(txn_guard.get(), guard.get(), dbi);
        if (unlikely(err != MDBX_SUCCESS))
          failure_perror("mdbx_cursor_bind()", err);
      }
    }
  else
    for (auto &guard : speculum_cursors) {
      MDBX_cursor *cursor = nullptr;
      err = mdbx_cursor_open(txn_guard.get(), dbi, &cursor);
      if (unlikely(err != MDBX_SUCCESS))
        failure_perror("mdbx_cursor_open()", err);
      guard.reset(cursor);
    }

  // mdbx_cursor_reset(speculum_cursors[seek_check].get());
  const auto cursor_lowerbound = speculum_cursors[lowerbound].get();
  const MDBX_val item_key = dataview2iov(item.first), item_data = dataview2iov(item.second);
  MDBX_val lowerbound_key = item_key;
  MDBX_val lowerbound_data = item_data;
  // verbose("prepare-cursors", "item", item_key, item_data);
  err = mdbx_cursor_get(cursor_lowerbound, &lowerbound_key, &lowerbound_data, MDBX_SET_LOWERBOUND);
  // verbose("prepare-cursors", "lowerbound", lowerbound_key, lowerbound_data,
  //         err);
  if (unlikely(err != MDBX_SUCCESS && err != MDBX_RESULT_TRUE && err != MDBX_NOTFOUND))
    failure("speculum-%s: %s %s %d %s", "prepare-cursors", "lowerbound", "cursor-get", err, mdbx_strerror(err));

  auto it_lowerbound = speculum.lower_bound(item);
  // verbose("prepare-cursors", "lowerbound", it_lowerbound);
  speculum_check_cursor("prepare-cursors", "lowerbound", it_lowerbound, err, lowerbound_key, lowerbound_data,
                        cursor_lowerbound);

  const auto cursor_prev = speculum_cursors[prev].get();
  err = mdbx_cursor_copy(cursor_lowerbound, cursor_prev);
  if (unlikely(err != MDBX_SUCCESS))
    failure("speculum-%s: %s %s %d %s", "prepare-cursors", "prev", "cursor-copy", err, mdbx_strerror(err));
  auto it_prev = it_lowerbound;
  if (it_prev != speculum.begin()) {
    speculum_check_cursor("prepare-cursors", "prev", --it_prev, cursor_prev, MDBX_PREV);
  } else if ((err = mdbx_cursor_on_first(cursor_prev)) != MDBX_RESULT_TRUE)
    failure("speculum-%s: %s on-first %d %s", "prepare-cursors", "prev", err, mdbx_strerror(err));

  const auto cursor_prev_prev = speculum_cursors[prev_prev].get();
  err = mdbx_cursor_copy(cursor_prev, cursor_prev_prev);
  if (unlikely(err != MDBX_SUCCESS))
    failure("speculum-%s: %s %s %d %s", "prepare-cursors", "prev-prev", "cursor-copy", err, mdbx_strerror(err));
  auto it_prev_prev = it_prev;
  if (it_prev_prev != speculum.begin()) {
    speculum_check_cursor("prepare-cursors", "prev-prev", --it_prev_prev, cursor_prev_prev, MDBX_PREV);
  } else if ((err = mdbx_cursor_on_first(cursor_prev_prev)) != MDBX_RESULT_TRUE)
    failure("speculum-%s: %s on-first %d %s", "prepare-cursors", "prev-prev", err, mdbx_strerror(err));

  const auto cursor_next = speculum_cursors[next].get();
  err = mdbx_cursor_copy(cursor_lowerbound, cursor_next);
  if (unlikely(err != MDBX_SUCCESS))
    failure("speculum-%s: %s %s %d %s", "prepare-cursors", "next", "cursor-copy", err, mdbx_strerror(err));
  auto it_next = it_lowerbound;
  if (it_next != speculum.end()) {
    speculum_check_cursor("prepare-cursors", "next", ++it_next, cursor_next, MDBX_NEXT);
  } else if ((err = mdbx_cursor_on_last(cursor_next)) != MDBX_RESULT_TRUE)
    failure("speculum-%s: %s on-last %d %s", "prepare-cursors", "next", err, mdbx_strerror(err));

  const auto cursor_next_next = speculum_cursors[next_next].get();
  err = mdbx_cursor_copy(cursor_next, cursor_next_next);
  if (unlikely(err != MDBX_SUCCESS))
    failure("speculum-%s: %s %s %d %s", "prepare-cursors", "next-next", "cursor-copy", err, mdbx_strerror(err));
  auto it_next_next = it_next;
  if (it_next_next != speculum.end()) {
    speculum_check_cursor("prepare-cursors", "next-next", ++it_next_next, cursor_next_next, MDBX_NEXT);
  } else if ((err = mdbx_cursor_on_last(cursor_next_next)) != MDBX_RESULT_TRUE)
    failure("speculum-%s: %s on-last %d %s", "prepare-cursors", "next-next", err, mdbx_strerror(err));
}
#endif /* SPECULUM_CURSORS */

int testcase::insert(const keygen::buffer &akey, const keygen::buffer &adata, MDBX_put_flags_t flags) {
  int err;
  bool rc = true;
  Item item;
#if SPECULUM_CURSORS
  MDBX_cursor *check_seek_cursor = nullptr;
  MDBX_val seek_check_key, seek_check_data;
  int seek_check_err = 42;
#endif /* SPECULUM_CURSORS */
  if (config.params.speculum) {
    item.first = iov2dataview(akey);
    item.second = iov2dataview(adata);
#if SPECULUM_CURSORS
    speculum_prepare_cursors(item);
    check_seek_cursor = speculum_cursors[seek_check].get();
    seek_check_key = akey->value;
    seek_check_data = adata->value;
    seek_check_err = mdbx_cursor_get(check_seek_cursor, &seek_check_key, &seek_check_data, MDBX_SET_LOWERBOUND);
    // speculum_render(speculum.find(item), check_seek_cursor);
    if (seek_check_err != MDBX_SUCCESS && seek_check_err != MDBX_NOTFOUND && seek_check_err != MDBX_RESULT_TRUE)
      failure("speculum-%s: %s pre-insert %d %s", "insert", "seek", seek_check_err, mdbx_strerror(seek_check_err));
#endif /* SPECULUM_CURSORS */
  }

  err = mdbx_put(txn_guard.get(), dbi, &akey->value, &adata->value, flags);
  if (err != MDBX_SUCCESS && err != MDBX_KEYEXIST)
    return err;

  if (config.params.speculum) {
    char dump_key[32], dump_value[32];
    const auto insertion_result = speculum.insert(item);
    if (err == MDBX_KEYEXIST && insertion_result.second) {
      log_error("speculum.insert: unexpected %s {%s, %s}", "MDBX_KEYEXIST",
                mdbx_dump_val(&akey->value, dump_key, sizeof(dump_key)),
                mdbx_dump_val(&adata->value, dump_value, sizeof(dump_value)));
      rc = false;
    }
    if (err == MDBX_SUCCESS && !insertion_result.second) {
      log_error("speculum.insert: unexpected %s {%s, %s}", "MDBX_SUCCESS",
                mdbx_dump_val(&akey->value, dump_key, sizeof(dump_key)),
                mdbx_dump_val(&adata->value, dump_value, sizeof(dump_value)));
      rc = false;
    }

#if SPECULUM_CURSORS
    if (insertion_result.second) {
      if (seek_check_err == MDBX_SUCCESS) {
        log_error("speculum.pre-insert-seek: unexpected %d {%s, %s}", seek_check_err,
                  mdbx_dump_val(&seek_check_key, dump_key, sizeof(dump_key)),
                  mdbx_dump_val(&seek_check_data, dump_value, sizeof(dump_value)));
        rc = false;
      }
    } else {
      if (seek_check_err != MDBX_SUCCESS) {
        log_error("speculum.pre-insert-seek: unexpected %d {%s, %s}", seek_check_err,
                  mdbx_dump_val(&seek_check_key, dump_key, sizeof(dump_key)),
                  mdbx_dump_val(&seek_check_data, dump_value, sizeof(dump_value)));
        speculum_check_iterator("insert", "pre-seek", insertion_result.first, seek_check_key, seek_check_data,
                                check_seek_cursor);
        rc = false;
      }
    }

    if (insertion_result.first != speculum.begin()) {
      const auto cursor_prev = speculum_cursors[prev].get();
      auto it_prev = insertion_result.first;
      speculum_check_cursor("after-insert", "prev", --it_prev, cursor_prev, MDBX_GET_CURRENT);
      if (it_prev != speculum.begin()) {
        const auto cursor_prev_prev = speculum_cursors[prev_prev].get();
        auto it_prev_prev = it_prev;
        speculum_check_cursor("after-insert", "prev-prev", --it_prev_prev, cursor_prev_prev, MDBX_GET_CURRENT);
      }
    }

    auto it_lowerbound = insertion_result.first;
    if (insertion_result.second)
      ++it_lowerbound;
    if (it_lowerbound != speculum.end()) {
      const auto cursor_lowerbound = speculum_cursors[lowerbound].get();
      speculum_check_cursor("after-insert", "lowerbound", it_lowerbound, cursor_lowerbound, MDBX_GET_CURRENT);

      auto it_next = it_lowerbound;
      if (++it_next != speculum.end()) {
        const auto cursor_next = speculum_cursors[next].get();
        speculum_check_cursor("after-insert", "next", it_next, cursor_next, MDBX_GET_CURRENT);

        auto it_next_next = it_next;
        if (++it_next_next != speculum.end()) {
          const auto cursor_next_next = speculum_cursors[next_next].get();
          speculum_check_cursor("after-insert", "next-next", it_next_next, cursor_next_next, MDBX_GET_CURRENT);
        }
      }
    }
    // speculum_render(insertion_result.first,
    //                 speculum_cursors[seek_check].get());
#endif /* SPECULUM_CURSORS */
  }

  return rc ? MDBX_SUCCESS : MDBX_RESULT_TRUE;
}

int testcase::replace(const keygen::buffer &akey, const keygen::buffer &new_data, const keygen::buffer &old_data,
                      MDBX_put_flags_t flags, bool hush_keygen_mistakes) {
  int expected_err = MDBX_SUCCESS;
  if (config.params.speculum) {
    const auto S_key = iov2dataview(akey);
    const auto S_old = iov2dataview(old_data);
    const auto S_new = iov2dataview(new_data);
    const auto removed = speculum.erase(SET::key_type(S_key, S_old));
    if (unlikely(!removed)) {
      char dump_key[128], dump_value[128];
      log_error("speculum-%s: no old pair {%s, %s} (keygen mistake)", "replace",
                mdbx_dump_val(&akey->value, dump_key, sizeof(dump_key)),
                mdbx_dump_val(&old_data->value, dump_value, sizeof(dump_value)));
      expected_err = MDBX_NOTFOUND;
    } else if (unlikely(!speculum.emplace(S_key, S_new).second)) {
      char dump_key[128], dump_value[128];
      log_error("speculum-%s: %s {%s, %s}", "replace", "new pair not inserted",
                mdbx_dump_val(&akey->value, dump_key, sizeof(dump_key)),
                mdbx_dump_val(&new_data->value, dump_value, sizeof(dump_value)));
      expected_err = MDBX_KEYEXIST;
    }
  }
  int err = mdbx_replace(txn_guard.get(), dbi, &akey->value, &new_data->value, &old_data->value, flags);
  if (err && err == expected_err && hush_keygen_mistakes) {
    log_notice("speculum-%s: %s %d", "replace", "hust keygen mistake", err);
    err = MDBX_SUCCESS;
  }
  return err;
}

int testcase::remove(const keygen::buffer &akey, const keygen::buffer &adata) {
  int err;
  bool rc = true;
  Item item;
  if (config.params.speculum) {
    item.first = iov2dataview(akey);
    item.second = iov2dataview(adata);
#if SPECULUM_CURSORS
    speculum_prepare_cursors(item);
    // MDBX_cursor *check_seek_cursor = speculum_cursors[seek_check].get();
    // MDBX_val seek_check_key = akey->value;
    // MDBX_val seek_check_data = adata->value;
    // mdbx_cursor_get(check_seek_cursor, &seek_check_key, &seek_check_data,
    //                 MDBX_SET_LOWERBOUND);
    // speculum_render(speculum.find(item), check_seek_cursor);
#endif /* SPECULUM_CURSORS */
  }

  err = mdbx_del(txn_guard.get(), dbi, &akey->value, &adata->value);
  if (err != MDBX_NOTFOUND && err != MDBX_SUCCESS)
    return err;

  if (config.params.speculum) {
    char dump_key[32], dump_value[32];
    const auto it_found = speculum.find(item);
    if (it_found == speculum.end()) {
      if (err != MDBX_NOTFOUND) {
        log_error("speculum.remove: unexpected %s {%s, %s}", "MDBX_SUCCESS",
                  mdbx_dump_val(&akey->value, dump_key, sizeof(dump_key)),
                  mdbx_dump_val(&adata->value, dump_value, sizeof(dump_value)));
        rc = false;
      }
    } else {
      if (err != MDBX_SUCCESS) {
        log_error("speculum.remove: unexpected %s {%s, %s}", "MDBX_NOTFOUND",
                  mdbx_dump_val(&akey->value, dump_key, sizeof(dump_key)),
                  mdbx_dump_val(&adata->value, dump_value, sizeof(dump_value)));
        rc = false;
      }

#if SPECULUM_CURSORS
      // speculum_render(it_found, speculum_cursors[seek_check].get());
      if (it_found != speculum.begin()) {
        const auto cursor_prev = speculum_cursors[prev].get();
        auto it_prev = it_found;
        speculum_check_cursor("after-remove", "prev", --it_prev, cursor_prev, MDBX_GET_CURRENT);
        if (it_prev != speculum.begin()) {
          const auto cursor_prev_prev = speculum_cursors[prev_prev].get();
          auto it_prev_prev = it_prev;
          speculum_check_cursor("after-remove", "prev-prev", --it_prev_prev, cursor_prev_prev, MDBX_GET_CURRENT);
        }
      }

      auto it_next = it_found;
      const auto cursor_next = speculum_cursors[next].get();
      const auto cursor_lowerbound = speculum_cursors[lowerbound].get();
      if (++it_next != speculum.end()) {
        speculum_check_cursor("after-remove", "next", it_next, cursor_next, MDBX_GET_CURRENT);
        speculum_check_cursor("after-remove", "lowerbound", it_next, cursor_lowerbound, MDBX_NEXT);

        auto it_next_next = it_next;
        const auto cursor_next_next = speculum_cursors[next_next].get();
        if (++it_next_next != speculum.end()) {
          speculum_check_cursor("after-remove", "next-next", it_next_next, cursor_next_next, MDBX_GET_CURRENT);
        } else if ((err = mdbx_cursor_on_last(cursor_next_next)) != MDBX_RESULT_TRUE)
          failure("speculum-%s: %s on-last %d %s", "after-remove", "next-next", err, mdbx_strerror(err));
      } else {
        if ((err = mdbx_cursor_on_last(cursor_next)) != MDBX_RESULT_TRUE)
          failure("speculum-%s: %s on-last %d %s", "after-remove", "next", err, mdbx_strerror(err));
        if ((err = mdbx_cursor_on_last(cursor_lowerbound)) != MDBX_RESULT_TRUE)
          failure("speculum-%s: %s on-last %d %s", "after-remove", "lowerbound", err, mdbx_strerror(err));
      }
#endif /* SPECULUM_CURSORS */

      speculum.erase(it_found);
    }
  }

  return rc ? MDBX_SUCCESS : MDBX_RESULT_TRUE;
}

bool testcase::speculum_verify() {
  if (!config.params.speculum)
    return true;

  if (!txn_guard)
    txn_begin(true);

  char dump_key[128], dump_value[128];
  char dump_mkey[128], dump_mvalue[128];

  MDBX_cursor *cursor;
  int eof, err = mdbx_cursor_open(txn_guard.get(), dbi, &cursor);
  if (err != MDBX_SUCCESS)
    failure_perror("mdbx_cursor_open()", err);

  bool rc = true;
  MDBX_val akey, avalue;
  MDBX_val mkey, mvalue;
  err = mdbx_cursor_get(cursor, &akey, &avalue, MDBX_FIRST);
  if (err == MDBX_NOTFOUND) {
    err = mdbx_cursor_get(cursor, &akey, &avalue, MDBX_GET_CURRENT);
    if (err == MDBX_ENODATA)
      err = MDBX_NOTFOUND;
    else {
      log_error("unexpected %d for MDBX_GET_CURRENT on empty DB", err);
      rc = false;
    }
  }

  unsigned extra = 0, lost = 0, n = 0;
  assert(std::is_sorted(speculum.cbegin(), speculum.cend(), ItemCompare(this)));
  auto it = speculum.cbegin();
  while (true) {
    if (err != MDBX_SUCCESS) {
      akey.iov_len = avalue.iov_len = 0;
      akey.iov_base = avalue.iov_base = nullptr;
    } else {
      eof = mdbx_cursor_eof(cursor);
      if (eof != MDBX_RESULT_FALSE) {
        log_error("false-positive cursor-eof %u/%u: db{%s, %s}, rc %i", n, extra,
                  mdbx_dump_val(&akey, dump_key, sizeof(dump_key)),
                  mdbx_dump_val(&avalue, dump_value, sizeof(dump_value)), eof);
        rc = false;
      }
    }
    const auto S_key = iov2dataview(akey);
    const auto S_data = iov2dataview(avalue);
    if (it != speculum.cend()) {
      mkey = it->first;
      mvalue = it->second;
    }
    if (err == MDBX_SUCCESS && it != speculum.cend() && S_key == it->first && S_data == it->second) {
      ++it;
      err = mdbx_cursor_get(cursor, &akey, &avalue, MDBX_NEXT);
    } else if (err == MDBX_SUCCESS &&
               (it == speculum.cend() || S_key < it->first || (S_key == it->first && S_data < it->second))) {
      extra += 1;
      if (it != speculum.cend()) {
        log_error(
            "extra pair %u/%u: db{%s, %s} < mi{%s, %s}", n, extra, mdbx_dump_val(&akey, dump_key, sizeof(dump_key)),
            mdbx_dump_val(&avalue, dump_value, sizeof(dump_value)), mdbx_dump_val(&mkey, dump_mkey, sizeof(dump_mkey)),
            mdbx_dump_val(&mvalue, dump_mvalue, sizeof(dump_mvalue)));
      } else {
        log_error("extra pair %u/%u: db{%s, %s} < mi.END", n, extra, mdbx_dump_val(&akey, dump_key, sizeof(dump_key)),
                  mdbx_dump_val(&avalue, dump_value, sizeof(dump_value)));
      }
      err = mdbx_cursor_get(cursor, &akey, &avalue, MDBX_NEXT);
      rc = false;
    } else if (it != speculum.cend() &&
               (err == MDBX_NOTFOUND || S_key > it->first || (S_key == it->first && S_data > it->second))) {
      lost += 1;
      if (err == MDBX_NOTFOUND) {
        log_error("lost pair %u/%u: db.END > mi{%s, %s}", n, lost, mdbx_dump_val(&mkey, dump_mkey, sizeof(dump_mkey)),
                  mdbx_dump_val(&mvalue, dump_mvalue, sizeof(dump_mvalue)));
      } else {
        log_error("lost pair %u/%u: db{%s, %s} > mi{%s, %s}", n, lost, mdbx_dump_val(&akey, dump_key, sizeof(dump_key)),
                  mdbx_dump_val(&avalue, dump_value, sizeof(dump_value)),
                  mdbx_dump_val(&mkey, dump_mkey, sizeof(dump_mkey)),
                  mdbx_dump_val(&mvalue, dump_mvalue, sizeof(dump_mvalue)));
      }
      ++it;
      rc = false;
    } else if (err == MDBX_NOTFOUND && it == speculum.cend()) {
      break;
    } else if (err != MDBX_SUCCESS) {
      failure_perror("mdbx_cursor_get()", err);
    } else {
      assert(!"WTF?");
    }
    n += 1;
  }

  if (err == MDBX_NOTFOUND) {
    eof = mdbx_cursor_eof(cursor);
    if (eof != MDBX_RESULT_TRUE) {
      eof = mdbx_cursor_eof(cursor);
      log_error("false-negative cursor-eof: %u, rc %i", n, eof);
      rc = false;
    }
  }
  mdbx_cursor_close(cursor);
  return rc;
}

bool testcase::check_batch_get() {
  char dump_key[128], dump_value[128];
  char dump_key_batch[128], dump_value_batch[128];

  MDBX_cursor *check_cursor;
  int check_err = mdbx_cursor_open(txn_guard.get(), dbi, &check_cursor);
  if (check_err != MDBX_SUCCESS)
    failure_perror("mdbx_cursor_open()", check_err);

  MDBX_cursor *batch_cursor;
  int batch_err = mdbx_cursor_open(txn_guard.get(), dbi, &batch_cursor);
  if (batch_err != MDBX_SUCCESS)
    failure_perror("mdbx_cursor_open()", batch_err);

  bool rc = true;
  MDBX_val pairs[42];
  size_t count = 0xDeadBeef;
  batch_err = mdbx_cursor_get_batch(batch_cursor, &count, pairs, ARRAY_LENGTH(pairs), MDBX_FIRST);
  size_t i, n = 0;
  while (batch_err == MDBX_SUCCESS || batch_err == MDBX_RESULT_TRUE) {
    for (i = 0; i < count; i += 2) {
      mdbx::slice k, v;
      check_err = mdbx_cursor_get(check_cursor, &k, &v, n ? MDBX_NEXT : MDBX_FIRST);
      if (check_err != MDBX_SUCCESS)
        failure_perror("batch-verify: mdbx_cursor_get(MDBX_NEXT)", check_err);
      if (k != pairs[i] || v != pairs[i + 1]) {
        log_error("batch-get pair mismatch %zu/%zu: sequential{%s, %s} != "
                  "batch{%s, %s}",
                  n + i / 2, i, mdbx_dump_val(&k, dump_key, sizeof(dump_key)),
                  mdbx_dump_val(&v, dump_value, sizeof(dump_value)),
                  mdbx_dump_val(&pairs[i], dump_key_batch, sizeof(dump_key_batch)),
                  mdbx_dump_val(&pairs[i + 1], dump_value_batch, sizeof(dump_value_batch)));
        rc = false;
      }
      ++n;
    }
    batch_err = mdbx_cursor_get_batch(batch_cursor, &count, pairs, ARRAY_LENGTH(pairs), MDBX_NEXT);
  }
  if (batch_err != MDBX_NOTFOUND) {
    log_error("mdbx_cursor_get_batch(), err %d", batch_err);
    rc = false;
  }

  batch_err = mdbx_cursor_eof(batch_cursor);
  if (batch_err != MDBX_RESULT_TRUE) {
    log_error("batch-get %s-cursor not-eof %d", "batch", batch_err);
    rc = false;
  }
  batch_err = mdbx_cursor_on_last(batch_cursor);
  if (batch_err != MDBX_RESULT_TRUE) {
    log_error("batch-get %s-cursor not-on-last %d", "batch", batch_err);
    rc = false;
  }

  check_err = mdbx_cursor_on_last(check_cursor);
  if (check_err != MDBX_RESULT_TRUE) {
    log_error("batch-get %s-cursor not-on-last %d", "checked", check_err);
    rc = false;
  }
  mdbx_cursor_close(check_cursor);
  mdbx_cursor_close(batch_cursor);
  return rc;
}

bool testcase::txn_probe_parking() {
  MDBX_txn_flags_t state = mdbx_txn_flags(txn_guard.get()) & (MDBX_TXN_RDONLY | MDBX_TXN_PARKED | MDBX_TXN_AUTOUNPARK |
                                                              MDBX_TXN_OUSTED | MDBX_TXN_BLOCKED);
  if (state != MDBX_TXN_RDONLY)
    return true;

  const bool autounpark = flipcoin();
  int err = mdbx_txn_park(txn_guard.get(), autounpark);
  if (err != MDBX_SUCCESS)
    failure("mdbx_txn_park(), err %d", err);

  MDBX_txn_info txn_info;
  if (flipcoin()) {
    err = mdbx_txn_info(txn_guard.get(), &txn_info, flipcoin());
    if (err != MDBX_SUCCESS)
      failure("mdbx_txn_info(1), state 0x%x, err %d", state = mdbx_txn_flags(txn_guard.get()), err);
  }

  if (osal_multiactor_mode() && !mode_readonly()) {
    while (flipcoin() && ((state = mdbx_txn_flags(txn_guard.get())) & MDBX_TXN_OUSTED) == 0)
      osal_udelay(4242);
  }

  if (flipcoin()) {
    err = mdbx_txn_info(txn_guard.get(), &txn_info, flipcoin());
    if (err != MDBX_SUCCESS)
      failure("mdbx_txn_info(2), state 0x%x, err %d", state = mdbx_txn_flags(txn_guard.get()), err);
  }

  if (flipcoin()) {
    MDBX_envinfo env_info;
    err = mdbx_env_info_ex(db_guard.get(), txn_guard.get(), &env_info, sizeof(env_info));
    if (!autounpark) {
      if (err != MDBX_BAD_TXN)
        failure("mdbx_env_info_ex(autounpark=%s), flags 0x%x, unexpected err "
                "%d, must %d",
                autounpark ? "true" : "false", state, err, MDBX_BAD_TXN);
    } else if (err != MDBX_SUCCESS) {
      if (err != MDBX_OUSTED || ((state = mdbx_txn_flags(txn_guard.get())) & MDBX_TXN_OUSTED) == 0)
        failure("mdbx_env_info_ex(autounpark=%s), flags 0x%x, err %d", autounpark ? "true" : "false", state, err);
      else {
        err = mdbx_txn_renew(txn_guard.get());
        if (err != MDBX_SUCCESS)
          failure("mdbx_txn_renew(), state 0x%x, err %d", state = mdbx_txn_flags(txn_guard.get()), err);
      }
    }
  }

  const bool autorestart = flipcoin();
  err = mdbx_txn_unpark(txn_guard.get(), autorestart);
  if (MDBX_IS_ERROR(err)) {
    if (err != MDBX_OUSTED || autorestart)
      failure("mdbx_txn_unpark(autounpark=%s, autorestart=%s), err %d", autounpark ? "true" : "false",
              autorestart ? "true" : "false", err);
    else {
      err = mdbx_txn_renew(txn_guard.get());
      if (err != MDBX_SUCCESS)
        failure("mdbx_txn_renew(), state 0x%x, err %d", state = mdbx_txn_flags(txn_guard.get()), err);
    }
  }

  state = mdbx_txn_flags(txn_guard.get()) &
          (MDBX_TXN_RDONLY | MDBX_TXN_PARKED | MDBX_TXN_AUTOUNPARK | MDBX_TXN_OUSTED | MDBX_TXN_BLOCKED);
  if (state != MDBX_TXN_RDONLY)
    failure("unexpected txn-state 0x%x", state);
  return state == MDBX_TXN_RDONLY;
}
