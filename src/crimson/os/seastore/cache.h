// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#pragma once

#include <iostream>

#include "seastar/core/shared_future.hh"

#include "include/buffer.h"

#include "crimson/os/seastore/logging.h"
#include "crimson/os/seastore/seastore_types.h"
#include "crimson/os/seastore/transaction.h"
#include "crimson/os/seastore/segment_manager.h"
#include "crimson/common/errorator.h"
#include "crimson/os/seastore/cached_extent.h"
#include "crimson/os/seastore/root_block.h"
#include "crimson/os/seastore/segment_cleaner.h"

namespace crimson::os::seastore {

/**
 * Cache
 *
 * This component is responsible for buffer management, including
 * transaction lifecycle.
 *
 * Seastore transactions are expressed as an atomic combination of
 * 1) newly written blocks
 * 2) logical mutations to existing physical blocks
 *
 * See record_t
 *
 * As such, any transaction has 3 components:
 * 1) read_set: references to extents read during the transaction
 *       See Transaction::read_set
 * 2) write_set: references to extents to be written as:
 *    a) new physical blocks, see Transaction::fresh_block_list
 *    b) mutations to existing physical blocks,
 *       see Transaction::mutated_block_list
 * 3) retired_set: extent refs to be retired either due to 2b or
 *    due to releasing the extent generally.

 * In the case of 2b, the CachedExtent will have been copied into
 * a fresh CachedExtentRef such that the source extent ref is present
 * in the read set and the newly allocated extent is present in the
 * write_set.
 *
 * A transaction has 3 phases:
 * 1) construction: user calls Cache::get_transaction() and populates
 *    the returned transaction by calling Cache methods
 * 2) submission: user calls Cache::try_start_transaction().  If
 *    succcessful, the user may construct a record and submit the
 *    transaction to the journal.
 * 3) completion: once the transaction is durable, the user must call
 *    Cache::complete_commit() with the block offset to complete
 *    the transaction.
 *
 * Internally, in phase 1, the fields in Transaction are filled in.
 * - reads may block if the referenced extent is being written
 * - once a read obtains a particular CachedExtentRef for a paddr_t,
 *   it'll always get the same one until overwritten
 * - once a paddr_t is overwritten or written, subsequent reads of
 *   that addr will get the new ref
 *
 * In phase 2, if all extents in the read set are valid (not expired),
 * we can commit (otherwise, we fail and the user must retry).
 * - Expire all extents in the retired_set (they must all be valid)
 * - Remove all extents in the retired_set from Cache::extents
 * - Mark all extents in the write_set wait_io(), add promises to
 *   transaction
 * - Merge Transaction::write_set into Cache::extents
 *
 * After phase 2, the user will submit the record to the journal.
 * Once complete, we perform phase 3:
 * - For each CachedExtent in block_list, call
 *   CachedExtent::complete_initial_write(paddr_t) with the block's
 *   final offset (inferred from the extent's position in the block_list
 *   and extent lengths).
 * - For each block in mutation_list, call
 *   CachedExtent::delta_written(paddr_t) with the address of the start
 *   of the record
 * - Complete all promises with the final record start paddr_t
 */
class Cache {
public:
  using base_ertr = crimson::errorator<
    crimson::ct_error::input_output_error>;
  using base_iertr = trans_iertr<base_ertr>;

  Cache(SegmentManager &segment_manager);
  ~Cache();

  retired_extent_gate_t retired_extent_gate;

  /// Creates empty transaction by source
  TransactionRef create_transaction(
      Transaction::src_t src) {
    LOG_PREFIX(Cache::create_transaction);

    ++(get_by_src(stats.trans_created_by_src, src));

    auto ret = std::make_unique<Transaction>(
      get_dummy_ordering_handle(),
      false,
      src,
      last_commit,
      [this](Transaction& t) {
        return on_transaction_destruct(t);
      }
    );
    retired_extent_gate.add_token(ret->retired_gate_token);
    DEBUGT("created source={}", *ret, src);
    return ret;
  }

  /// Creates empty weak transaction by source
  TransactionRef create_weak_transaction(
      Transaction::src_t src) {
    LOG_PREFIX(Cache::create_weak_transaction);

    ++(get_by_src(stats.trans_created_by_src, src));

    auto ret = std::make_unique<Transaction>(
      get_dummy_ordering_handle(),
      true,
      src,
      last_commit,
      [this](Transaction& t) {
        return on_transaction_destruct(t);
      }
    );
    retired_extent_gate.add_token(ret->retired_gate_token);
    DEBUGT("created source={}", *ret, src);
    return ret;
  }

  /// Resets transaction preserving
  void reset_transaction_preserve_handle(Transaction &t) {
    LOG_PREFIX(Cache::reset_transaction_preserve_handle);
    if (t.did_reset()) {
      ++(get_by_src(stats.trans_created_by_src, t.get_src()));
    }
    t.reset_preserve_handle(last_commit);
    DEBUGT("reset", t);
  }

  /**
   * drop_from_cache
   *
   * Drop extent from cache.  Intended for use when
   * ref refers to a logically dead extent as during
   * replay.
   */
  void drop_from_cache(CachedExtentRef ref) {
    remove_extent(ref);
  }

  /// Declare ref retired in t
  void retire_extent(Transaction &t, CachedExtentRef ref) {
    t.add_to_retired_set(ref);
  }

  /// Declare paddr retired in t
  using retire_extent_iertr = base_iertr;
  using retire_extent_ret = base_iertr::future<>;
  retire_extent_ret retire_extent_addr(
    Transaction &t, paddr_t addr, extent_len_t length);

  /**
   * get_root
   *
   * returns ref to current root or t.root if modified in t
   */
  using get_root_iertr = base_iertr;
  using get_root_ret = get_root_iertr::future<RootBlockRef>;
  get_root_ret get_root(Transaction &t);

  /**
   * get_root_fast
   *
   * returns t.root and assume it is already present/read in t
   */
  RootBlockRef get_root_fast(Transaction &t) {
    assert(t.root);
    return t.root;
  }

  /**
   * get_extent
   *
   * returns ref to extent at offset~length of type T either from
   * - extent_set if already in cache
   * - disk
   */
  using src_ext_t = std::pair<Transaction::src_t, extent_types_t>;
  using get_extent_ertr = base_ertr;
  template <typename T>
  using get_extent_ret = get_extent_ertr::future<TCachedExtentRef<T>>;
  template <typename T>
  get_extent_ret<T> get_extent(
    paddr_t offset,                ///< [in] starting addr
    segment_off_t length,          ///< [in] length
    const src_ext_t* p_metric_key  ///< [in] cache query metric key
  ) {
    auto cached = query_cache(offset, p_metric_key);
    if (!cached) {
      auto ret = CachedExtent::make_cached_extent_ref<T>(
        alloc_cache_buf(length));
      ret->set_paddr(offset);
      ret->state = CachedExtent::extent_state_t::CLEAN;
      add_extent(ret);
      return read_extent<T>(std::move(ret));
    }

    // extent PRESENT in cache
    if (cached->get_type() == extent_types_t::RETIRED_PLACEHOLDER) {
      auto ret = CachedExtent::make_cached_extent_ref<T>(
        alloc_cache_buf(length));
      ret->set_paddr(offset);
      ret->state = CachedExtent::extent_state_t::CLEAN;
      extents.replace(*ret, *cached);

      // replace placeholder in transactions
      while (!cached->transactions.empty()) {
        auto t = cached->transactions.begin()->t;
        t->replace_placeholder(*cached, *ret);
      }

      cached->state = CachedExtent::extent_state_t::INVALID;
      return read_extent<T>(std::move(ret));
    } else {
      auto ret = TCachedExtentRef<T>(static_cast<T*>(cached.get()));
      return ret->wait_io(
      ).then([ret=std::move(ret)]() mutable -> get_extent_ret<T> {
        // ret may be invalid, caller must check
        return get_extent_ret<T>(
          get_extent_ertr::ready_future_marker{},
          std::move(ret));
      });
    }
  }

  /**
   * get_extent_if_cached
   *
   * Returns extent at offset if in cache
   */
  using get_extent_if_cached_iertr = base_iertr;
  using get_extent_if_cached_ret =
    get_extent_if_cached_iertr::future<CachedExtentRef>;
  get_extent_if_cached_ret get_extent_if_cached(
    Transaction &t,
    paddr_t offset,
    extent_types_t type) {
    CachedExtentRef ret;
    auto result = t.get_extent(offset, &ret);
    if (result != Transaction::get_extent_ret::ABSENT) {
      // including get_extent_ret::RETIRED
      return get_extent_if_cached_iertr::make_ready_future<
        CachedExtentRef>(ret);
    }

    // get_extent_ret::ABSENT from transaction
    auto metric_key = std::make_pair(t.get_src(), type);
    ret = query_cache(offset, &metric_key);
    if (!ret ||
        // retired_placeholder is not really cached yet
        ret->get_type() == extent_types_t::RETIRED_PLACEHOLDER) {
      return get_extent_if_cached_iertr::make_ready_future<
        CachedExtentRef>();
    }

    // present in cache and is not a retired_placeholder
    t.add_to_read_set(ret);
    return ret->wait_io().then([ret] {
      return get_extent_if_cached_iertr::make_ready_future<
        CachedExtentRef>(ret);
    });
  }

  /**
   * get_extent
   *
   * returns ref to extent at offset~length of type T either from
   * - t if modified by t
   * - extent_set if already in cache
   * - disk
   *
   * t *must not* have retired offset
   */
  using get_extent_iertr = base_iertr;
  template <typename T>
  get_extent_iertr::future<TCachedExtentRef<T>> get_extent(
    Transaction &t,
    paddr_t offset,
    segment_off_t length) {
    CachedExtentRef ret;
    auto result = t.get_extent(offset, &ret);
    if (result != Transaction::get_extent_ret::ABSENT) {
      assert(result != Transaction::get_extent_ret::RETIRED);
      return seastar::make_ready_future<TCachedExtentRef<T>>(
	ret->cast<T>());
    } else {
      auto metric_key = std::make_pair(t.get_src(), T::TYPE);
      return trans_intr::make_interruptible(
	get_extent<T>(offset, length, &metric_key)
      ).si_then(
	[&t, this](auto ref) {
	  if (!ref->is_valid()) {
	    LOG_PREFIX(Cache::get_extent);
	    DEBUGT("got invalid extent: {}", t, ref);
	    this->invalidate(t, *ref.get());
	    return get_extent_iertr::make_ready_future<TCachedExtentRef<T>>();
	  } else {
	    t.add_to_read_set(ref);
	    return get_extent_iertr::make_ready_future<TCachedExtentRef<T>>(
	      std::move(ref));
	  }
	});
    }
  }


  /**
   * get_extent_by_type
   *
   * Based on type, instantiate the correct concrete type
   * and read in the extent at location offset~length.
   */
  get_extent_ertr::future<CachedExtentRef> get_extent_by_type(
    extent_types_t type,             ///< [in] type tag
    paddr_t offset,                  ///< [in] starting addr
    laddr_t laddr,                   ///< [in] logical address if logical
    segment_off_t length,            ///< [in] length
    const Transaction::src_t* p_src  ///< [in] src tag for cache query metric
  );

  using get_extent_by_type_iertr = get_extent_iertr;
  using get_extent_by_type_ret = get_extent_by_type_iertr::future<
    CachedExtentRef>;
  get_extent_by_type_ret get_extent_by_type(
    Transaction &t,
    extent_types_t type,
    paddr_t offset,
    laddr_t laddr,
    segment_off_t length) {
    CachedExtentRef ret;
    auto status = t.get_extent(offset, &ret);
    if (status == Transaction::get_extent_ret::RETIRED) {
      return seastar::make_ready_future<CachedExtentRef>();
    } else if (status == Transaction::get_extent_ret::PRESENT) {
      return seastar::make_ready_future<CachedExtentRef>(ret);
    } else {
      auto src = t.get_src();
      return trans_intr::make_interruptible(
	get_extent_by_type(type, offset, laddr, length, &src)
      ).si_then([=, &t](CachedExtentRef ret) {
        if (!ret->is_valid()) {
          LOG_PREFIX(Cache::get_extent_by_type);
          DEBUGT("got invalid extent: {}", t, ret);
          invalidate(t, *ret.get());
          return get_extent_ertr::make_ready_future<CachedExtentRef>();
        } else {
          t.add_to_read_set(ret);
          return get_extent_ertr::make_ready_future<CachedExtentRef>(
            std::move(ret));
        }
      });
    }
  }

  /**
   * alloc_new_extent
   *
   * Allocates a fresh extent.  addr will be relative until commit.
   */
  template <typename T>
  TCachedExtentRef<T> alloc_new_extent(
    Transaction &t,      ///< [in, out] current transaction
    segment_off_t length ///< [in] length
  ) {
    auto ret = CachedExtent::make_cached_extent_ref<T>(
      alloc_cache_buf(length));
    t.add_fresh_extent(ret);
    ret->state = CachedExtent::extent_state_t::INITIAL_WRITE_PENDING;
    return ret;
  }

  /**
   * alloc_new_extent
   *
   * Allocates a fresh extent.  addr will be relative until commit.
   */
  CachedExtentRef alloc_new_extent_by_type(
    Transaction &t,       ///< [in, out] current transaction
    extent_types_t type,  ///< [in] type tag
    segment_off_t length  ///< [in] length
    );

  /**
   * Allocates mutable buffer from extent_set on offset~len
   *
   * TODO: Note, currently all implementations literally copy the
   * buffer.  This needn't be true, CachedExtent implementations could
   * choose to refer to the same buffer unmodified until commit and just
   * buffer the mutations in an ancillary data structure.
   *
   * @param current transaction
   * @param extent to duplicate
   * @return mutable extent
   */
  CachedExtentRef duplicate_for_write(
    Transaction &t,    ///< [in, out] current transaction
    CachedExtentRef i  ///< [in] ref to existing extent
  );

  /**
   * prepare_record
   *
   * Construct the record for Journal from transaction.
   */
  record_t prepare_record(
    Transaction &t ///< [in, out] current transaction
  );

  /**
   * complete_commit
   *
   * Must be called upon completion of write.  Releases blocks on mutating
   * extents, fills in addresses, and calls relevant callbacks on fresh
   * and mutated exents.
   */
  void complete_commit(
    Transaction &t,            ///< [in, out] current transaction
    paddr_t final_block_start, ///< [in] offset of initial block
    journal_seq_t seq,         ///< [in] journal commit seq
    SegmentCleaner *cleaner=nullptr ///< [out] optional segment stat listener
  );

  /**
   * init
   */
  void init();

  /**
   * mkfs
   *
   * Alloc initial root node and add to t.  The intention is for other
   * components to use t to adjust the resulting root ref prior to commit.
   */
  using mkfs_ertr = crimson::errorator<
    crimson::ct_error::input_output_error>;
  mkfs_ertr::future<> mkfs(Transaction &t);

  /**
   * close
   *
   * TODO: should flush dirty blocks
   */
  using close_ertr = crimson::errorator<
    crimson::ct_error::input_output_error>;
  close_ertr::future<> close();

  /**
   * replay_delta
   *
   * Intended for use in Journal::delta. For each delta, should decode delta,
   * read relevant block from disk or cache (using correct type), and call
   * CachedExtent::apply_delta marking the extent dirty.
   */
  using replay_delta_ertr = crimson::errorator<
    crimson::ct_error::input_output_error>;
  using replay_delta_ret = replay_delta_ertr::future<>;
  replay_delta_ret replay_delta(
    journal_seq_t seq,
    paddr_t record_block_base,
    const delta_info_t &delta);

  /**
   * init_cached_extents
   *
   * Calls passed lambda for each dirty cached block.  Intended for use
   * after replay to allow lba_manager (or w/e) to read in any ancestor
   * blocks.
   */
  using init_cached_extents_iertr = base_iertr;
  using init_cached_extents_ret = init_cached_extents_iertr::future<>;
  template <typename F>
  init_cached_extents_ret init_cached_extents(
    Transaction &t,
    F &&f)
  {
    std::vector<CachedExtentRef> dirty;
    for (auto &e : extents) {
      dirty.push_back(CachedExtentRef(&e));
    }
    return seastar::do_with(
      std::forward<F>(f),
      std::move(dirty),
      [&t](auto &f, auto &refs) mutable {
	return trans_intr::do_for_each(
	  refs,
	  [&t, &f](auto &e) { return f(t, e); });
      }).handle_error_interruptible(
	init_cached_extents_iertr::pass_further{},
	crimson::ct_error::assert_all{
	  "Invalid error in Cache::init_cached_extents"
	}
      );
  }

  /**
   * update_extent_from_transaction
   *
   * Updates passed extent based on t.  If extent has been retired,
   * a null result will be returned.
   */
  CachedExtentRef update_extent_from_transaction(
    Transaction &t,
    CachedExtentRef extent) {
    if (extent->get_type() == extent_types_t::ROOT) {
      if (t.root) {
	return t.root;
      } else {
	t.add_to_read_set(extent);
	return extent;
      }
    } else {
      auto result = t.get_extent(extent->get_paddr(), &extent);
      if (result == Transaction::get_extent_ret::RETIRED) {
	return CachedExtentRef();
      } else {
	if (result == Transaction::get_extent_ret::ABSENT) {
	  t.add_to_read_set(extent);
	}
	return extent;
      }
    }
  }

  /**
   * print
   *
   * Dump summary of contents (TODO)
   */
  std::ostream &print(
    std::ostream &out) const {
    return out;
  }

  /// returns extents with get_dirty_from() < seq
  using get_next_dirty_extents_ertr = crimson::errorator<>;
  using get_next_dirty_extents_ret = get_next_dirty_extents_ertr::future<
    std::vector<CachedExtentRef>>;
  get_next_dirty_extents_ret get_next_dirty_extents(
    journal_seq_t seq,
    size_t max_bytes);

  /// returns std::nullopt if no dirty extents or get_dirty_from() for oldest
  std::optional<journal_seq_t> get_oldest_dirty_from() const {
    if (dirty.empty()) {
      return std::nullopt;
    } else {
      auto oldest = dirty.begin()->get_dirty_from();
      if (oldest == journal_seq_t()) {
	return std::nullopt;
      } else {
	return oldest;
      }
    }
  }

  /// Dump live extents
  void dump_contents();

private:
  SegmentManager &segment_manager; ///< ref to segment_manager
  RootBlockRef root;               ///< ref to current root
  ExtentIndex extents;             ///< set of live extents

  journal_seq_t last_commit = JOURNAL_SEQ_MIN;

  /**
   * dirty
   *
   * holds refs to dirty extents.  Ordered by CachedExtent::get_dirty_from().
   */
  CachedExtent::list dirty;

  struct query_counters_t {
    uint64_t access = 0;
    uint64_t hit = 0;
  };

  /**
   * effort_t
   *
   * Count the number of extents involved in the effort and the total bytes of
   * them.
   *
   * Each effort_t represents the effort of a set of extents involved in the
   * transaction, classified by read, mutate, retire and allocate behaviors,
   * see trans_efforts_t.
   */
  struct effort_t {
    uint64_t extents = 0;
    uint64_t bytes = 0;

    void increment(uint64_t extent_len) {
      ++extents;
      bytes += extent_len;
    }
  };

  struct trans_efforts_t {
    effort_t read;
    effort_t mutate;
    uint64_t mutate_delta_bytes = 0;
    effort_t retire;
    effort_t fresh;
  };

  template <typename CounterT>
  using counter_by_extent_t = std::array<CounterT, EXTENT_TYPES_MAX>;

  struct trans_byextent_efforts_t {
    counter_by_extent_t<effort_t> read_by_ext;
    counter_by_extent_t<effort_t> mutate_by_ext;
    counter_by_extent_t<uint64_t> delta_bytes_by_ext;
    counter_by_extent_t<effort_t> retire_by_ext;
    counter_by_extent_t<effort_t> fresh_by_ext;
  };

  template <typename CounterT>
  using counter_by_src_t = std::array<CounterT, Transaction::SRC_MAX>;

  struct {
    counter_by_src_t<uint64_t> trans_created_by_src;
    counter_by_src_t<uint64_t> trans_committed_by_src;
    counter_by_src_t<trans_byextent_efforts_t>      committed_efforts_by_src;
    counter_by_src_t<counter_by_extent_t<uint64_t>> trans_invalidated;
    counter_by_src_t<trans_efforts_t>  invalidated_efforts_by_src;
    counter_by_src_t<query_counters_t> cache_query_by_src;
    uint64_t read_transactions_successful;
    effort_t read_effort_successful;
    uint64_t dirty_bytes;
  } stats;

  template <typename CounterT>
  CounterT& get_by_src(
      counter_by_src_t<CounterT>& counters_by_src,
      Transaction::src_t src) {
    assert(static_cast<std::size_t>(src) < counters_by_src.size());
    return counters_by_src[static_cast<std::size_t>(src)];
  }

  template <typename CounterT>
  CounterT& get_by_ext(
      counter_by_extent_t<CounterT>& counters_by_ext,
      extent_types_t ext) {
    auto index = static_cast<uint8_t>(ext);
    assert(index < EXTENT_TYPES_MAX);
    return counters_by_ext[index];
  }

  seastar::metrics::metric_group metrics;
  void register_metrics();

  /// alloc buffer for cached extent
  bufferptr alloc_cache_buf(size_t size) {
    // TODO: memory pooling etc
    auto bp = ceph::bufferptr(
      buffer::create_page_aligned(size));
    bp.zero();
    return bp;
  }

  /// Add extent to extents handling dirty and refcounting
  void add_extent(CachedExtentRef ref);

  /// Mark exising extent ref dirty -- mainly for replay
  void mark_dirty(CachedExtentRef ref);

  /// Add dirty extent to dirty list
  void add_to_dirty(CachedExtentRef ref);

  /// Remove from dirty list
  void remove_from_dirty(CachedExtentRef ref);

  /// Remove extent from extents handling dirty and refcounting
  void remove_extent(CachedExtentRef ref);

  /// Retire extent, move reference to retired_extent_gate
  void retire_extent(CachedExtentRef ref);

  /// Replace prev with next
  void replace_extent(CachedExtentRef next, CachedExtentRef prev);

  /// Invalidate extent and mark affected transactions
  void invalidate(CachedExtent &extent);

  /// Mark a valid transaction as conflicted
  void invalidate(Transaction& t, CachedExtent& conflicting_extent);

  /// Introspect transaction when it is being destructed
  void on_transaction_destruct(Transaction& t);

  template <typename T>
  get_extent_ret<T> read_extent(
    TCachedExtentRef<T>&& extent
  ) {
    extent->set_io_wait();
    return segment_manager.read(
      extent->get_paddr(),
      extent->get_length(),
      extent->get_bptr()
    ).safe_then(
      [extent=std::move(extent)]() mutable {
        /* TODO: crc should be checked against LBA manager */
        extent->last_committed_crc = extent->get_crc32c();

        extent->on_clean_read();
        extent->complete_io();
        return get_extent_ertr::make_ready_future<TCachedExtentRef<T>>(
          std::move(extent));
      },
      get_extent_ertr::pass_further{},
      crimson::ct_error::assert_all{
        "Cache::get_extent: invalid error"
      }
    );
  }

  // Extents in cache may contain placeholders
  CachedExtentRef query_cache(
      paddr_t offset,
      const src_ext_t* p_metric_key) {
    query_counters_t* p_counters = nullptr;
    if (p_metric_key) {
      p_counters = &get_by_src(stats.cache_query_by_src, p_metric_key->first);
      ++p_counters->access;
    }
    if (auto iter = extents.find_offset(offset);
        iter != extents.end()) {
      if (p_metric_key &&
          // retired_placeholder is not really cached yet
          iter->get_type() != extent_types_t::RETIRED_PLACEHOLDER) {
        ++p_counters->hit;
      }
      return CachedExtentRef(&*iter);
    } else {
      return CachedExtentRef();
    }
  }

};
using CacheRef = std::unique_ptr<Cache>;

}