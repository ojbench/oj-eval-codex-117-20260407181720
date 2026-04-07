#include <cassert>
#include <iostream>
#include "src.hpp"

int main() {
  std::size_t evict_id = 114514;
  ReplacementManager replacer(3, 2, ReplacementPolicy::kLRU_K);
  replacer.Visit(1, evict_id);
  assert(evict_id == ReplacementManager::npos);
  replacer.Visit(2, evict_id);
  assert(evict_id == ReplacementManager::npos);
  replacer.Visit(3, evict_id);
  assert(evict_id == ReplacementManager::npos);
  replacer.Visit(1, evict_id);
  assert(evict_id == ReplacementManager::npos);
  replacer.Visit(3, evict_id);
  assert(evict_id == ReplacementManager::npos);

  assert(replacer.TryEvict(ReplacementPolicy::kFIFO) == 1);
  assert(replacer.TryEvict(ReplacementPolicy::kLRU) == 2);
  assert(replacer.TryEvict(ReplacementPolicy::kMRU) == 3);
  assert(replacer.TryEvict(ReplacementPolicy::kLRU_K) == 2);

  replacer.Visit(4, evict_id, ReplacementPolicy::kFIFO);
  assert(evict_id == 1);

  // current: [2 3 4]
  replacer.Visit(1, evict_id, ReplacementPolicy::kLRU_K);
  assert(evict_id == 2);

  // current: [1 3 4]
  assert(replacer.TryEvict() == 4);
  replacer.Visit(4, evict_id, ReplacementPolicy::kLRU_K);
  assert(evict_id == ReplacementManager::npos);
  assert(replacer.TryEvict() == 1);

  // current: [1 3 4]
  replacer.Visit(2, evict_id, ReplacementPolicy::kLRU);
  assert(evict_id == 3);

  // current: [1 2 4]
  assert(replacer.Size() == 3);
  assert(replacer.Full());
  assert(!replacer.Empty());
  assert(replacer.RemovePage(2));
  assert(!replacer.RemovePage(2));
  assert(!replacer.RemovePage(3));

  // current: [1 4]
  assert(replacer.Size() == 2);
  assert(!replacer.Full());
  assert(!replacer.Empty());
  assert(replacer.RemovePage(1));
  assert(replacer.RemovePage(4));

  // []
  assert(replacer.Size() == 0);
  assert(!replacer.Full());
  assert(replacer.Empty());
  std::cout << "Local tests OK" << std::endl;
  return 0;
}
