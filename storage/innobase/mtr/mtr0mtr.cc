/*****************************************************************************

Copyright (c) 1995, 2021, Oracle and/or its affiliates.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License, version 2.0, as published by the
Free Software Foundation.

This program is also distributed with certain software (including but not
limited to OpenSSL) that is licensed under separate terms, as designated in a
particular file or component or in included license documentation. The authors
of MySQL hereby grant you an additional permission to link the program and
your derivative works with the separately licensed software that they have
included with MySQL.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License, version 2.0,
for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

*****************************************************************************/

/** @file mtr/mtr0mtr.cc
 Mini-transaction buffer

 Created 11/26/1995 Heikki Tuuri
 *******************************************************/

#include "mtr0mtr.h"

#include "buf0buf.h"
#include "buf0flu.h"
#include "clone0api.h"
#include "fsp0sysspace.h"
#include "log0meb.h"
#ifndef UNIV_HOTBACKUP
#include "clone0clone.h"
#include "log0log.h"
#include "log0recv.h"
#include "mtr0log.h"
#endif /* !UNIV_HOTBACKUP */
#include "my_dbug.h"
#ifndef UNIV_HOTBACKUP
#include "page0types.h"
#include "trx0purge.h"
#endif /* !UNIV_HOTBACKUP */

static_assert(static_cast<int>(MTR_MEMO_PAGE_S_FIX) == static_cast<int>(RW_S_LATCH), "");

static_assert(static_cast<int>(MTR_MEMO_PAGE_X_FIX) == static_cast<int>(RW_X_LATCH), "");

static_assert(static_cast<int>(MTR_MEMO_PAGE_SX_FIX) == static_cast<int>(RW_SX_LATCH), "");

/** Iterate over a memo block in reverse. */
template <typename Functor>
struct Iterate {

  /** Release specific object */
  explicit Iterate(Functor &functor) : m_functor(functor) {
    /* Do nothing */
  }

  /** @return false if the functor returns false. */
  bool operator()(mtr_buf_t::block_t *block) {
    const mtr_memo_slot_t *start = reinterpret_cast<const mtr_memo_slot_t *>(block->begin());
    mtr_memo_slot_t *slot = reinterpret_cast<mtr_memo_slot_t *>(block->end());
    ut_ad(!(block->used() % sizeof(*slot)));

    while (slot-- != start) {
      if (!m_functor(slot)) {
        return false;
      }
    }

    return true;
  }

  Functor &m_functor;
};

/** Find specific object */
struct Find {
  /** Constructor */
  Find(const void *object, ulint type): m_slot(), m_type(type), m_object(object) {
    ut_a(object != nullptr);
  }

  /** @return false if the object was found. */
  bool operator()(mtr_memo_slot_t *slot) {
    if (m_object == slot->object && m_type == slot->type) {
      m_slot = slot;
      return false;
    }

    return true;
  }

  /** Slot if found */
  mtr_memo_slot_t *m_slot;

  /** Type of the object to look for */
  ulint m_type;

  /** The object instance to look for */
  const void *m_object;
};

/** Find a page frame */
struct Find_page {
  /** Constructor
  @param[in]	ptr	pointer to within a page frame
  @param[in]	flags	MTR_MEMO flags to look for */
  Find_page(const void *ptr, ulint flags)
      : m_ptr(ptr), m_flags(flags), m_slot(nullptr) {
    /* We can only look for page-related flags. */
    ut_ad(!(flags &
            ~(MTR_MEMO_PAGE_S_FIX | MTR_MEMO_PAGE_X_FIX | MTR_MEMO_PAGE_SX_FIX |
              MTR_MEMO_BUF_FIX | MTR_MEMO_MODIFY)));
  }

  /** Visit a memo entry.
  @param[in]	slot	memo entry to visit
  @retval	false	if a page was found
  @retval	true	if the iteration should continue */
  bool operator()(mtr_memo_slot_t *slot) {
    ut_ad(m_slot == nullptr);

    if (!(m_flags & slot->type) || slot->object == nullptr) {
      return true;
    }

    buf_block_t *block = reinterpret_cast<buf_block_t *>(slot->object);

    if (m_ptr < block->frame ||
        m_ptr >= block->frame + block->page.size.logical()) {
      return true;
    }

    m_slot = slot;
    return false;
  }

  /** @return the slot that was found */
  mtr_memo_slot_t *get_slot() const {
    ut_ad(m_slot != nullptr);
    return m_slot;
  }
  /** @return the block that was found */
  buf_block_t *get_block() const {
    return reinterpret_cast<buf_block_t *>(get_slot()->object);
  }

 private:
  /** Pointer inside a page frame to look for */
  const void *const m_ptr;
  /** MTR_MEMO flags to look for */
  const ulint m_flags;
  /** The slot corresponding to m_ptr */
  mtr_memo_slot_t *m_slot;
};

#ifndef UNIV_HOTBACKUP
#ifdef UNIV_DEBUG
struct Mtr_memo_contains {
  Mtr_memo_contains(const mtr_t *mtr, mtr_memo_type_t type)
      : m_mtr(mtr), m_type(type) {}

  /** Check if the object in the given slot is of the correct type
  and then check if it is contained in the mtr.
  @retval true if the object in the slot is not of required type.
  os is of the required type, but is not contained in the mtr.
  @retval false if the object in the slot is of the required type
                and it is contained in the mtr. */
  bool operator()(mtr_memo_slot_t *slot) {
    if (slot->type != m_type) {
      return true;
    }
    return !mtr_memo_contains(m_mtr, slot->object, m_type);
  }

 private:
  const mtr_t *m_mtr;
  mtr_memo_type_t m_type;
};

bool mtr_t::conflicts_with(const mtr_t *mtr2) const {
  Mtr_memo_contains check(mtr2, MTR_MEMO_MODIFY);
  Iterate<Mtr_memo_contains> iterator(check);

  return !m_impl.m_memo.for_each_block_in_reverse(iterator);
}
#endif /* UNIV_DEBUG */
#endif /* !UNIV_HOTBACKUP */

/** Release latches and decrement the buffer fix count.
@param[in]	slot	memo slot */
static void memo_slot_release(mtr_memo_slot_t *slot) {
  switch (slot->type) {
#ifndef UNIV_HOTBACKUP
    buf_block_t *block;
#endif /* !UNIV_HOTBACKUP */

    case MTR_MEMO_BUF_FIX:
    case MTR_MEMO_PAGE_S_FIX:
    case MTR_MEMO_PAGE_SX_FIX:
    case MTR_MEMO_PAGE_X_FIX:
#ifndef UNIV_HOTBACKUP
      block = reinterpret_cast<buf_block_t *>(slot->object);

      buf_page_release_latch(block, slot->type);
      /* The buf_page_release_latch(block,..) call was last action dereferencing
      the `block`, so we can unfix the `block` now, but not sooner.*/
      buf_block_unfix(block);
#endif /* !UNIV_HOTBACKUP */
      break;

    case MTR_MEMO_S_LOCK:
      rw_lock_s_unlock(reinterpret_cast<rw_lock_t *>(slot->object));
      break;

    case MTR_MEMO_SX_LOCK:
      rw_lock_sx_unlock(reinterpret_cast<rw_lock_t *>(slot->object));
      break;

    case MTR_MEMO_X_LOCK:
      rw_lock_x_unlock(reinterpret_cast<rw_lock_t *>(slot->object));
      break;

#ifdef UNIV_DEBUG
    default:
      ut_ad(slot->type == MTR_MEMO_MODIFY);
#endif /* UNIV_DEBUG */
  }

  slot->object = nullptr;
}

/** Release the latches and blocks acquired by the mini-transaction. */
struct Release_all {
  /** @return true always. */
  bool operator()(mtr_memo_slot_t *slot) const {
    if (slot->object != nullptr) {
      memo_slot_release(slot);
    }

    return true;
  }
};

/** Check that all slots have been handled. */
struct Debug_check {
  /** @return true always. */
  bool operator()(const mtr_memo_slot_t *slot) const {
    ut_a(slot->object == nullptr);
    return true;
  }
};

#ifdef UNIV_DEBUG
/** Assure that there are no slots that are latching any resources. Only buffer
 * fixing a page is allowed. */
struct Debug_check_no_latching {
  /** @return true always. */
  bool operator()(const mtr_memo_slot_t *slot) const {
    switch (slot->type) {
      case MTR_MEMO_BUF_FIX:
        break;
      default:
        ib::fatal() << "Debug_check_no_latching failed, slot->type="
                    << slot->type;
    }
    return true;
  }
};
#endif

/** Add blocks modified by the mini-transaction to the flush list. */
/** 将被 mtr 修改的 block 添加到刷新列表 flush list 中 */
struct Add_dirty_blocks_to_flush_list {

  /** Constructor.
  @param[in]	    start_lsn	LSN of the first entry that was added to REDO by the MTR
  @param[in]	    end_lsn		LSN after the last entry was added to REDO by the MTR
  @param[in,out]	observer	flush observer
  */
  Add_dirty_blocks_to_flush_list(lsn_t start_lsn, lsn_t end_lsn, FlushObserver *observer);

  /** Add the modified page to the buffer flush list. */
  /** 将修改过的 block 添加到 buffer flush list 中 */
  void add_dirty_page_to_flush_list(mtr_memo_slot_t *slot) const {
    ut_ad(m_end_lsn > m_start_lsn || (m_end_lsn == 0 && m_start_lsn == 0));
#ifndef UNIV_HOTBACKUP
    buf_block_t *block;
    block = reinterpret_cast<buf_block_t *>(slot->object);
    buf_flush_note_modification(block, m_start_lsn, m_end_lsn, m_flush_observer);
#endif /* !UNIV_HOTBACKUP */
  }

  /** @return true always. */
  //
  // 运算符 () 的重载。
  bool operator()(mtr_memo_slot_t *slot) const {

    if (slot->object != nullptr) {
      //
      if (slot->type == MTR_MEMO_PAGE_X_FIX || slot->type == MTR_MEMO_PAGE_SX_FIX) {
        add_dirty_page_to_flush_list(slot);
      } else if (slot->type == MTR_MEMO_BUF_FIX) {
        buf_block_t *block;
        block = reinterpret_cast<buf_block_t *>(slot->object);
        if (block->made_dirty_with_no_latch) {
          add_dirty_page_to_flush_list(slot);
          block->made_dirty_with_no_latch = false;
        }
      }
    }

    return true;
  }

  /** Mini-transaction REDO end LSN */
  const lsn_t m_end_lsn;

  /** Mini-transaction REDO start LSN */
  const lsn_t m_start_lsn;

  /** Flush observer */
  FlushObserver *const m_flush_observer;
};


/** Constructor.
@param[in]	    start_lsn	LSN of the first entry that was added to REDO by the MTR
@param[in]	    end_lsn		LSN after the last entry was added to REDO by the MTR
@param[in,out]	observer	flush observer */
Add_dirty_blocks_to_flush_list::Add_dirty_blocks_to_flush_list(lsn_t start_lsn, lsn_t end_lsn, FlushObserver *observer)
    : m_end_lsn(end_lsn), m_start_lsn(start_lsn), m_flush_observer(observer) {
  /* Do nothing */
}

class mtr_t::Command {
 public:
  /** Constructor.
  Takes ownership of the mtr->m_impl, is responsible for deleting it.
  @param[in,out]	mtr	Mini-transaction */
  explicit Command(mtr_t *mtr) : m_locks_released() { init(mtr); }

  void init(mtr_t *mtr) {
    m_impl = &mtr->m_impl;
    m_sync = mtr->m_sync;
  }

  /** Destructor */
  ~Command() { ut_ad(m_impl == nullptr); }

  /** Write the redo log record, add dirty pages to the flush list and release the resources. */
  void execute();

  /** Add blocks modified in this mini-transaction to the flush list. */
  void add_dirty_blocks_to_flush_list(lsn_t start_lsn, lsn_t end_lsn);

  /** Release both the latches and blocks used in the mini-transaction. */
  void release_all();

  /** Release the resources */
  void release_resources();

 private:
#ifndef UNIV_HOTBACKUP
  /** Prepare to write the mini-transaction log to the redo log buffer.
  @return number of bytes to write in finish_write() */
  ulint prepare_write();
#endif /* !UNIV_HOTBACKUP */

  /** true if it is a sync mini-transaction. */
  bool m_sync;

  /** The mini-transaction state. */
  mtr_t::Impl *m_impl;

  /** Set to 1 after the user thread releases the latches. The log
  writer thread must wait for this to be set to 1. */
  volatile ulint m_locks_released;
};

/* Mode update matrix. The array is indexed as [old mode][new mode].
All new modes for a specific old mode are in one horizontal line.
true : update to new mode
false: ignore new mode
   A  - MTR_LOG_ALL
   N  - MTR_LOG_NONE
   NR - MTR_LOG_NO_REDO
   S  - MTR_LOG_SHORT_INSERTS */
bool mtr_t::s_mode_update[MTR_LOG_MODE_MAX][MTR_LOG_MODE_MAX] = {
    /*      |  A      N    NR     S  */
    /* A */ {false, true, true, true},   /* A is default and we allow to switch
                                            to all other modes. */
    /* N */ {true, false, true, false},  /* For both A & NR, we can shortly
                                             switch to N and return back*/
    /* NR*/ {false, true, false, false}, /* Default is NR when global redo is
                                            disabled. Allow to move to N */
    /* S */ {true, false, false, false}  /* Only allow return back to A after
                                            short switch from A to S */
};
#ifdef UNIV_DEBUG
/* Mode update validity matrix. The array is indexed as [old mode][new mode]. */
bool mtr_t::s_mode_update_valid[MTR_LOG_MODE_MAX][MTR_LOG_MODE_MAX] = {
    /*      | A      N    NR    S  */
    /* A */ {true, true, true, true}, /* No assert case. */

    /* N */ {true, true, true, true},

    /* NR*/ {true, true, true, true}, /* We generally never return back from
                                         NR to A but need to allow for LOB
                                         restarting B-tree mtr. */

    /* S */ {true, false, false, true} /* Short Insert state is set transiently
                                          and we don't expect N or NR switch. */
};
#endif /* UNIV_DEBUG */

#ifndef UNIV_HOTBACKUP
mtr_t::Logging mtr_t::s_logging;
#endif /* !UNIV_HOTBACKUP */

mtr_log_t mtr_t::set_log_mode(mtr_log_t mode) {
  ut_ad(mode < MTR_LOG_MODE_MAX);

  const auto old_mode = m_impl.m_log_mode;
  ut_ad(s_mode_update_valid[old_mode][mode]);

#ifdef UNIV_DEBUG
  if (mode == MTR_LOG_NO_REDO && old_mode == MTR_LOG_ALL) {
    /* Should change to no redo mode before generating any redo. */
    ut_ad(m_impl.m_n_log_recs == 0);
  }
#endif /* UNIV_DEBUG */

  if (s_mode_update[old_mode][mode]) {
    m_impl.m_log_mode = mode;
  }

#ifndef UNIV_HOTBACKUP
  /* If we are explicitly setting no logging, this mtr doesn't need
  logging and we can safely unmark it. */
  if (mode == MTR_LOG_NO_REDO && mode == old_mode) {
    check_nolog_and_unmark();
    m_impl.m_log_mode = mode;
  }
#endif /* !UNIV_HOTBACKUP */

  return old_mode;
}

/** Check if a mini-transaction is dirtying a clean page.
@return true if the mtr is dirtying a clean page. */
bool mtr_t::is_block_dirtied(const buf_block_t *block) {
  ut_ad(buf_block_get_state(block) == BUF_BLOCK_FILE_PAGE);
  ut_ad(block->page.buf_fix_count > 0);

  /* It is OK to read oldest_modification because no
  other thread can be performing a write of it and it
  is only during write that the value is reset to 0. */
  return !block->page.is_dirty();
}

#ifndef UNIV_HOTBACKUP

/** Write the block contents to the REDO log */
struct mtr_write_log_t {

  /** Append a block to the redo log buffer.
  @return whether the appending should continue */
  //
  // 操作符重载，参数 block 表示将要写入 redo log 的内容
  //
  // 步骤:
  //   首先是调用 log_buffer_write 来写入 block 到 redo log.
  //   更新 m_left_to_write.
  //   如果内容写完则 log_buffer_set_first_record_group ？
  //   调用 log_buffer_write_completed 完成 buffer 的写入
  //   更新 m_lsn , 以便于下次使用.
  //
  // 核心就是两个调用，一个是 log_buffer_write , 一个是 log_buffer_write_completed .
  bool operator()(const mtr_buf_t::block_t *block) {

    lsn_t start_lsn;
    lsn_t end_lsn;

    ut_ad(block != nullptr);

    if (block->used() == 0) {
      return true;
    }

    start_lsn = m_lsn;

    // 1.
    end_lsn = log_buffer_write(*log_sys, m_handle, block->begin(), block->used(), start_lsn);

    ut_a(end_lsn % OS_FILE_LOG_BLOCK_SIZE < OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_TRL_SIZE);

    m_left_to_write -= block->used();

    if (m_left_to_write == 0
        /* This write was up to the end of record group,
        the last record in group has been written.

        Therefore next group of records starts at m_lsn.
        We need to find out, if the next group is the first group,
        that starts in this log block.

        In such case we need to set first_rec_group.

        Now, we could have two cases:
        1. This group of log records has started in previous block
           to block containing m_lsn.
        2. This group of log records has started in the same block
           as block containing m_lsn.

        Only in case 1), the next group of records is the first group
        of log records in block containing m_lsn. */
        && m_handle.start_lsn / OS_FILE_LOG_BLOCK_SIZE != end_lsn / OS_FILE_LOG_BLOCK_SIZE) {
      log_buffer_set_first_record_group(*log_sys, m_handle, end_lsn);
    }

    // 2.
    log_buffer_write_completed(*log_sys, m_handle, start_lsn, end_lsn);

    m_lsn = end_lsn;

    return true;
  }

  Log_handle m_handle;
  lsn_t m_lsn;
  ulint m_left_to_write;
};
#endif /* !UNIV_HOTBACKUP */

#ifdef UNIV_DEBUG
thread_local ut::unordered_set<const mtr_t *> mtr_t::s_my_thread_active_mtrs;
#endif

/** Start a mini-transaction.
@param sync		true if it is a synchronous mini-transaction
@param read_only	true if read only mini-transaction */

// MTR 的启动函数 mtr_t::start 。
// 这个函数包含两个参数，第一个 sync 表示是否当前的 mtr 是同步，第二个是 read_only ，这个表示当前 mtr 是否只读。
// 默认情况下 sync=true, read_only=false 。
//
//
// mtr_start 主要包括：
//   1. 初始化 mtr 的各个状态变量
//   2. 默认模式为 MTR_LOG_ALL ，表示记录所有的数据变更
//   3. mtr 状态设置为 ACTIVE 状态（MTR_STATE_ACTIVE）
//   4. 为锁管理对象和日志管理对象初始化内存（mtr_buf_t），初始化对象链表
//
void mtr_t::start(bool sync, bool read_only) {

  ut_ad(m_impl.m_state == MTR_STATE_INIT || m_impl.m_state == MTR_STATE_COMMITTED);

  UNIV_MEM_INVALID(this, sizeof(*this));
  UNIV_MEM_INVALID(&m_impl, sizeof(m_impl));

  m_sync = sync;

  m_commit_lsn = 0;

  /* 用于记录 redo log 的 mtr 本地 Buffer. */
  new (&m_impl.m_log)  mtr_buf_t();
  new (&m_impl.m_memo) mtr_buf_t();

  /* 初始化 mtr 基础字段. */
  m_impl.m_mtr = this;
  m_impl.m_log_mode = MTR_LOG_ALL;
  m_impl.m_inside_ibuf = false;
  m_impl.m_modifications = false;
  m_impl.m_made_dirty = false;
  m_impl.m_n_log_recs = 0;
  m_impl.m_state = MTR_STATE_ACTIVE;
  m_impl.m_flush_observer = nullptr;
  m_impl.m_marked_nolog = false;

#ifndef UNIV_HOTBACKUP
  check_nolog_and_mark();
#endif /* !UNIV_HOTBACKUP */

  ut_d(m_impl.m_magic_n = MTR_MAGIC_N);

#ifdef UNIV_DEBUG
  auto res = s_my_thread_active_mtrs.insert(this);
  /* Assert there are no collisions in thread local context - it would mean
  reusing MTR without committing or destructing it. */
  ut_a(res.second);
#endif /* UNIV_DEBUG */

}

#ifndef UNIV_HOTBACKUP
void mtr_t::check_nolog_and_mark() {
  /* Safe check to make this call idempotent. */
  if (m_impl.m_marked_nolog) {
    return;
  }

  size_t shard_index = default_indexer_t<>::get_rnd_index();
  m_impl.m_marked_nolog = s_logging.mark_mtr(shard_index);

  /* Disable redo logging by this mtr if logging is globally off. */
  if (m_impl.m_marked_nolog) {
    ut_ad(m_impl.m_log_mode == MTR_LOG_ALL);
    m_impl.m_log_mode = MTR_LOG_NO_REDO;
    m_impl.m_shard_index = shard_index;
  }
}

void mtr_t::check_nolog_and_unmark() {
  if (m_impl.m_marked_nolog) {
    s_logging.unmark_mtr(m_impl.m_shard_index);

    m_impl.m_marked_nolog = false;
    m_impl.m_shard_index = 0;

    if (m_impl.m_log_mode == MTR_LOG_NO_REDO) {
      /* Reset back to default mode. */
      m_impl.m_log_mode = MTR_LOG_ALL;
    }
  }
}
#endif /* !UNIV_HOTBACKUP */

/** Release the resources */
void mtr_t::Command::release_resources() {
  ut_ad(m_impl->m_magic_n == MTR_MAGIC_N);

  /* Currently only used in commit */
  ut_ad(m_impl->m_state == MTR_STATE_COMMITTING);

#ifdef UNIV_DEBUG
  Debug_check release;
  Iterate<Debug_check> iterator(release);

  m_impl->m_memo.for_each_block_in_reverse(iterator);
#endif /* UNIV_DEBUG */

  /* Reset the mtr buffers */
  m_impl->m_log.erase();

  m_impl->m_memo.erase();

  m_impl->m_state = MTR_STATE_COMMITTED;

  m_impl = nullptr;
}

/** Commit a mini-transaction. */


// mtr_commit 时会将 mtr 中的 mlog 和 mblock（dirty page）分别拷贝到 logbuffer 和 flushlist 中。
// 在真实事务提交时，会将该事务涉及的所有 mlog 刷盘，这样各个原子变更就持久化了。
// 恢复的时候按照类型(mtr_type_t)调用相应的回调函数，恢复 page 。
//
// InnoDB存储引擎在内存中维护了一个全局的Redo Log Buffer用以缓存对Redo Log的修改，
// mtr在提交的时候，会将mtr执行过程中产生的本地日志copy到全局Redo Log Buffer中，
// 并将mtr执行过程中修改的数据页（被称做脏页dirty page）加入到一个全局的队列中flush list。
//
// InnoDB存储引擎会根据不同的策略将Redo Log Buffer中的日志落盘，或将flush list中的脏页刷盘并推进Checkpoint。
//
// 在脏页落盘以及Checkpoint推进的过程中，需要严格保证Redo日志先落盘再刷脏页的顺序，
// 在MySQL 8之前，InnoDB存储引擎严格的保证MTR写入Redo Log Buffer的顺序是按照LSN递增的顺序，
// 以及flush list中的脏页按LSN递增顺序排序。
//
// 在多线程并发写入Redo Log Buffer及flush list时，这一约束是通过两个全局锁log_sys_t::mutex和log_sys_t::flush_order_mutex实现的。
//


// 在插入数据过程中，产生的 redo log 都记录在 mtr.m_impl.m_log 中，只有显式提交 mtr 时，才会写到公共 redo log buffer 中，
// 并将对应的脏页加到 flush list 上。
//
// 执行步骤：
//   Step 1: mtr_t::Command::prepare_write() 主要是持有 log_sys->mutex ，做写入前检查。
//   Step 2: mtr_t::Command::finish_write() 将日志从 mtr 中拷贝到公共 redo log buffer。
//   Step 3：如果本次修改产生了脏页，获取 log_sys->log_flush_order_mutex ，随后释放 log_sys->mutex 。
//   Step 4. 将当前 Mtr 修改的脏页加入到 flush list 上，脏页上记录的 lsn 为当前 mtr 写入的结束点 lsn 。
//           基于上述加锁逻辑，能够保证 flush list 上的脏页总是以 LSN 排序。
//   Step 5. 释放 log_sys->log_flush_order_mutex 锁
//   Step 6. 释放当前 mtr 持有的锁（主要是 page latch ）及分配的内存，mtr 完成提交。


void mtr_t::commit() {
  ut_ad(is_active());
  ut_ad(!is_inside_ibuf());
  ut_ad(m_impl.m_magic_n == MTR_MAGIC_N);

  //1. 首先设为 COMMITTING 状态
  m_impl.m_state = MTR_STATE_COMMITTING;

  DBUG_EXECUTE_IF("mtr_commit_crash", DBUG_SUICIDE(););

  Command cmd(this);

  // 2. 然后进行判断是否需要执行所做的修改：
  //    - m_n_log_recs(表示当前 mtr redo log 数目) 大于 0 .
  //    - mtr 修改了 buffer pool pages 并且不生成 redo log 操作
  if (m_impl.m_n_log_recs > 0 || (m_impl.m_modifications && m_impl.m_log_mode == MTR_LOG_NO_REDO)) {
    ut_ad(!srv_read_only_mode || m_impl.m_log_mode == MTR_LOG_NO_REDO);
    // 执行 commit
    cmd.execute();
  } else {
    // 如果 mtr 没有产生 redo 且没有修改数据页，就不用执行 commit 逻辑，仅仅释放资源即可。
    cmd.release_all();
    cmd.release_resources();
  }

#ifndef UNIV_HOTBACKUP
  check_nolog_and_unmark();
#endif /* !UNIV_HOTBACKUP */

  ut_d(remove_from_debug_list());
}


#ifdef UNIV_DEBUG
void mtr_t::remove_from_debug_list() const {
  auto it = s_my_thread_active_mtrs.find(this);
  /* We have to find the MTR that is about to be committed in local context. We
  are not sharing MTRs between threads. */
  ut_a(it != s_my_thread_active_mtrs.end());
  s_my_thread_active_mtrs.erase(it);
}

void mtr_t::check_is_not_latching() const {
  Debug_check_no_latching release;
  Iterate<Debug_check_no_latching> iterator(release);

  m_impl.m_memo.for_each_block_in_reverse(iterator);
}
#endif /* UNIV_DEBUG */

#ifndef UNIV_HOTBACKUP

/** Acquire a tablespace X-latch.
NOTE: use mtr_x_lock_space().
@param[in]	space		tablespace instance
@param[in]	file		file name from where called
@param[in]	line		line number in file */
void mtr_t::x_lock_space(fil_space_t *space, const char *file, ulint line) {
  ut_ad(m_impl.m_magic_n == MTR_MAGIC_N);
  ut_ad(is_active());

  x_lock(&space->latch, file, line);
}

/** Release an object in the memo stack. */
void mtr_t::memo_release(const void *object, ulint type) {
  ut_ad(m_impl.m_magic_n == MTR_MAGIC_N);
  ut_ad(is_active());

  /* We cannot release a page that has been written to in the
  middle of a mini-transaction. */
  ut_ad(!m_impl.m_modifications || type != MTR_MEMO_PAGE_X_FIX);

  Find find(object, type);
  Iterate<Find> iterator(find);

  if (!m_impl.m_memo.for_each_block_in_reverse(iterator)) {
    memo_slot_release(find.m_slot);
  }
}

/** Release a page latch.
@param[in]	ptr	pointer to within a page frame
@param[in]	type	object type: MTR_MEMO_PAGE_X_FIX, ... */
void mtr_t::release_page(const void *ptr, mtr_memo_type_t type) {
  ut_ad(m_impl.m_magic_n == MTR_MAGIC_N);
  ut_ad(is_active());

  /* We cannot release a page that has been written to in the
  middle of a mini-transaction. */
  ut_ad(!m_impl.m_modifications || type != MTR_MEMO_PAGE_X_FIX);

  Find_page find(ptr, type);
  Iterate<Find_page> iterator(find);

  if (!m_impl.m_memo.for_each_block_in_reverse(iterator)) {
    memo_slot_release(find.get_slot());
    return;
  }

  /* The page was not found! */
  ut_ad(0);
}

/** Prepare to write the mini-transaction log to the redo log buffer.
@return number of bytes to write in finish_write() */

// 将 mtr 日志写入 redo log buffer 前的准备工作。
//
// 先来看几个变量.
//   - m_log.size() 这个返回当前 m_log buffer 的字节长度.
//   - m_n_log_recs 这个表示当前 mtr 将要写入的 log record 个数.
//
// 因此 prepare_write 这个函数就是根据 m_n_log_recs 来判断是否是多个 record ，从而来设置不同的标记。
//   - 如果 redo log 记录数目 n_recs 小于等于 1 ，设置 Flag 为 MLOG_SINGLE_REC_FLAG ，标记当前 mtr 只含有一条记录。
//   - 否则，mtr 中包含多条日志记录，在最后附加 MLOG_MULTI_REC_END 类型记录并更新 nrec++ ，用于保证完整性。
//
ulint mtr_t::Command::prepare_write() {

  switch (m_impl->m_log_mode) {
    case MTR_LOG_SHORT_INSERTS:
      ut_ad(0);
      /* fall through (write no redo log) */
    case MTR_LOG_NO_REDO:
    case MTR_LOG_NONE:
      ut_ad(m_impl->m_log.size() == 0);
      return 0;
    case MTR_LOG_ALL:
      break;
    default:
      ut_ad(false);
      return 0;
  }

  /* An ibuf merge could happen when loading page to apply log
  records during recovery. During the ibuf merge mtr is used. */

  ut_a(!recv_recovery_is_on() || !recv_no_ibuf_operations);

  //
  ulint len = m_impl->m_log.size();
  ut_ad(len > 0);

  // 获取 redo log 记录数
  ulint n_recs = m_impl->m_n_log_recs;
  ut_ad(n_recs > 0);
  ut_ad(log_sys != nullptr);
  ut_ad(m_impl->m_n_log_recs == n_recs);
  /* This was not the first time of dirtying a tablespace since the latest checkpoint. */
  ut_ad(n_recs == m_impl->m_n_log_recs);

  // 如果 redo log 记录数目 n_recs 小于等于 1 ，设置 Flag 为 MLOG_SINGLE_REC_FLAG ；
  if (n_recs <= 1) {
    ut_ad(n_recs == 1);
    /* Flag the single log record as the only record in this mini-transaction. */
    /* 标记 mtr 中只有一条数据。*/
    *m_impl->m_log.front()->begin() |= MLOG_SINGLE_REC_FLAG;
  // 否则，设置 Flag 为 MLOG_MULTI_REC_END 。
  } else {
    /* Because this mini-transaction comprises multiple log records, append MLOG_MULTI_REC_END at the end. */
    /* 因为 mtr 中包含多条日志记录，所以在最后附加 MLOG_MULTI_REC_END 类型记录，用于保证完整性。 */
    mlog_catenate_ulint(&m_impl->m_log, MLOG_MULTI_REC_END, MLOG_1BYTE);
    ++len;
  }

  ut_ad(m_impl->m_log_mode == MTR_LOG_ALL);
  ut_ad(m_impl->m_log.size() == len);
  ut_ad(len > 0);
  return len;
}
#endif /* !UNIV_HOTBACKUP */

/** Release the latches and blocks acquired by this mini-transaction */
void mtr_t::Command::release_all() {
  Release_all release;
  Iterate<Release_all> iterator(release);

  m_impl->m_memo.for_each_block_in_reverse(iterator);

  /* Note that we have released the latches. */
  m_locks_released = 1;
}

/** Add blocks modified in this mini-transaction to the flush list. */
void mtr_t::Command::add_dirty_blocks_to_flush_list(lsn_t start_lsn, lsn_t end_lsn) {
  // 通过重载 () 操作符实现核心逻辑
  Add_dirty_blocks_to_flush_list add_to_flush(start_lsn, end_lsn, m_impl->m_flush_observer);

  // 迭代器封装，当调用 iterator 的 () 时会调用底层对象的 () ，也即 Add_dirty_blocks_to_flush_list 的 ()
  Iterate<Add_dirty_blocks_to_flush_list> iterator(add_to_flush);

  // m_memo.for_each_block_in_reverse 比较简单，就是从尾部开始遍历，然后调用 iterator 的 ()
  m_impl->m_memo.for_each_block_in_reverse(iterator);
}

/** Write the redo log record, add dirty pages to the flush list and release the resources. */
//
// 在 mtr 提交过程中，最重要的过程是 cmd.execute() ，首先通过 prepare_write 得到最终要写入的日志长度，分为5步：
//
//  - log_buffer_reserve：等待recent_written的空间，预留logbuffer的空间，如果空间不够，会调用log_write_up_to清理LogBuffer空间；log_write_up_to通过设置writer_event，异步触发log_writer写。
//  - write_log：将m_log的内容memcpy到LogBuffer中，然后在recent_written加一个link。
//  - log_wait_for_space_in_log_recent_closed：等待recent_closed的空间
//  - add_dirty_block_to_flush_list：将该mtr对应的脏页添加到flushlist中
//  - log_buffer_close：在recent_closed中加一个link。
//
// 具体流程如下，其中 LinkBuf，recent_written ，recent_closed 等结构体涉及到 redo log 的无锁优化，后文会具体介绍。
//
//  |--> mtr_t::Command::execute
//  |    |--> //在excecute中会先进行写之前的操作，主要是进行一些校验以及最终返回将要写入的redolog长度
//  |    |--> mtr_t::Command::prepare_write
//  |    |--> //进入excecute阶段，首先构造mtr_write_log_t结构
//  |    |--> mtr_write_log_t write_log;
//  |    |--> //1.分配log buf以及初始化write_log.
//  |    |--> //不同的mtr会首先调用log_buffer_reserve函数获得起始和终点lsn位置
//  |    |--> log_buffer_reserve
//  |    |    |--> log_buffer_s_lock_enter
//  |    |    |    |--> log.sn_lock.s_lock
//  |    |    |--> //用自己的REDO长度，原子地对全局偏移log.sn做fetch_add，得到自己在Log Buffer中独享的空间
//  |    |    |--> log.sn.fetch_add
//  |    |    |--> //不同mtr并行的将自己的m_log中的数据拷贝到各自独享的空间内。
//  |    |    |--> log_wait_for_space_after_reserving
//  |    |    |    |--> //确保这条 redo log 能完整的写入 redo log Buffer，而且回环后不会覆盖尚未写入磁盘的 redo log.
//  |    |    |    |--> log_wait_for_space_in_log_buf
//  |    |    |    |    |--> log_write_up_to
//  |    |    |    |    |    |--> log_wait_for_write
//  |    |    |    |    |    |    |--> //log_write_notify会唤醒这个条件变量
//  |    |    |    |    |    |    |--> os_event_wait_for(log.write_events[slot],stop_condition);
//  |    |    |    |    |    |--> log_wait_for_flush
//  |    |--> //2.从mtr的buffer中写入内容到redolog的buffer.
//  |    |--> m_impl->m_log.for_each_block(write_log)
//  |    |    |--> mtr_write_log_t::operator
//  |    |    |    |--> //写入到redo log buffer(log->buf).
//  |    |    |    |--> log_buffer_write
//  |    |    |    |--> //拷贝完成后触发LinkBuf更新,更新recent_written字段
//  |    |    |    |--> log_buffer_write_completed
//  |    |    |    |    |--> //更新本次写入的内容范围对应的LinkBuf内特定的数组项值
//  |    |    |    |    |--> log.recent_written.add_link
//  |    |--> //3.在加脏页之前需要判断是否link buf已满。
//  |    |--> log_wait_for_space_in_log_recent_closed
//  |    |--> //4.加脏页到flush list中.
//  |    |--> add_dirty_blocks_to_flush_list
//  |    |    |--> add_dirty_page_to_flush_list
//  |    |--> log_buffer_close
//  |    |    |--> //5.更新recent_closed字段
//  |    |    |--> log.recent_closed.add_link(start_lsn, end_lsn);
//
//
void mtr_t::Command::execute() {
  ut_ad(m_impl->m_log_mode != MTR_LOG_NONE);

#ifndef UNIV_HOTBACKUP

  // 获取 redo log 的大小
  ulint len = prepare_write();

  // 如果 redo log 的长度不为 0 时, 调用 log_buffer_reserve() 确保全局 redo log buffer 中有空闲空间。
  if (len > 0) {

    // 首先构造一个 mtr_write_log_t 结构，用于将 mtr 中的日志写入到 redo log buffer 中.
    mtr_write_log_t write_log;

    // m_left_to_write 表示还需要写入到 redo log 的内容的长度，因此该值默认就是 len .
    write_log.m_left_to_write = len;

    // 调用 log_buffer_reserve() 确保全局 redo log buffer 中有足够(len)的空闲空间，并返回 Log_handle(start_lsn, end_lsn) 给本 mtr 使用。
     auto handle = log_buffer_reserve(*log_sys, len);

    write_log.m_handle = handle;

    // 取出 redo log 的 start_lsn
    write_log.m_lsn = handle.start_lsn;

    // 将 mtr 的 buffer 写入到 redo log buffer 中
    m_impl->m_log.for_each_block(write_log);

    ut_ad(write_log.m_left_to_write == 0);
    ut_ad(write_log.m_lsn == handle.end_lsn);

    // 等待 flush list 中的无序度降到阈值以内 recent_closed.has_space(start_lsn)
    log_wait_for_space_in_log_recent_closed(*log_sys, handle.start_lsn);

    DEBUG_SYNC_C("mtr_redo_before_add_dirty_blocks");

    // 把脏页 dirty_blocks 拷贝到 flush list 中
    add_dirty_blocks_to_flush_list(handle.start_lsn, handle.end_lsn);

    // 把脏页插入 Buffer Pool 中的 flush_list 后，更新 log_t 中的 recent_closed 链表.
    log_buffer_close(*log_sys, handle);

    m_impl->m_mtr->m_commit_lsn = handle.end_lsn;

  } else {
  // 否则，redo log 的长度为 0 ，直接调用 add_dirty_blocks_to_flush_list(0, 0)
    DEBUG_SYNC_C("mtr_noredo_before_add_dirty_blocks");
    add_dirty_blocks_to_flush_list(0, 0);
  }

#endif /* !UNIV_HOTBACKUP */


  //
  release_all();

  // 将 m_state 置为 MTR_STATE_COMMITTED
  release_resources();
}



#ifndef UNIV_HOTBACKUP
int mtr_t::Logging::enable(THD *thd) {
  if (is_enabled()) {
    return 0;
  }
  /* Allow mtrs to generate redo log. Concurrent clone and redo
  log archiving is still restricted till we reach a recoverable state. */
  ut_ad(m_state.load() == DISABLED);
  m_state.store(ENABLED_RESTRICT);

  /* 1. Wait for all no-log mtrs to finish and add dirty pages to disk.*/
  auto err = wait_no_log_mtr(thd);
  if (err != 0) {
    m_state.store(DISABLED);
    return err;
  }

  /* 2. Wait for dirty pages to flush by forcing checkpoint at current LSN.
  All no-logging page modification are done with the LSN when we stopped
  redo logging. We need to have one write mini-transaction after enabling redo
  to progress the system LSN and take a checkpoint. An easy way is to flush
  the max transaction ID which is generally done at TRX_SYS_TRX_ID_WRITE_MARGIN
  interval but safe to do any time. */
  trx_sys_mutex_enter();
  trx_sys_flush_max_trx_id();
  trx_sys_mutex_exit();

  /* It would ensure that the modified page in previous mtr and all other
  pages modified before are flushed to disk. Since there could be large
  number of left over pages from LAD operation, we still don't enable
  double-write at this stage. */
  log_make_latest_checkpoint(*log_sys);
  m_state.store(ENABLED_DBLWR);

  /* 3. Take another checkpoint after enabling double write to ensure any page
  being written without double write are already synced to disk. */
  log_make_latest_checkpoint(*log_sys);

  /* 4. Mark that it is safe to recover from crash. */
  log_persist_enable(*log_sys);

  ib::warn(ER_IB_WRN_REDO_ENABLED);
  m_state.store(ENABLED);

  return 0;
}

int mtr_t::Logging::disable(THD *) {
  if (is_disabled()) {
    return 0;
  }

  /* Disallow archiving to start. */
  ut_ad(m_state.load() == ENABLED);
  m_state.store(ENABLED_RESTRICT);

  /* Check if redo log archiving is active. */
  if (meb::redo_log_archive_is_active()) {
    m_state.store(ENABLED);
    my_error(ER_INNODB_REDO_ARCHIVING_ENABLED, MYF(0));
    return ER_INNODB_REDO_ARCHIVING_ENABLED;
  }

  /* Concurrent clone is blocked by BACKUP MDL lock except when
  clone_ddl_timeout = 0. Force any existing clone to abort. */
  clone_mark_abort(true);
  ut_ad(!clone_check_active());

  /* Mark that it is unsafe to crash going forward. */
  log_persist_disable(*log_sys);

  ib::warn(ER_IB_WRN_REDO_DISABLED);
  m_state.store(DISABLED);

  clone_mark_active();

  return 0;
}

int mtr_t::Logging::wait_no_log_mtr(THD *thd) {
  auto wait_cond = [&](bool alert, bool &result) {
    if (Counter::total(m_count_nologging_mtr) == 0) {
      result = false;
      return 0;
    }
    result = true;

    if (thd_killed(thd)) {
      my_error(ER_QUERY_INTERRUPTED, MYF(0));
      return ER_QUERY_INTERRUPTED;
    }
    return 0;
  };

  /* Sleep for 1 millisecond */
  Clone_Msec sleep_time(10);
  /* Generate alert message every 5 second. */
  Clone_Sec alert_interval(5);
  /* Wait for 5 minutes. */
  Clone_Sec time_out(Clone_Min(5));

  bool is_timeout = false;
  auto err = Clone_Sys::wait(sleep_time, time_out, alert_interval, wait_cond, nullptr, is_timeout);

  if (err == 0 && is_timeout) {
    ut_ad(false);
    my_error(ER_INTERNAL_ERROR, MYF(0), "Innodb wait for no-log mtr timed out.");
    err = ER_INTERNAL_ERROR;
  }

  return err;
}

#ifdef UNIV_DEBUG
/** Check if memo contains the given item.
@return	true if contains */
bool mtr_t::memo_contains(const mtr_buf_t *memo, const void *object, ulint type) {
  Find find(object, type);
  Iterate<Find> iterator(find);

  return !memo->for_each_block_in_reverse(iterator);
}

/** Debug check for flags */
struct FlaggedCheck {
  FlaggedCheck(const void *ptr, ulint flags) : m_ptr(ptr), m_flags(flags) {
    // Do nothing
  }

  bool operator()(const mtr_memo_slot_t *slot) const {
    if (m_ptr == slot->object && (m_flags & slot->type)) {
      return false;
    }

    return true;
  }

  const void *m_ptr;
  ulint m_flags;
};

/** Check if memo contains the given item.
@param ptr		object to search
@param flags		specify types of object (can be ORred) of
                        MTR_MEMO_PAGE_S_FIX ... values
@return true if contains */
bool mtr_t::memo_contains_flagged(const void *ptr, ulint flags) const {
  ut_ad(m_impl.m_magic_n == MTR_MAGIC_N);
  ut_ad(is_committing() || is_active());

  FlaggedCheck check(ptr, flags);
  Iterate<FlaggedCheck> iterator(check);

  return !m_impl.m_memo.for_each_block_in_reverse(iterator);
}

/** Check if memo contains the given page.
@param[in]	ptr	pointer to within buffer frame
@param[in]	flags	specify types of object with OR of
                        MTR_MEMO_PAGE_S_FIX... values
@return	the block
@retval	NULL	if not found */
buf_block_t *mtr_t::memo_contains_page_flagged(const byte *ptr,
                                               ulint flags) const {
  Find_page check(ptr, flags);
  Iterate<Find_page> iterator(check);

  return m_impl.m_memo.for_each_block_in_reverse(iterator) ? nullptr
                                                           : check.get_block();
}

/** Mark the given latched page as modified.
@param[in]	ptr	pointer to within buffer frame */
void mtr_t::memo_modify_page(const byte *ptr) {
  buf_block_t *block = memo_contains_page_flagged(
      ptr, MTR_MEMO_PAGE_X_FIX | MTR_MEMO_PAGE_SX_FIX);
  ut_ad(block != nullptr);

  if (!memo_contains(get_memo(), block, MTR_MEMO_MODIFY)) {
    memo_push(block, MTR_MEMO_MODIFY);
  }
}

/** Print info of an mtr handle. */
void mtr_t::print() const {
  ib::info(ER_IB_MSG_1275) << "Mini-transaction handle: memo size "
                           << m_impl.m_memo.size() << " bytes log size "
                           << get_log()->size() << " bytes";
}

lsn_t mtr_commit_mlog_test(log_t &log, size_t payload) {
  constexpr size_t MAX_PAYLOAD_SIZE = 1024;
  ut_a(payload <= MAX_PAYLOAD_SIZE);

  /* Create MLOG_TEST record in the memory. */
  byte record[MLOG_TEST_REC_OVERHEAD + MAX_PAYLOAD_SIZE];

  byte *record_end =
      Log_test::create_mlog_rec(record, 1, MLOG_TEST_VALUE, payload);

  const size_t rec_len = record_end - record;

  mtr_t mtr;
  mtr_start(&mtr);

  /* Copy the created MLOG_TEST to mtr's local buffer. */
  byte *dst = nullptr;

  //
  bool success = mlog_open(&mtr, rec_len, dst);


  ut_a(success);
  std::memcpy(dst, record, rec_len);
  mlog_close(&mtr, dst + rec_len);

  mtr.added_rec();

  ut_ad(mtr.get_expected_log_size() == MLOG_TEST_REC_OVERHEAD + payload);

  mtr_commit(&mtr);

  return mtr.commit_lsn();
}

static void mtr_commit_mlog_test_filling_block_low(log_t &log,
                                                   size_t req_space_left,
                                                   size_t recursive_level) {
  ut_a(req_space_left <= LOG_BLOCK_DATA_SIZE);

  /* Compute how much free space we have in current log block. */
  const lsn_t current_lsn = log_get_lsn(log);
  size_t cur_space_left = OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_TRL_SIZE -
                          current_lsn % OS_FILE_LOG_BLOCK_SIZE;

  /* Subtract minimum space required for a single MLOG_TEST. */
  if (cur_space_left < MLOG_TEST_REC_OVERHEAD) {
    /* Even the smallest MLOG_TEST was not fitting the left space,
    so we will need to use the next log block too. */
    cur_space_left += LOG_BLOCK_DATA_SIZE;
  }
  cur_space_left -= MLOG_TEST_REC_OVERHEAD;

  /* Compute how big payload is required to leave exactly the provided
  req_space_left bytes free in last block. */
  size_t payload;
  if (cur_space_left < req_space_left) {
    /* We requested to leave more free bytes, than we have currently
    in the last block, we need to use the next log block. */
    payload = cur_space_left + LOG_BLOCK_DATA_SIZE - req_space_left;
  } else {
    payload = cur_space_left - req_space_left;
  }

  /* Check if size of the record fits the maximum allowed size, which
  is defined by the dyn_buf_t used in mtr_t (mtr_buf_t). */

  if (MLOG_TEST_REC_OVERHEAD + payload <= mtr_buf_t::MAX_DATA_SIZE) {
    mtr_commit_mlog_test(*log_sys, payload);
  } else {
    /* It does not fit, so we need to write as much as possible here,
    but keep in mind that next record will need to take at least
    MLOG_TEST_REC_OVERHEAD bytes. Fortunately the MAX_DATA_SIZE is
    always at least twice larger than the MLOG_TEST_REC_OVERHEAD,
    so the payload has to be larger than MLOG_TEST_REC_OVERHEAD. */
    ut_ad(mtr_buf_t::MAX_DATA_SIZE >= MLOG_TEST_REC_OVERHEAD * 2);
    ut_a(payload > MLOG_TEST_REC_OVERHEAD);

    /* Subtract space which we will consume by usage of next record.
    The remaining space is maximum we are allowed to consume within
    this record. */
    payload -= MLOG_TEST_REC_OVERHEAD;

    if (MLOG_TEST_REC_OVERHEAD + payload > mtr_buf_t::MAX_DATA_SIZE) {
      /* We still cannot fit mtr_buf_t::MAX_DATA_SIZE bytes, so write
      as much as possible within this record. */
      payload = mtr_buf_t::MAX_DATA_SIZE - MLOG_TEST_REC_OVERHEAD;
    }

    /* Write this MLOG_TEST record. */
    mtr_commit_mlog_test(*log_sys, payload);

    /* Compute upper bound for maximum level of recursion that is ever possible.
    This is to verify the guarantee that we don't go to deep.

    We do not want to depend on actual difference between the
    mtr_buf_t::MAX_DATA_SIZE and LOG_BLOCK_DATA_SIZE.

    Note that mtr_buf_t::MAX_DATA_SIZE is the maximum size of log record we
    could add. The LOG_BLOCK_DATA_SIZE consists of LOG_BLOCK_DATA_SIZE /
    mtr_buf_t::MAX_DATA_SIZE records of mtr_buf_t::MAX_DATA_SIZE size each (0 if
    MAX_DATA_SIZE is larger than the LOG_BLOCK_DATA_SIZE). If we shifted these
    records then possibly 2 more records are required at boundaries (beginning
    and ending) to cover the whole range. If the last record would not end at
    proper offset, we decrease its payload. If we needed to move its end to even
    smaller offset from beginning of log block than we reach with payload=0,
    then we subtract up to MLOG_TEST_REC_OVERHEAD bytes from payload of previous
    record, which is always possible because:
      MAX_DATA_SIZE - MLOG_TEST_REC_OVERHEAD >= MLOG_TEST_REC_OVERHEAD.

    If the initial free space minus MLOG_TEST_REC_OVERHEAD is smaller than the
    requested free space, then we need to move forward by at most
    LOG_BLOCK_DATA_SIZE bytes. For that we need at most LOG_BLOCK_DATA_SIZE /
    mtr_buf_t::MAX_DATA_SIZE + 2 records shifted in the way described above.

    This solution is reached by the loop of writing MAX_DATA_SIZE records until
    the distance to target is <= MAX_DATA_SIZE + MLOG_TEST_REC_OVERHEAD, in
    which case we adjust size of next record to end it exactly
    MLOG_TEST_REC_OVERHEAD bytes before the target (this is why we subtract
    MLOG_TEST_REC_OVERHEAD from payload). Then next recursive call will have an
    easy task of adding record with payload=0. The loop mentioned above is
    implemented by the recursion. */
    constexpr auto MAX_REC_N = LOG_BLOCK_DATA_SIZE / mtr_buf_t::MAX_DATA_SIZE + 2;

    ut_a(recursive_level + 1 <= MAX_REC_N);

    /* Write next MLOG_TEST record(s). */
    mtr_commit_mlog_test_filling_block_low(log, req_space_left, recursive_level + 1);
  }
}

void mtr_commit_mlog_test_filling_block(log_t &log, size_t req_space_left) {
  mtr_commit_mlog_test_filling_block_low(log, req_space_left, 1);
}

void mtr_t::wait_for_flush() {
  ut_ad(commit_lsn() > 0);
  log_write_up_to(*log_sys, commit_lsn(), true);
}

#endif /* UNIV_DEBUG */
#endif /* !UNIV_HOTBACKUP */
