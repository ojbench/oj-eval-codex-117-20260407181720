#ifndef SRC_HPP
#define SRC_HPP

#include <cstddef>

enum class ReplacementPolicy { kDEFAULT = 0, kFIFO, kLRU, kMRU, kLRU_K };

class PageNode {
public:
  void Init(std::size_t *times_buf, std::size_t k) {
    occupied_ = false;
    page_id_ = 0;
    insert_time_ = 0;
    earliest_time_ = 0;
    last_time_ = 0;
    times_ = times_buf;
    k_ = (k == 0 ? 1 : k);
    count_ = 0;
    ring_next_ = 0;
  }

  bool occupied_ = false;
  std::size_t page_id_ = 0;
  std::size_t insert_time_ = 0;   // time when inserted into cache (for FIFO)
  std::size_t earliest_time_ = 0;  // first access time since (re)insertion
  std::size_t last_time_ = 0;      // most recent access time

  // Access history ring buffer of size k_
  std::size_t *times_ = nullptr;
  std::size_t k_ = 1;
  std::size_t count_ = 0;      // how many valid entries (<= k_)
  std::size_t ring_next_ = 0;  // next position to overwrite (oldest) when full

  // Record an access at logical time t
  void Record(std::size_t t) {
    if (count_ == 0) {
      times_[0] = t;
      count_ = 1;
      ring_next_ = (k_ == 1) ? 0 : 1;  // when k_==1, always overwrite index 0
      earliest_time_ = t;
      last_time_ = t;
      return;
    }
    if (count_ < k_) {
      times_[count_] = t;
      ++count_;
      // ring_next_ becomes 0 only when buffer becomes full
      if (count_ == k_) {
        ring_next_ = 0;
      }
    } else {
      // overwrite oldest
      times_[ring_next_] = t;
      ring_next_ = (ring_next_ + 1) % k_;
    }
    last_time_ = t;
  }

  // Return the k-th most recent access time if available; undefined if count_<k_
  std::size_t KthMostRecent() const {
    // When buffer is full, the oldest among last k is at ring_next_
    // because ring_next_ points to the next slot to overwrite (the oldest now)
    if (count_ < k_) {
      return 0;  // treat as -inf in comparisons by caller
    }
    return times_[ring_next_];
  }
};

class ReplacementManager {
public:
  constexpr static std::size_t npos = static_cast<std::size_t>(-1);

  ReplacementManager() = delete;

  ReplacementManager(std::size_t max_size, std::size_t k, ReplacementPolicy default_policy)
      : max_size_(max_size), k_(k == 0 ? 1 : k), default_policy_(default_policy) {
    size_ = 0;
    clock_ = 0;
    // allocate storage for nodes and their time buffers
    nodes_ = new PageNode[max_size_];
    times_pool_ = new std::size_t[max_size_ * k_];
    for (std::size_t i = 0; i < max_size_; ++i) {
      nodes_[i].Init(times_pool_ + i * k_, k_);
    }
  }

  ~ReplacementManager() {
    delete[] times_pool_;
    delete[] nodes_;
  }

  void SwitchDefaultPolicy(ReplacementPolicy default_policy) { default_policy_ = default_policy; }

  void Visit(std::size_t page_id, std::size_t &evict_id, ReplacementPolicy policy = ReplacementPolicy::kDEFAULT) {
    // Advance logical clock for this access
    ++clock_;
    // Determine policy
    ReplacementPolicy use_policy = (policy == ReplacementPolicy::kDEFAULT) ? default_policy_ : policy;

    // If page is already in cache, just record access
    std::size_t idx = FindIndex(page_id);
    if (idx != npos) {
      nodes_[idx].Record(clock_);
      evict_id = npos;
      return;
    }

    // Miss: if full, evict one according to policy
    if (Full()) {
      std::size_t victim = ChooseVictim(use_policy);
      if (victim != npos) {
        evict_id = nodes_[victim].page_id_;
        nodes_[victim].occupied_ = false;
        // size_ will remain same after removing and then inserting
        // so decrement now and increment on insertion
        --size_;
      } else {
        // Should not happen if Full(), but guard anyway
        evict_id = npos;
      }
    } else {
      evict_id = npos;
    }

    // Insert the new page into the first free slot
    for (std::size_t i = 0; i < max_size_; ++i) {
      if (!nodes_[i].occupied_) {
        nodes_[i].occupied_ = true;
        nodes_[i].page_id_ = page_id;
        nodes_[i].insert_time_ = clock_;
        // reset per-page history metadata
        nodes_[i].count_ = 0;
        nodes_[i].ring_next_ = (k_ == 1 ? 0 : 0);
        nodes_[i].earliest_time_ = clock_;
        nodes_[i].last_time_ = clock_;
        nodes_[i].Record(clock_);
        ++size_;
        break;
      }
    }
  }

  bool RemovePage(std::size_t page_id) {
    std::size_t idx = FindIndex(page_id);
    if (idx == npos) {
      return false;
    }
    nodes_[idx].occupied_ = false;
    --size_;
    return true;
  }

  [[nodiscard]] std::size_t TryEvict(ReplacementPolicy policy = ReplacementPolicy::kDEFAULT) const {
    if (!Full()) {
      return npos;
    }
    ReplacementPolicy use_policy = (policy == ReplacementPolicy::kDEFAULT) ? default_policy_ : policy;
    std::size_t victim = ChooseVictimConst(use_policy);
    if (victim == npos) return npos;
    return nodes_[victim].page_id_;
  }

  [[nodiscard]] bool Empty() const { return size_ == 0; }

  [[nodiscard]] bool Full() const { return size_ >= max_size_; }

  [[nodiscard]] std::size_t Size() const { return size_; }

private:
  std::size_t FindIndex(std::size_t page_id) const {
    for (std::size_t i = 0; i < max_size_; ++i) {
      if (nodes_[i].occupied_ && nodes_[i].page_id_ == page_id) {
        return i;
      }
    }
    return npos;
  }

  std::size_t ChooseVictim(ReplacementPolicy policy) {
    // linear scan to select according to policy
    std::size_t victim = npos;
    if (policy == ReplacementPolicy::kFIFO) {
      std::size_t best_time = static_cast<std::size_t>(-1);
      for (std::size_t i = 0; i < max_size_; ++i) {
        if (!nodes_[i].occupied_) continue;
        if (nodes_[i].insert_time_ < best_time) {
          best_time = nodes_[i].insert_time_;
          victim = i;
        }
      }
    } else if (policy == ReplacementPolicy::kLRU) {
      std::size_t best_time = static_cast<std::size_t>(-1);
      for (std::size_t i = 0; i < max_size_; ++i) {
        if (!nodes_[i].occupied_) continue;
        if (nodes_[i].last_time_ < best_time) {
          best_time = nodes_[i].last_time_;
          victim = i;
        }
      }
    } else if (policy == ReplacementPolicy::kMRU) {
      std::size_t best_time = 0;
      bool init = false;
      for (std::size_t i = 0; i < max_size_; ++i) {
        if (!nodes_[i].occupied_) continue;
        if (!init || nodes_[i].last_time_ > best_time) {
          init = true;
          best_time = nodes_[i].last_time_;
          victim = i;
        }
      }
    } else { // kLRU_K
      // First, prefer pages with count < k_: pick the one with earliest earliest_time_
      std::size_t best_earliest = static_cast<std::size_t>(-1);
      bool found_fewer = false;
      for (std::size_t i = 0; i < max_size_; ++i) {
        if (!nodes_[i].occupied_) continue;
        if (nodes_[i].count_ < k_) {
          if (!found_fewer || nodes_[i].earliest_time_ < best_earliest) {
            found_fewer = true;
            best_earliest = nodes_[i].earliest_time_;
            victim = i;
          }
        }
      }
      if (!found_fewer) {
        // choose smallest k-th most recent time among all
        std::size_t best_time = static_cast<std::size_t>(-1);
        for (std::size_t i = 0; i < max_size_; ++i) {
          if (!nodes_[i].occupied_) continue;
          // count_ >= k_ by construction here
          std::size_t kth = nodes_[i].KthMostRecent();
          if (kth < best_time) {
            best_time = kth;
            victim = i;
          }
        }
      }
    }
    return victim;
  }

  std::size_t ChooseVictimConst(ReplacementPolicy policy) const {
    std::size_t victim = npos;
    if (policy == ReplacementPolicy::kFIFO) {
      std::size_t best_time = static_cast<std::size_t>(-1);
      for (std::size_t i = 0; i < max_size_; ++i) {
        if (!nodes_[i].occupied_) continue;
        if (nodes_[i].insert_time_ < best_time) {
          best_time = nodes_[i].insert_time_;
          victim = i;
        }
      }
    } else if (policy == ReplacementPolicy::kLRU) {
      std::size_t best_time = static_cast<std::size_t>(-1);
      for (std::size_t i = 0; i < max_size_; ++i) {
        if (!nodes_[i].occupied_) continue;
        if (nodes_[i].last_time_ < best_time) {
          best_time = nodes_[i].last_time_;
          victim = i;
        }
      }
    } else if (policy == ReplacementPolicy::kMRU) {
      std::size_t best_time = 0;
      bool init = false;
      for (std::size_t i = 0; i < max_size_; ++i) {
        if (!nodes_[i].occupied_) continue;
        if (!init || nodes_[i].last_time_ > best_time) {
          init = true;
          best_time = nodes_[i].last_time_;
          victim = i;
        }
      }
    } else { // kLRU_K
      std::size_t best_earliest = static_cast<std::size_t>(-1);
      bool found_fewer = false;
      for (std::size_t i = 0; i < max_size_; ++i) {
        if (!nodes_[i].occupied_) continue;
        if (nodes_[i].count_ < k_) {
          if (!found_fewer || nodes_[i].earliest_time_ < best_earliest) {
            found_fewer = true;
            best_earliest = nodes_[i].earliest_time_;
            victim = i;
          }
        }
      }
      if (!found_fewer) {
        std::size_t best_time = static_cast<std::size_t>(-1);
        for (std::size_t i = 0; i < max_size_; ++i) {
          if (!nodes_[i].occupied_) continue;
          std::size_t kth = nodes_[i].KthMostRecent();
          if (kth < best_time) {
            best_time = kth;
            victim = i;
          }
        }
      }
    }
    return victim;
  }

  // data
  std::size_t max_size_ = 0;
  std::size_t k_ = 1;
  ReplacementPolicy default_policy_ = ReplacementPolicy::kFIFO;
  std::size_t size_ = 0;
  std::size_t clock_ = 0;  // logical time increasing per Visit

  PageNode *nodes_ = nullptr;
  std::size_t *times_pool_ = nullptr; // contiguous pool of size max_size_ * k_
};

#endif

