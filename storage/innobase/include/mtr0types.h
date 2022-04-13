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

/** @file include/mtr0types.h
 Mini-transaction buffer global types

 Created 11/26/1995 Heikki Tuuri
 *******************************************************/

#ifndef mtr0types_h
#define mtr0types_h

#include "sync0rw.h"

struct mtr_t;

/** Logging modes for a mini-transaction */
enum mtr_log_t {
  /** Default mode: log all operations modifying disk-based data */
  MTR_LOG_ALL = 0,

  /** Log no operations and dirty pages are not added to the flush list */
  MTR_LOG_NONE = 1,

  /** Don't generate REDO log but add dirty pages to flush list */
  MTR_LOG_NO_REDO = 2,

  /** Inserts are logged in a shorter form */
  MTR_LOG_SHORT_INSERTS = 3,

  /** Last element */
  MTR_LOG_MODE_MAX = 4
};

/** @name Log item types
The log items are declared 'byte' so that the compiler can warn if val
and type parameters are switched in a call to mlog_write_ulint. NOTE!
For 1 - 8 bytes, the flag value must give the length also! @{ */


// 常用的 redo log 类型有：
//
// mlog_1byte、mlog_2bytes、mlog_4bytes、mlog_8bytes：这四个类型，表示要在某个位置，写入一个（两个、四个、八个）字节的内容；
// mlog_write_string：这种类型的日志，其实和 mlog_ibyte 是类似的，只是 mlog_ibyte 是要写一个固定长度的数据，而 mlog_write_string 是要写一段变长的数据。
// mlog_undo_insert：可以简单理解为写 undo 时候产生的 redo
// mlog_init_file_page：这个类型的日志比较简单，只有前面的基本头信息，没有 data 部分；
// mlog_comp_page_create：这个类型只需要存一个类型及要创建的页面的位置即可；
// mlog_multi_rec_end：这个类型的记录是非常特殊的，它只起一个标记的作用，其存储的内容只有占一个字节的类型值。
// mlog_comp_rec_clust_delete_mark：这个类型的日志是表示，需要将聚集索引中的某个记录打上删除标志；
// mlog_comp_rec_update_in_place：这个类型的日志记录更新后的记录信息，包括所有被更新的列的信息。
// mlog_comp_page_reorganize：这个类型的日志表示的是要重组指定的页面，其记录的内容也很简单，只需要存储要重组哪一个页面即可；
//
//
// MLOG_1BYTE、MLOG_2BYTES、MLOG_4BYTES、MLOG_8BYTES:
//
//  这四个类型，表示要在某个位置，写入一个（两个、四个、八个）字节的内容；
//      MLOG_1BYTE： type 字段对应的十进制为 1 ，表示在页面的某个偏移量处写入一个字节
//      MLOG_2BYTES：type 字段对应的十进制为 2 ，表示在页面的某个偏移量处写入两个字节
//      MLOG_4BYTES：type 字段对应的十进制为 4 ，表示在页面的某个偏移量处写入四个字节
//      MLOG_8BYTES：type 字段对应的十进制为 8 ，表示在页面的某个偏移量处写入八个字节
//
//  在页面上修改 N 个字节，可以看做物理 log 。
//  各种页链表指针修改以及文件头、段页内容的修改都是以这几种方式记录日志。
//
//
// MLOG_MULTI_REC_END
//
//  这个类型标识一个 mtr 产生多条 redo 记录已经结束，当数据恢复时候，分析 mtr 时候，只有分析到该类型时候，前面的 redo 记录才会去做 REDO 操作。
//
//
// MLOG_SINGLE_REC_FLAG
//  用来标识一个 mtr 只产生一条 redo 记录。
//  当数据恢复时候，发现某一个 record 有 MLOG_SINGLE_REC_FLAG 标记，则直接当作一个完整的 mtr 来重做。
//
//  如果发现某一个 record 无 MLOG_SINGLE_REC_FLAG 标记说明其是 multi-record mtr 的起点，一直寻找其终点（MLOG_MULTI_REC_END）；
//  如果在发现 MLOG_MULTI_REC_END 前得到了具有 MLOG_SINGLE_REC_FLAG 的 record，说明 log corrupt，但目前的处理方式是，将这段不完整的
//  multi-record mtr 日志丢弃（不加入到 hash table），继续解析。但同时会提示用户运行 CHECK TABLE（ER_IB_MSG_698）。
//
//
// MLOG_COMP_REC_INSERT, MLOG_REC_DELETE
//
//  表示插入一条使用 compact 行格式记录时的 redo 日志类型.
//
//  MLOG_COMP_REC_INSERT 记录流程
//
//        |--> page_cur_insert_rec_write_log
//        |    |--> mlog_open_and_write_index
//        |    |    |--> //初始化日志记录
//        |    |    |--> mlog_write_initial_log_record_fast
//        |    |    |    |--> //mini-transaction相关的函数，用来将redo条目写入到redo log buffer
//        |    |    |    |--> //写入type,space,page_no
//        |    |    |    |--> mlog_write_initial_log_record_low
//        |    |    |--> //写入字段个数filed no，2个字节
//        |    |    |--> mach_write_to_2(log_ptr, n);
//        |    |    |--> //写入行记录上决定唯一性的列的个数 u_uniq，2个字节
//        |    |    |--> mach_write_to_2(log_ptr, dict_index_get_n_unique_in_tree(index))
//        |    |    |--> /*loop*/
//        |    |    |--> //写入每个字段的长度 filed_length
//        |    |    |--> mach_write_to_2(log_ptr, len);
//        |    |    |--> /*end loop*/
//        |    |--> //写入记录在page上的偏移量 current rec off，占两个字节
//        |    |--> mach_write_to_2(log_ptr, page_offset(cursor_rec));
//        |    |--> //mismatch len
//        |    |--> mach_write_compressed(log_ptr, 2 * (rec_size - i));
//        |    |--> //将插入的记录拷贝到redo文件 body，同时关闭mlog
//        |    |--> memcpy
//        |    |--> mlog_close
//
//  MLOG_COMP_REC_INSERT 回放流程
//        |--> //解析出索引信息。根据主键，在内存创建dict_table
//        |--> mlog_parse_index
//        |--> page_cur_parse_insert_rec(FALSE, ptr, end_ptr, block, index, mtr)
//        |    |--> //拿到前一条记录在page内部的偏移量
//        |    |--> offset = mach_read_from_2(ptr); ptr += 2;
//        |    |--> //拿到page内前一条记录
//        |    |--> cursor_rec = page + offset;
//        |    |--> //将redo的相关信息拷贝到buf中
//        |    |--> ut_memcpy(buf, rec_get_start(cursor_rec, offsets), mismatch_index);
//        |    |--> ut_memcpy(buf + mismatch_index, ptr, end_seg_len);
//        |    |    |--> /** Positions the cursor on the given record. */
//        |    |    |--> page_cur_position(cursor_rec, block, &cursor)
//        |    |    |--> page_cur_rec_insert
//        |    |    |    |--> //真正insert的逻辑，current_rec为新记录的指针，rec为新插入字段的内容
//        |    |    |    |--> page_cur_insert_rec_low(current_rec, index, rec, offsets, mtr)
//        |    |    |    |    |--> /* 1. Get the size of the physical record in the page */
//        |    |    |    |    |--> //拿到记录的大小
//        |    |    |    |    |--> rec_size = rec_offs_size(offsets);
//        |    |    |    |    |--> /* 2. Try to find suitable space from page memory management */
//        |    |    |    |    |--> //首先尝试从自由空间链表中获得合适的存储位置（空间足够），如果没有满足的，就会在未分配空间中申请。
//        |    |    |    |    |--> free_rec = page_header_get_ptr(page, PAGE_FREE);
//        |    |    |    |    |--> /* 3. Create the record */
//        |    |    |    |    |--> //构造页面内的记录
//        |    |    |    |    |--> insert_rec = rec_copy(insert_buf, rec, offsets);
//        |    |    |    |    |--> /* 4. Insert the record in the linked list of records */
//        |    |    |    |    |--> //将记录插入到slot链表，维护记录前后指针，维护页面PAGE_N_RECS
//        |    |    |    |    |--> rec_t *next_rec = page_rec_get_next(current_rec);
//        |    |    |    |    |--> page_rec_set_next(insert_rec, next_rec);
//        |    |    |    |    |--> page_rec_set_next(current_rec, insert_rec);
//        |    |    |    |    |--> /* 5. Set the n_owned field in the inserted record to zero */
//        |    |    |    |    |--> //初始化n_owned(slot支链的高度，只有链尾有效，其他为0).维护heap_no(该记录在heap内编号，不会再改变)
//        |    |    |    |    |--> rec_set_n_owned_new
//        |    |    |    |    |--> rec_set_heap_no_new
//        |    |    |    |    |--> /* 6. Update the last insertion info in page header */
//        |    |    |    |    |--> //维护PAGE_LAST_INSERT
//        |    |    |    |    |--> last_insert = page_header_get_ptr(page, PAGE_LAST_INSERT);
//        |    |    |    |    |--> /* 7. It remains to update the owner record. */
//        |    |    |    |    |--> //维护slot链尾的n_owned
//        |    |    |    |    |--> rec_t *owner_rec = page_rec_find_owner_rec(insert_rec);
//        |    |    |    |    |--> /* 8. maintain slot */
//        |    |    |    |    |--> //slot高度为4-8，超过8则split
//        |    |    |    |    |--> page_dir_split_slot
//        |    |    |    |    |--> /* 9. Write log record of the insert */
//        |    |    |    |    |--> //如果mtr!=NULL,则记录redo
//        |    |    |    |    |--> page_cur_insert_rec_write_log
//
// MLOG_REC_UPDATE_IN_PLACE,MLOG_COMP_REC_UPDATE_IN_PLACE
//
//  记录了对 Page 中一个 Record 的修改，格式如下：
//
//    （Page ID，Record Offset，(Filed 1, Value 1) ... (Filed i, Value i) ... )
//
//  其中，PageID 指定要操作的 Page 页，Record Offset 记录了 Record 在 Page 内的偏移位置，
//  后面的 Fields 数组，记录了需要修改的 Field 以及修改后的 Value 。
//
//
// MLOG_INIT_FILE_PAGE2、MLOG_INIT_FILE_PAGE
//
//  申请空间，在分配一个 buffer pool 的 page 空间时候, 就初始化这个 page 的 space_id 和 page_no .
//  当数据恢复时候，该类型的日志作用是从 buffer pool 分配一个 page , 赋值对应的 space_id,page_no 为对应的值.
//
//
// MLOG_COMP_PAGE_CREATE,MLOG_PAGE_CREATE
//
//  初始化page，新建一个compact索引数据页，初始化page的infimum和supremum等
//  当数据恢复时候,该类型的日志作用是初始化page的infimum、supremum、N_HEAP等.
//
//
// MLOG_UNDO_INSERT
//
//  在redo中，写入一条undo相关redo记录
//
// 字段解释:
//   undo rec len: 表示一条 undo 日志记录的长度
//   生成 undo 日志记录的语句都会产生该类型的 redo log.
//   当数据恢复时候,该类型的日志作用是 INSERT 一条 undo 记录.
//

//
//
//
//
//
//
//
//
//
//
//
//
//
//
//
//
//
//
//
enum mlog_id_t {
  /** if the mtr contains only one log record for one page,
  i.e., write_initial_log_record has been called only once,
  this flag is ORed to the type of that first log record */
  MLOG_SINGLE_REC_FLAG = 128,

  /** one byte is written */
  MLOG_1BYTE = 1,

  /** 2 bytes ... */
  MLOG_2BYTES = 2,

  /** 4 bytes ... */
  MLOG_4BYTES = 4,

  /** 8 bytes ... */
  MLOG_8BYTES = 8,

  /** Record insert */
  MLOG_REC_INSERT = 9,

  /** Mark clustered index record deleted */
  MLOG_REC_CLUST_DELETE_MARK = 10,

  /** Mark secondary index record deleted */
  MLOG_REC_SEC_DELETE_MARK = 11,

  /** update of a record, preserves record field sizes */
  MLOG_REC_UPDATE_IN_PLACE = 13,

  /*!< Delete a record from a page */
  MLOG_REC_DELETE = 14,

  /** Delete record list end on index page */
  MLOG_LIST_END_DELETE = 15,

  /** Delete record list start on index page */
  MLOG_LIST_START_DELETE = 16,

  /** Copy record list end to a new created index page */
  MLOG_LIST_END_COPY_CREATED = 17,

  /** Reorganize an index page in ROW_FORMAT=REDUNDANT */
  MLOG_PAGE_REORGANIZE = 18,

  /** Create an index page */
  MLOG_PAGE_CREATE = 19,

  /** Insert entry in an undo log */
  MLOG_UNDO_INSERT = 20,

  /** erase an undo log page end */
  MLOG_UNDO_ERASE_END = 21,

  /** initialize a page in an undo log */
  MLOG_UNDO_INIT = 22,

  /** reuse an insert undo log header */
  MLOG_UNDO_HDR_REUSE = 24,

  /** create an undo log header */
  MLOG_UNDO_HDR_CREATE = 25,

  /** mark an index record as the predefined minimum record */
  MLOG_REC_MIN_MARK = 26,

  /** initialize an ibuf bitmap page */
  MLOG_IBUF_BITMAP_INIT = 27,

#ifdef UNIV_LOG_LSN_DEBUG
  /** Current LSN */
  MLOG_LSN = 28,
#endif /* UNIV_LOG_LSN_DEBUG */

  /** this means that a file page is taken into use and the prior
  contents of the page should be ignored: in recovery we must not
  trust the lsn values stored to the file page.
  Note: it's deprecated because it causes crash recovery problem
  in bulk create index, and actually we don't need to reset page
  lsn in recv_recover_page_func() now. */
  MLOG_INIT_FILE_PAGE = 29,

  /** write a string to a page */
  MLOG_WRITE_STRING = 30,

  /** If a single mtr writes several log records, this log
  record ends the sequence of these records */
  MLOG_MULTI_REC_END = 31,

  /** dummy log record used to pad a log block full */
  MLOG_DUMMY_RECORD = 32,

  /** log record about creating an .ibd file, with format */
  MLOG_FILE_CREATE = 33,

  /** rename a tablespace file that starts with (space_id,page_no) */
  MLOG_FILE_RENAME = 34,

  /** delete a tablespace file that starts with (space_id,page_no) */
  MLOG_FILE_DELETE = 35,

  /** mark a compact index record as the predefined minimum record */
  MLOG_COMP_REC_MIN_MARK = 36,

  /** create a compact index page */
  MLOG_COMP_PAGE_CREATE = 37,

  /** compact record insert */
  MLOG_COMP_REC_INSERT = 38,

  /** mark compact clustered index record deleted */
  MLOG_COMP_REC_CLUST_DELETE_MARK = 39,

  /** mark compact secondary index record deleted; this log
  record type is redundant, as MLOG_REC_SEC_DELETE_MARK is
  independent of the record format. */
  MLOG_COMP_REC_SEC_DELETE_MARK = 40,

  /** update of a compact record, preserves record field sizes */
  MLOG_COMP_REC_UPDATE_IN_PLACE = 41,

  /** delete a compact record from a page */
  MLOG_COMP_REC_DELETE = 42,

  /** delete compact record list end on index page */
  MLOG_COMP_LIST_END_DELETE = 43,

  /*** delete compact record list start on index page */
  MLOG_COMP_LIST_START_DELETE = 44,

  /** copy compact record list end to a new created index page */
  MLOG_COMP_LIST_END_COPY_CREATED = 45,

  /** reorganize an index page */
  MLOG_COMP_PAGE_REORGANIZE = 46,

  /** write the node pointer of a record on a compressed
  non-leaf B-tree page */
  MLOG_ZIP_WRITE_NODE_PTR = 48,

  /** write the BLOB pointer of an externally stored column
  on a compressed page */
  MLOG_ZIP_WRITE_BLOB_PTR = 49,

  /** write to compressed page header */
  MLOG_ZIP_WRITE_HEADER = 50,

  /** compress an index page */
  MLOG_ZIP_PAGE_COMPRESS = 51,

  /** compress an index page without logging it's image */
  MLOG_ZIP_PAGE_COMPRESS_NO_DATA = 52,

  /** reorganize a compressed page */
  MLOG_ZIP_PAGE_REORGANIZE = 53,

  /** Create a R-Tree index page */
  MLOG_PAGE_CREATE_RTREE = 57,

  /** create a R-tree compact page */
  MLOG_COMP_PAGE_CREATE_RTREE = 58,

  /** this means that a file page is taken into use.
  We use it to replace MLOG_INIT_FILE_PAGE. */
  MLOG_INIT_FILE_PAGE2 = 59,

  /** Table is being truncated. (Marked only for file-per-table) */
  /* MLOG_TRUNCATE = 60,  Disabled for WL6378 */

  /** notify that an index tree is being loaded without writing
  redo log about individual pages */
  MLOG_INDEX_LOAD = 61,

  /** log for some persistent dynamic metadata change */
  MLOG_TABLE_DYNAMIC_META = 62,

  /** create a SDI index page */
  MLOG_PAGE_CREATE_SDI = 63,

  /** create a SDI compact page */
  MLOG_COMP_PAGE_CREATE_SDI = 64,

  /** Extend the space */
  MLOG_FILE_EXTEND = 65,

  /** Used in tests of redo log. It must never be used outside unit tests. */
  MLOG_TEST = 66,

  /** biggest value (used in assertions) */
  MLOG_BIGGEST_TYPE = MLOG_TEST
};

/** Types for the mlock objects to store in the mtr memo;
    NOTE that the first 3 values must be RW_S_LATCH, RW_X_LATCH, RW_NO_LATCH
*/
//
//
//          变量名	            描述
//   MTR_MEMO_PAGE_S_FIX	用于 PAGE 上的 S 锁
//   MTR_MEMO_PAGE_X_FIX	用于 PAGE 上的 X 锁
//   MTR_MEMO_PAGE_SX_FIX	用于 PAGE 上的 SX 锁，以上锁通过 mtr_memo_push 保存到 mtr 中
//   MTR_MEMO_BUF_FIX	    PAGE 上未加读写锁，仅做 buf fix
//   MTR_MEMO_S_LOCK	    S  锁，通常用于索引锁
//   MTR_MEMO_X_LOCK	    X  锁，通常用于索引锁
//   MTR_MEMO_SX_LOCK	    SX 锁，通常用于索引锁
//                          以上 3 个锁，通过 mtr_s/x/sx_lock 加锁，通过 mtr_memo_release 释放锁
//
enum mtr_memo_type_t {
  MTR_MEMO_PAGE_S_FIX = RW_S_LATCH,

  MTR_MEMO_PAGE_X_FIX = RW_X_LATCH,

  MTR_MEMO_PAGE_SX_FIX = RW_SX_LATCH,

  MTR_MEMO_BUF_FIX = RW_NO_LATCH,

#ifdef UNIV_DEBUG
  MTR_MEMO_MODIFY = 32,
#endif /* UNIV_DEBUG */

  MTR_MEMO_S_LOCK = 64,

  MTR_MEMO_X_LOCK = 128,

  MTR_MEMO_SX_LOCK = 256
};

#ifdef UNIV_DEBUG
#define MTR_MAGIC_N 54551
#endif /* UNIV_DEBUG */


// 表示当前 MTR 的状态，主要有 3 个状态，分别是激活、提交中、以及提交完毕.
enum mtr_state_t {
  MTR_STATE_INIT = 0,
  MTR_STATE_ACTIVE = 12231,
  MTR_STATE_COMMITTING = 56456,
  MTR_STATE_COMMITTED = 34676
};

#endif /* mtr0types_h */
