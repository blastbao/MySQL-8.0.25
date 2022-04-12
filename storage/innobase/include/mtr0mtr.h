/*****************************************************************************

Copyright (c) 1995, 2021, Oracle and/or its affiliates.
Copyright (c) 2012, Facebook Inc.

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

/** @file include/mtr0mtr.h
 Mini-transaction buffer

 Created 11/26/1995 Heikki Tuuri
 *******************************************************/

#ifndef mtr0mtr_h
#define mtr0mtr_h

#include <stddef.h>

#include "univ.i"

#include "buf0types.h"
#include "dyn0buf.h"
#include "fil0fil.h"
#include "log0types.h"
#include "mtr0types.h"
#include "srv0srv.h"
#include "trx0types.h"

/** Start a mini-transaction. */
#define mtr_start(m) (m)->start()

/** Start a synchronous mini-transaction */
#define mtr_start_sync(m) (m)->start(true)

/** Start an asynchronous read-only mini-transaction */
#define mtr_start_ro(m) (m)->start(true, true)

/** Commit a mini-transaction. */
#define mtr_commit(m) (m)->commit()

/** Set and return a savepoint in mtr.
@return	savepoint */
#define mtr_set_savepoint(m) (m)->get_savepoint()

/** Release the (index tree) s-latch stored in an mtr memo after a
savepoint. */
#define mtr_release_s_latch_at_savepoint(m, s, l) \
  (m)->release_s_latch_at_savepoint((s), (l))

/** Get the logging mode of a mini-transaction.
@return	logging mode: MTR_LOG_NONE, ... */
#define mtr_get_log_mode(m) (m)->get_log_mode()

/** Change the logging mode of a mini-transaction.
@return	old mode */
#define mtr_set_log_mode(m, d) (m)->set_log_mode((d))

/** Get the flush observer of a mini-transaction.
@return flush observer object */
#define mtr_get_flush_observer(m) (m)->get_flush_observer()

/** Set the flush observer of a mini-transaction. */
#define mtr_set_flush_observer(m, d) (m)->set_flush_observer((d))

/** Read 1 - 4 bytes from a file page buffered in the buffer pool.
@return	value read */
#define mtr_read_ulint(p, t, m) (m)->read_ulint((p), (t))

/** Release an object in the memo stack.
@return true if released */
#define mtr_memo_release(m, o, t) (m)->memo_release((o), (t))

#ifdef UNIV_DEBUG

/** Check if memo contains the given item ignore if table is intrinsic
@return true if contains or table is intrinsic. */
#define mtr_is_block_fix(m, o, t, table) \
  (mtr_memo_contains(m, o, t) || table->is_intrinsic())

/** Check if memo contains the given page ignore if table is intrinsic
@return true if contains or table is intrinsic. */
#define mtr_is_page_fix(m, p, t, table) \
  (mtr_memo_contains_page(m, p, t) || table->is_intrinsic())

/** Check if memo contains the given item.
@return	true if contains */
/// 判断锁对象是否在memo当中
#define mtr_memo_contains(m, o, t) (m)->memo_contains((m)->get_memo(), (o), (t))

/** Check if memo contains the given page.
@return	true if contains */
#define mtr_memo_contains_page(m, p, t) \
  (m)->memo_contains_page_flagged((p), (t))
#endif /* UNIV_DEBUG */

/** Print info of an mtr handle. */
#define mtr_print(m) (m)->print()

/** Return the log object of a mini-transaction buffer.
@return	log */
#define mtr_get_log(m) (m)->get_log()


// memo 的 latch 管理接口：
//    mtr_memo_push	                    获得一个 latch，并将状态信息存入 mtr memo 当中
//    mtr_memo_slot_t                   保存 latch 内容
//    mtr_release_s_latch_at_savepoint	释放 memo 偏移 savepoint 的 slot 锁状态
//    mtr_memo_contains	                判断锁对象是否在 memo 当中
//    mtr_memo_slot_release	            释放 slot 锁的控制权
//    mtr_memo_pop_all	                释放所有 memo 中的锁的控制权


/** Push an object to an mtr memo stack. */
//  获得一个 latch ，并将状态信息存入 mtr memo 中
#define mtr_memo_push(m, o, t) (m)->memo_push(o, t)

/** Lock an rw-lock in s-mode. */
#define mtr_s_lock(l, m) (m)->s_lock((l), __FILE__, __LINE__)

/** Lock an rw-lock in x-mode. */
#define mtr_x_lock(l, m) (m)->x_lock((l), __FILE__, __LINE__)

/** Lock a tablespace in x-mode. */
#define mtr_x_lock_space(s, m) (m)->x_lock_space((s), __FILE__, __LINE__)

/** Lock an rw-lock in sx-mode. */
#define mtr_sx_lock(l, m) (m)->sx_lock((l), __FILE__, __LINE__)

#define mtr_memo_contains_flagged(m, p, l) (m)->memo_contains_flagged((p), (l))

#define mtr_memo_contains_page_flagged(m, p, l) \
  (m)->memo_contains_page_flagged((p), (l))

/// 释放memo偏移savepoint的slot锁状态
#define mtr_release_block_at_savepoint(m, s, b) \
  (m)->release_block_at_savepoint((s), (b))

#define mtr_block_sx_latch_at_savepoint(m, s, b) \
  (m)->sx_latch_at_savepoint((s), (b))

#define mtr_block_x_latch_at_savepoint(m, s, b) \
  (m)->x_latch_at_savepoint((s), (b))

/** Check if a mini-transaction is dirtying a clean page.
@param b	block being x-fixed
@return true if the mtr is dirtying a clean page. */
#define mtr_block_dirtied(b) mtr_t::is_block_dirtied((b))

/** Forward declaration of a tablespace object */
struct fil_space_t;

/** Mini-transaction memo stack slot. */
//
// 记录加锁的对象和加锁的类型。
struct mtr_memo_slot_t {
  /** pointer to the object */
  /* 加锁的对象. */
  void *object;

  /** type of the stored object (MTR_MEMO_S_LOCK, ...) */
  /* 持有的锁类型 */
  /* 类型如下：
      MTR_MEMO_PAGE_S_FIX	/rw_locks-latch/
      MTR_MEMO_PAGE_X_FIX	/rw_lockx-latch/
      MTR_MEMO_BUF_FIX	    /buf_block_t/
      MTR_MEMO_S_LOCK	    /rw_lock s-latch/
      MTR_MEMO_X_LOCK	    /rw_lock x-latch/
  */
  ulint type;
};


// [背景介绍]
//
// innodb 存储引擎中的一个很重要的用来保证持久性的机制就是 mtr ，本书把它称做“物理事务”，这样叫是相对逻辑事务而言的。
//
// 对于逻辑事务，做熟悉数据库的人都很清楚，它是数据库区别于文件系统的最重要特性之一，它具有四个特性 ACID ，
// 用来保证数据库的完整性——要么都做修改，要么什么都没有做。
//
// 对于物理事务，从名字来看，是物理的，因为在 innodb 存储引擎中，只要是涉及到文件修改、文件读取等物理操作，都离不开这个物理事务，
// 可以说物理事务是内存与文件之间的一个桥梁。
//
// innodb 在访问一个文件页面的时候，会将要访问的页面载入到页面缓冲区中，然后才可以访问它，此时可以读取或者更新这个页面，
// 在将更新写回到文件中之前，这个页面都会处于缓冲区中。
// 在这个过程中，有一个机制一直扮演着很重要的角色，那就是物理事务。
//
//
// 物理事务既然被称为事务，那它同样有事务的开始与提交，在 innodb 中，物理事务的开始其实就是对物理事务的结构体 mtr_t 的初始化，
// 在修改或读一个数据文件中的数据时，一般是通过 mtr 来控制对对应 page 或者索引树的加锁，在 5.7 中，有几种锁类型（mtr_memo_type_t）。
//

// [原理]
//
// [物理事务执行过程]
//   mtr_t mtr
//   mtr.start()
//
//   /* ... */
//   /* 读数据 */
//   /* 写数据至 mtr 的 m_log. */
//   /* ... */
//
//   mtr.commit()
//
//
// [物理事务的读写过程]
// 在系统将一个页面载入到缓冲区的时候，需要新开始一个（mtr_start）或者有一个已经开始的物理事务，
// 载入时需要指定页面的获取方式，比如是用来读取的还是用来修改的，这样会影响物理事务对这个页面的上锁情况，
// 如果用来修改，则上X锁，否则上S锁（当然还可以指定不上锁）。
//
// 在确定了获取方式、这个页面的表空间 ID 及页面号之后，就可以通过函数 buf_page_get 来获取指定页面了。
//
// 当找到相应页面后，物理事务就要对它上指定的锁，此时需要对这个页面的上锁情况进行检查，
// 一个页面的上锁情况是在结构体 buf_block_struct 中的 lock 中体现的，此时如果这个页面还没有上锁，
// 则这个物理事务直接对其上锁，否则还需要考虑两个锁的兼容性，只有两个锁都是共享锁（S）的情况下才是可以上锁成功的，否则需要等待。
//
// 当上锁成功后，物理事务会将这个页面的内存结构存储到 mtr.memo 动态数组中，然后这个物理事务就可以访问这个页面了。
//
// 物理事务对页面的访问包括两种操作，一种是读，另一种是写，读就是简单读取其指定页面内偏移及长度的数据；
// 写则是指定从某一偏移开始写入指定长度的新数据，同时如果这个物理事务是写日志的（MTR_LOG_ALL），
// 此时还需要对刚才的写操作记下日志，这里的日志就逻辑事务中提到的 REDO 日志。
//
// 写下相应的日志之后，同样将其存储到 mtr.log 动态数组中，同时要将上面结构体中的 n_log_recs 自增，维护这个物理事务的日志计数值。
//
//
// [物理事务的提交]
// 物理事务的提交是通过 mtr_commit 来实现的，主要是将所有这个物理事务产生的日志写入到 innodb 的日志系统的日志缓冲区中，
// 然后等待 srv_master_thread 线程定时将日志系统的日志缓冲区中的日志数据刷到日志文件中，这一部分会单独在其它章节点讲述。
//
// 上面已经讲过，物理事务和逻辑事务一样，也是可以保证数据库操作的完整性的，一般说来，一个操作必须要在一个物理事务中完成，
// 也就是指要么这个操作已经完成，要么什么也没有做，否则有可能造成数据不完整的问题，因为在数据库系统做 REDO 操作时是以
// 物理事务为单位做的，如果一个物理事务的日志是不完整的，则它对应的所有日志都不会重做，那么如何辨别一个物理事务是否完整呢？
// 这个问题是在物理事务提交时用了个很巧妙的方法保证了，在提交前，如果发现这个物理事务有日志，则在日志最后再写一些特殊的日志，
// 这些特殊的日志就是一个物理事务结束的标志，那么提交时一起将这些特殊的日志写入，在重做时如果当前这一批日志信息最后面存在这个标志，
// 则说明这些日志是完整的，否则就是不完整的，则不会重做。
//
// 物理事务提交时还有一项很重要的工作就是处理上面结构体中动态数组 memo 中的内容，现在都已经知道这个数组中存储的是这个物理事务所有访问过的页面，
// 并且都已经上了锁，那么在它提交时，如果发现这些页面中有已经被修改过的，则这些页面就成为了脏页，这些脏页需要被加入到 innodb 的 buffer 缓冲区
// 中的更新链表中，当然如果已经在更新链中，则直接跳过（不能重复加入），svr_master_thread 线程会定时检查这个链表，将一定数目的脏页刷到磁盘中；
// 加入之后还需要将这个页面上的锁释放掉，表示这个页面已经处理完成；如果页面没有被修改，或者只是用来读取数据的，则只需要直接将其共享锁（S锁）释放掉即可。
//
// 上面的内容就是物理事务的一个完整的讲述，因为它是比较底层的一个模块，牵扯的东西比较多，
// 这里重点讲述了物理事务的意义、操作原理、与 BUFFER 系统的关联、日志的产生等内容。
//
//
//
//
//
//




// [原理]
//
// InnoDB 的 redo log 都是通过 mtr 产生的，先写到 mtr 的 cache 中，然后在提交时写入到公共 buffer 中。
//
// 加锁、写日志到 mlog 等操作在 mtr 过程中进行，解锁、把日志刷盘等操作全部在 mtr_commit 中进行，和事务类似。
// mtr 没有回滚操作， 因为只有在 mtr_commit 才将修改落盘，如果宕机，内存丢失，无需回滚；
// 如果落盘过程中宕机，崩溃恢复时可以看出落盘过程不完整，丢弃这部分修改。
//
//
// mtr_commit 主要包含以下步骤:
//   1. 将 mlog 中日志写入的是 redo log buffer ，具体落盘时机为：事务提交、log buffer 的空间使用过半、log CHECKPOINT 等等；
//   2. 释放 mtr 持有的锁，锁信息保存在 memo 中，以栈形式保存，后加的锁先释放；
//   3. 清理 mtr 申请的内存空间，memo 和 log ；
//   4. mtr—>state 设置为 MTR_COMMITTED 。
//
//
//
// 有几种场景可能会触发 redo log 写文件：
//   Redo log buffer 空间不足时
//   事务提交
//   后台线程
//   做 checkpoint
//   实例 shutdown 时
//   binlog 切换时
//
// 我们所熟悉的参数 innodb_flush_log_at_trx_commit 作用于事务提交时，这也是最常见的场景。
//
// 在步骤 1 中，日志刷盘策略和 innodb_flush_log_at_trx_commit 有关
//  - 当设置为 1 时，每次事务提交都要做一次 fsync ，这是最安全的配置，即使宕机也不会丢失事务；
//  - 当设置为 2 时，则在事务提交时只做 write 操作，只保证写到系统的 page cache ，因此实例 crash 不会丢失事务，但宕机则可能丢失事务；
//  - 当设置为 0 时，事务提交不会触发 redo 写操作，而是留给后台线程每秒一次的刷盘操作，因此实例 crash 将最多丢失 1 秒钟内的事务。
//
// 显然对性能的影响是随着持久化程度的增加而增加的。通常我们建议在日常场景将该值设置为 1 ，但在系统高峰期临时修改成 2 以应对大负载。
//
// 由于各个事务可以交叉的将事务日志拷贝到 log buffer 中，因而一次事务提交触发的写 redo 到文件，
// 可能隐式的帮别的线程“顺便”也写了 redo log ，从而达到 group commit 的效果。
//
//
//
//
//

/** Mini-transaction handle and buffer */
//
//     变量名	                  描述
//  mtr_buf_t m_memo	        用于存储该 mtr 持有的锁类型
//  mtr_buf_t m_log	            存储 redo log 记录
//  bool m_made_dirty	        是否产生了至少一个脏页
//  bool m_inside_ibuf	        是否在操作 change buffer
//  bool m_modifications	    是否修改了 buffer pool page
//  ib_uint32_t m_n_log_recs    该 mtr log 记录个数
//  mtr_log_t m_log_mode	    Mtr 的工作模式，包括四种：
//                                  MTR_LOG_ALL：默认模式，记录所有会修改磁盘数据的操作；
//                                  MTR_LOG_NONE：不记录redo，脏页也不放到flush list上；
//                                  MTR_LOG_NO_REDO：不记录redo，但脏页放到flush list上；
//                                  MTR_LOG_SHORT_INSERTS：插入记录操作REDO，在将记录从一个page拷贝到另外一个新建的page时用到，此时忽略写索引信息到redo log中。
//                                  （参阅函数 page_cur_insert_rec_write_log ）
//  fil_space_t* m_user_space	当前 mtr 修改的用户表空间
//  fil_space_t* m_undo_space	当前 mtr 修改的 undo 表空间
//  fil_space_t* m_sys_space	当前 mtr 修改的系统表空间
//  mtr_state_t m_state	        包含四种状态: MTR_STATE_INIT、MTR_STATE_COMMITTING、 MTR_STATE_COMMITTED
//
struct mtr_t {

  /** State variables of the mtr */
  /* mtr_t 内嵌一个结构体 Impl */
  struct Impl {

    /** memo stack for locks etc. */
    /* 记录加锁的对象和加锁的类型. */
    // m_memo 用来存储所有这个物理事务用到（访问）的页面，这些页面都是被所属的物理事务上了锁的（读锁或者写锁，某些时候会不上锁）；
    //
    // m_memo 里面保存了需要加入到 flush list 的 block ，在使用 mtr 的时候，需要自己挂载 block 到这个数据结构。
    mtr_buf_t m_memo;

    /** mini-transaction log */
    /* mtr 产生的日志 */
    //
    // m_log 用来存储这个物理事务在访问修改数据页面的过程中产生的所有日志，这个日志就是数据库中经常说到的重做（redo log）日志；
    // m_log 存储当前的 MTR 提交的 log 内容。
    mtr_buf_t m_log;

    /** true if mtr has made at least one buffer pool page dirty */
    /* 是否修改了 Buffer Pool 中的 Page ，产生了脏页. */
    bool m_made_dirty;

    /** true if inside ibuf changes */
    bool m_inside_ibuf;

    /** true if the mini-transaction modified buffer pool pages */
    /* 是否修改了 Buffer Pool 中的 Page. */
    bool m_modifications;

    /** true if mtr is forced to NO_LOG mode because redo logging is
    disabled globally. In this case, mtr increments the global counter
    at ::start and must decrement it back at ::commit. */
    bool m_marked_nolog;

    /** Shard index used for incrementing global counter at ::start. We need
    to use the same shard while decrementing counter at ::commit. */
    size_t m_shard_index;

    /** Count of how many page initial log records have been
    written to the mtr log */
    /* 该 mtr 包含多少条log. */
    ib_uint32_t m_n_log_recs;

    /** specifies which operations should be logged; default
    value MTR_LOG_ALL */
    /*log操作模式，MTR_LOG_ALL、MTR_LOG_NONE、MTR_LOG_SHORT_INSERTS*/
    //
    // m_log_mode, 表示 mtr 的日志模式，有 4 种类型:
    //  MTR_LOG_ALL             表示 LOG 所有的操作（包括写 redolog 以及加脏页到 flush list )
    //  MTR_LOG_NONE            不记录任何操作.
    //  MTR_LOG_NO_REDO         不生成 REDO log , 可是会加脏页到 flush list
    //  MTR_LOG_SHORT_INSERTS   这个也是不记录任何操作，纯粹只是使用了 MTR 的一些功能(只在 copy page 的使用).
    mtr_log_t m_log_mode;

    /** State of the transaction */
    /* mtr 状态，主要有初始化(MTR_STATE_INIT)、激活(MTR_STATE_ACTIVE)、提交中(MTR_STATE_COMMITTING)、以及提交完毕(MTR_STATE_COMMITTED). */
    mtr_state_t m_state;

    /** Flush Observer */
    FlushObserver *m_flush_observer;

#ifdef UNIV_DEBUG

    /** For checking corruption. */
    /*魔法字*/
    ulint m_magic_n;

#endif /* UNIV_DEBUG */

    /** Owning mini-transaction */
    mtr_t *m_mtr;
  };


#ifndef UNIV_HOTBACKUP

  /** mtr global logging */
  class Logging {

   public:

    /** mtr global redo logging state.
    Enable Logging  :
        [ENABLED] -> [ENABLED_RESTRICT] -> [DISABLED]
    Disable Logging :
        [DISABLED] -> [ENABLED_RESTRICT] -> [ENABLED_DBLWR] -> [ENABLED]
    */

    enum State : uint32_t {

      /* Redo Logging is enabled. Server is crash safe. */
      ENABLED,

      /* Redo logging is enabled. All non-logging mtr are finished with the
      pages flushed to disk. Double write is enabled. Some pages could be
      still getting written to disk without double-write. Not safe to crash. */
      ENABLED_DBLWR,

      /* Redo logging is enabled but there could be some mtrs still running
      in no logging mode. Redo archiving and clone are not allowed to start.
      No double-write */
      ENABLED_RESTRICT,

      /* Redo logging is disabled and all new mtrs would not generate any redo.
      Redo archiving and clone are not allowed. */
      DISABLED
    };

    /** Initialize logging state at server start up. */
    void init() {
      m_state.store(ENABLED);
      /* We use sharded counter and force sequentially consistent counting
      which is the general default for c++ atomic operation. If we try to
      optimize it further specific to current operations, we could use
      Release-Acquire ordering i.e. std::memory_order_release during counting
      and std::memory_order_acquire while checking for the count. However,
      sharding looks to be good enough for now and we should go for non default
      memory ordering only with some visible proof for improvement. */
      m_count_nologging_mtr.set_order(std::memory_order_seq_cst);
      Counter::clear(m_count_nologging_mtr);
    }

    /** Disable mtr redo logging. Server is crash unsafe without logging.
    @param[in]	thd	server connection THD
    @return mysql error code. */
    int disable(THD *thd);

    /** Enable mtr redo logging. Ensure that the server is crash safe
    before returning.
    @param[in]	thd	server connection THD
    @return mysql error code. */
    int enable(THD *thd);

    /** Mark a no-logging mtr to indicate that it would not generate redo log
    and system is crash unsafe.
    @return true iff logging is disabled and mtr is marked. */
    bool mark_mtr(size_t index) {
      /* Have initial check to avoid incrementing global counter for regular
      case when redo logging is enabled. */
      if (is_disabled()) {
        /* Increment counter to restrict state change DISABLED to ENABLED. */
        Counter::inc(m_count_nologging_mtr, index);

        /* Check if the no-logging is still disabled. At this point, if we
        find the state disabled, it is no longer possible for the state move
        back to enabled till the mtr finishes and we unmark the mtr. */
        if (is_disabled()) {
          return (true);
        }
        Counter::dec(m_count_nologging_mtr, index);
      }
      return (false);
    }

    /** unmark a no logging mtr. */
    void unmark_mtr(size_t index) {
      ut_ad(!is_enabled());
      ut_ad(Counter::total(m_count_nologging_mtr) > 0);
      Counter::dec(m_count_nologging_mtr, index);
    }

    /* @return flush loop count for faster response when logging is disabled. */
    uint32_t get_nolog_flush_loop() const { return (NOLOG_MAX_FLUSH_LOOP); }

    /** @return true iff redo logging is enabled and server is crash safe. */
    bool is_enabled() const { return (m_state.load() == ENABLED); }

    /** @return true iff redo logging is disabled and new mtrs are not going
    to generate redo log. */
    bool is_disabled() const { return (m_state.load() == DISABLED); }

    /** @return true iff we can skip data page double write. */
    bool dblwr_disabled() const {
      auto state = m_state.load();
      return (state == DISABLED || state == ENABLED_RESTRICT);
    }

    /* Force faster flush loop for quicker adaptive flush response when logging
    is disabled. When redo logging is disabled the system operates faster with
    dirty pages generated at much faster rate. */
    static constexpr uint32_t NOLOG_MAX_FLUSH_LOOP = 5;

   private:
    /** Wait till all no-logging mtrs are finished.
    @return mysql error code. */
    int wait_no_log_mtr(THD *thd);

   private:
    /** Global redo logging state. */
    std::atomic<State> m_state;

    using Shards = Counter::Shards<128>;

    /** Number of no logging mtrs currently running. */
    Shards m_count_nologging_mtr;
  };

  /** Check if redo logging is disabled globally and mark
  the global counter till mtr ends. */
  void check_nolog_and_mark();

  /** Check if the mtr has marked the global no log counter and
  unmark it. */
  void check_nolog_and_unmark();
#endif /* !UNIV_HOTBACKUP */


  // 构造函数
  mtr_t() {
    m_impl.m_state = MTR_STATE_INIT;
    m_impl.m_marked_nolog = false;
    m_impl.m_shard_index = 0;
  }

  // 析构函数
  ~mtr_t() {

// 调试
#ifdef UNIV_DEBUG
    switch (m_impl.m_state) {
      case MTR_STATE_ACTIVE:
        ut_ad(m_impl.m_memo.size() == 0);
        ut_d(remove_from_debug_list());
        break;
      case MTR_STATE_INIT:
      case MTR_STATE_COMMITTED:
        break;
      case MTR_STATE_COMMITTING:
        ut_error;
    }
#endif /* UNIV_DEBUG */

#ifndef UNIV_HOTBACKUP
    /* Safety check in case mtr is not committed. */
    if (m_impl.m_state != MTR_STATE_INIT) {
      check_nolog_and_unmark();
    }
#endif /* !UNIV_HOTBACKUP */
  }

#ifdef UNIV_DEBUG

  /** Removed the MTR from the s_my_thread_active_mtrs list. */
  void remove_from_debug_list() const;

  /** Assure that there are no slots that are latching any resources. Only
  buffer fixing a page is allowed. */
  void check_is_not_latching() const;
#endif /* UNIV_DEBUG */

  /** Start a mini-transaction.
  @param sync		true if it is a synchronous mini-transaction
  @param read_only	true if read only mini-transaction
  */
  void start(bool sync = true, bool read_only = false);

  /** @return whether this is an asynchronous mini-transaction. */
  bool is_async() const {
    return (!m_sync);
  }

  /** Request a future commit to be synchronous. */
  void set_sync() {
    m_sync = true;
  }

  /** Commit the mini-transaction. */
  void commit();

  /** Return current size of the buffer.
  @return	savepoint */
  ulint get_savepoint() const MY_ATTRIBUTE((warn_unused_result)) {
    ut_ad(is_active());
    ut_ad(m_impl.m_magic_n == MTR_MAGIC_N);

    return (m_impl.m_memo.size());
  }

  /** Release the (index tree) s-latch stored in an mtr memo after a
  savepoint.
  @param savepoint	value returned by @see set_savepoint.
  @param lock		latch to release */
  inline void release_s_latch_at_savepoint(ulint savepoint, rw_lock_t *lock);

  /** Release the block in an mtr memo after a savepoint. */
  inline void release_block_at_savepoint(ulint savepoint, buf_block_t *block);

  /** SX-latch a not yet latched block after a savepoint. */
  inline void sx_latch_at_savepoint(ulint savepoint, buf_block_t *block);

  /** X-latch a not yet latched block after a savepoint. */
  inline void x_latch_at_savepoint(ulint savepoint, buf_block_t *block);

  /** Get the logging mode.
  @return	logging mode */
  inline mtr_log_t get_log_mode() const MY_ATTRIBUTE((warn_unused_result));

  /** Change the logging mode.
  @param mode	 logging mode
  @return	old mode */
  mtr_log_t set_log_mode(mtr_log_t mode);

  /** Read 1 - 4 bytes from a file page buffered in the buffer pool.
  @param ptr	pointer from where to read
  @param type	MLOG_1BYTE, MLOG_2BYTES, MLOG_4BYTES
  @return	value read */
  inline uint32_t read_ulint(const byte *ptr, mlog_id_t type) const
      MY_ATTRIBUTE((warn_unused_result));

  /** Locks a rw-latch in S mode.
  NOTE: use mtr_s_lock().
  @param lock	rw-lock
  @param file	file name from where called
  @param line	line number in file */
  inline void s_lock(rw_lock_t *lock, const char *file, ulint line);

  /** Locks a rw-latch in X mode.
  NOTE: use mtr_x_lock().
  @param lock	rw-lock
  @param file	file name from where called
  @param line	line number in file */
  inline void x_lock(rw_lock_t *lock, const char *file, ulint line);

  /** Locks a rw-latch in X mode.
  NOTE: use mtr_sx_lock().
  @param lock	rw-lock
  @param file	file name from where called
  @param line	line number in file */
  inline void sx_lock(rw_lock_t *lock, const char *file, ulint line);

  /** Acquire a tablespace X-latch.
  NOTE: use mtr_x_lock_space().
  @param[in]	space		tablespace instance
  @param[in]	file		file name from where called
  @param[in]	line		line number in file */
  void x_lock_space(fil_space_t *space, const char *file, ulint line);

  /** Release an object in the memo stack.
  @param object	object
  @param type	object type: MTR_MEMO_S_LOCK, ... */
  void memo_release(const void *object, ulint type);

  /** Release a page latch.
  @param[in]	ptr	pointer to within a page frame
  @param[in]	type	object type: MTR_MEMO_PAGE_X_FIX, ... */
  void release_page(const void *ptr, mtr_memo_type_t type);

  /** Note that the mini-transaction has modified data. */
  void set_modified() { m_impl.m_modifications = true; }

  /** Set the state to not-modified. This will not log the
  changes.  This is only used during redo log apply, to avoid
  logging the changes. */
  void discard_modifications() { m_impl.m_modifications = false; }

  /** Get the LSN of commit().
  @return the commit LSN
  @retval 0 if the transaction only modified temporary tablespaces or logging
  is disabled globally. */
  lsn_t commit_lsn() const MY_ATTRIBUTE((warn_unused_result)) {
    ut_ad(has_committed());
    ut_ad(m_impl.m_log_mode == MTR_LOG_ALL);
    return (m_commit_lsn);
  }

  /** Note that we are inside the change buffer code. */
  void enter_ibuf() { m_impl.m_inside_ibuf = true; }

  /** Note that we have exited from the change buffer code. */
  void exit_ibuf() { m_impl.m_inside_ibuf = false; }

  /** @return true if we are inside the change buffer code */
  bool is_inside_ibuf() const MY_ATTRIBUTE((warn_unused_result)) {
    return (m_impl.m_inside_ibuf);
  }

  /*
  @return true if the mini-transaction is active */
  bool is_active() const MY_ATTRIBUTE((warn_unused_result)) {
    return (m_impl.m_state == MTR_STATE_ACTIVE);
  }

  /** Get flush observer
  @return flush observer */
  FlushObserver *get_flush_observer() const MY_ATTRIBUTE((warn_unused_result)) {
    return (m_impl.m_flush_observer);
  }

  /** Set flush observer
  @param[in]	observer	flush observer */
  void set_flush_observer(FlushObserver *observer) {
    ut_ad(observer == nullptr || m_impl.m_log_mode == MTR_LOG_NO_REDO);

    m_impl.m_flush_observer = observer;
  }

#ifdef UNIV_DEBUG
  /** Check if memo contains the given item.
  @param memo	memo stack
  @param object	object to search
  @param type	type of object
  @return	true if contains */
  static bool memo_contains(const mtr_buf_t *memo, const void *object,
                            ulint type) MY_ATTRIBUTE((warn_unused_result));

  /** Check if memo contains the given item.
  @param ptr		object to search
  @param flags		specify types of object (can be ORred) of
                          MTR_MEMO_PAGE_S_FIX ... values
  @return true if contains */
  bool memo_contains_flagged(const void *ptr, ulint flags) const
      MY_ATTRIBUTE((warn_unused_result));

  /** Check if memo contains the given page.
  @param[in]	ptr	pointer to within buffer frame
  @param[in]	flags	specify types of object with OR of
                          MTR_MEMO_PAGE_S_FIX... values
  @return	the block
  @retval	NULL	if not found */
  buf_block_t *memo_contains_page_flagged(const byte *ptr, ulint flags) const
      MY_ATTRIBUTE((warn_unused_result));

  /** Mark the given latched page as modified.
  @param[in]	ptr	pointer to within buffer frame */
  void memo_modify_page(const byte *ptr);

  /** Print info of an mtr handle. */
  void print() const;

  /** @return true if the mini-transaction has committed */
  bool has_committed() const MY_ATTRIBUTE((warn_unused_result)) {
    return (m_impl.m_state == MTR_STATE_COMMITTED);
  }

  /** @return true if the mini-transaction is committing */
  bool is_committing() const {
    return (m_impl.m_state == MTR_STATE_COMMITTING);
  }

  /** @return true if mini-transaction contains modifications. */
  bool has_modifications() const MY_ATTRIBUTE((warn_unused_result)) {
    return (m_impl.m_modifications);
  }

  /** Check if the changes done in this mtr conflicts with changes done
  in the given mtr.  Two mtrs are said to conflict with each other, if
  they modify the same buffer block.
  @param[in]   mtr2  the given mtr.
  @return true if there is conflict, false otherwise. */
  bool conflicts_with(const mtr_t *mtr2) const
      MY_ATTRIBUTE((warn_unused_result));

  /** @return the memo stack */
  const mtr_buf_t *get_memo() const MY_ATTRIBUTE((warn_unused_result)) {
    return (&m_impl.m_memo);
  }

  /** @return the memo stack */
  mtr_buf_t *get_memo() MY_ATTRIBUTE((warn_unused_result)) {
    return (&m_impl.m_memo);
  }

  /** Computes the number of bytes that would be written to the redo
  log if mtr was committed right now (excluding headers of log blocks).
  @return  number of bytes of the collected log records increased
           by 1 if MLOG_MULTI_REC_END would already be required */
  size_t get_expected_log_size() const {
    return (m_impl.m_log.size() + (m_impl.m_n_log_recs > 1 ? 1 : 0));
  }

  void wait_for_flush();
#endif /* UNIV_DEBUG */

  /** @return true if a record was added to the mini-transaction */
  bool is_dirty() const MY_ATTRIBUTE((warn_unused_result)) {
    return (m_impl.m_made_dirty);
  }

  /** Note that a record has been added to the log */
  void added_rec() { ++m_impl.m_n_log_recs; }

  /** Get the buffered redo log of this mini-transaction.
  @return	redo log */
  const mtr_buf_t *get_log() const MY_ATTRIBUTE((warn_unused_result)) {
    ut_ad(m_impl.m_magic_n == MTR_MAGIC_N);

    return (&m_impl.m_log);
  }

  /** Get the buffered redo log of this mini-transaction.
  @return	redo log */
  mtr_buf_t *get_log() MY_ATTRIBUTE((warn_unused_result)) {
    ut_ad(m_impl.m_magic_n == MTR_MAGIC_N);

    return (&m_impl.m_log);
  }

  /** Push an object to an mtr memo stack.
  @param object	object
  @param type	object type: MTR_MEMO_S_LOCK, ... */
  inline void memo_push(void *object, mtr_memo_type_t type);

#ifdef UNIV_DEBUG
  /** Iterate all MTRs created in this thread to assure they are not latching
  any resources. Violating this could lead to deadlocks under
  log_free_check(). */
  static void check_my_thread_mtrs_are_not_latching() {
    for (auto &it : s_my_thread_active_mtrs) {
      it->check_is_not_latching();
    }
  }
#endif

  /** Check if this mini-transaction is dirtying a clean page.
  @param block	block being x-fixed
  @return true if the mtr is dirtying a clean page. */
  static bool is_block_dirtied(const buf_block_t *block)
      MY_ATTRIBUTE((warn_unused_result));

  /** Matrix to check if a mode update request should be ignored. */
  static bool s_mode_update[MTR_LOG_MODE_MAX][MTR_LOG_MODE_MAX];

#ifdef UNIV_DEBUG
  /** For checking invalid mode update requests. */
  static bool s_mode_update_valid[MTR_LOG_MODE_MAX][MTR_LOG_MODE_MAX];
#endif /* UNIV_DEBUG */

#ifndef UNIV_HOTBACKUP
  /** Instance level logging information for all mtrs. */
  static Logging s_logging;
#endif /* !UNIV_HOTBACKUP */

 private:

  // m_impl 就是上面我们介绍的 Impl
  Impl m_impl;


  /** LSN at commit time */
  //
  // m_commit_lsn 表示在 commit 的时候(commit)的 lsn
  lsn_t m_commit_lsn;

  /** true if it is synchronous mini-transaction */
  bool m_sync;

#ifdef UNIV_DEBUG
  /** List of all non-committed MTR instances created in this thread. Used for
  debug purposes in the log_free_check(). */
  static thread_local ut::unordered_set<const mtr_t *> s_my_thread_active_mtrs;
#endif

  // Command 这个数据结构主要是抽象了 MTR 的具体操作。
  // 也就是说，对于 Redo log 的修改其实是在 Command 这个结构中执行的.
  class Command;
  friend class Command;
};

#ifndef UNIV_HOTBACKUP
#ifdef UNIV_DEBUG

/** Reserves space in the log buffer and writes a single MLOG_TEST.
@param[in,out]  log      redo log
@param[in]      payload  number of extra bytes within the record,
                         not greater than 1024
@return end_lsn pointing to the first byte after the written record */
lsn_t mtr_commit_mlog_test(log_t &log, size_t payload = 0);

/** Reserves space in the log buffer and writes a single MLOG_TEST.
Adjusts size of the payload in the record, in order to fill the current
block up to its boundary. If nothing else is happening in parallel,
we could expect to see afterwards:
(cur_lsn + space_left) % OS_FILE_LOG_BLOCK_SIZE == LOG_BLOCK_HDR_SIZE,
where cur_lsn = log_get_lsn(log).
@param[in,out]  log         redo log
@param[in]      space_left  extra bytes left to the boundary of block,
                            must be not greater than 496 */
void mtr_commit_mlog_test_filling_block(log_t &log, size_t space_left = 0);

#endif /* UNIV_DEBUG */
#endif /* !UNIV_HOTBACKUP */

#include "mtr0mtr.ic"

#endif /* mtr0mtr_h */
