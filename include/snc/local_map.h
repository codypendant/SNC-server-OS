// Per-shard local object map (spec section 3): open addressing with linear
// probing from ObjectID to ObjectHeader*. Touched only by the owning worker,
// so it has no locks or atomics. Uses the low bits of snc_hash64 (the slot
// uses the high bits). Deletion uses backward shift, so there are no
// tombstones in the table itself.
//
// Deviation from spec, tracked in docs/STATUS.md: growth currently rehashes
// in one step instead of incrementally across loop iterations. Callers can
// reserve() up front to keep rehashing out of measured runs.
#pragma once

#include <vector>

#include "snc/object.h"

namespace snc {

class LocalMap {
 public:
  explicit LocalMap(std::size_t expected = 1024) { reserve(expected); }

  ObjectHeader* find(const ObjectID& id) const {
    std::size_t i = index_of(id);
    while (true) {
      const Entry& e = table_[i];
      if (e.obj == nullptr) return nullptr;
      if (e.obj->id == id) return e.obj;
      i = (i + 1) & mask_;
    }
  }

  // Inserts or replaces. Returns the previous pointer, or nullptr.
  ObjectHeader* upsert(ObjectHeader* obj) {
    if ((size_ + 1) * 5 > table_.size() * 4) grow();  // load factor <= 0.8
    std::size_t i = index_of(obj->id);
    while (true) {
      Entry& e = table_[i];
      if (e.obj == nullptr) {
        e.obj = obj;
        ++size_;
        return nullptr;
      }
      if (e.obj->id == obj->id) {
        ObjectHeader* old = e.obj;
        e.obj = obj;
        return old;
      }
      i = (i + 1) & mask_;
    }
  }

  // The entry's storage, for swapping in a moved block: look it up before
  // realloc, write it after. Probing compares IDs through the stored
  // pointers, so the map must never be searched while it holds a freed block.
  // Invalidated by any insert or erase.
  ObjectHeader** slot(const ObjectID& id) {
    std::size_t i = index_of(id);
    while (table_[i].obj != nullptr) {
      if (table_[i].obj->id == id) return &table_[i].obj;
      i = (i + 1) & mask_;
    }
    return nullptr;
  }

  // Removes and returns the entry, or nullptr if absent.
  ObjectHeader* erase(const ObjectID& id) {
    std::size_t i = index_of(id);
    while (true) {
      if (table_[i].obj == nullptr) return nullptr;
      if (table_[i].obj->id == id) break;
      i = (i + 1) & mask_;
    }
    ObjectHeader* removed = table_[i].obj;
    // Backward-shift deletion keeps probe chains intact.
    std::size_t hole = i;
    std::size_t j = (i + 1) & mask_;
    while (table_[j].obj != nullptr) {
      std::size_t home = index_of(table_[j].obj->id);
      // Move j into the hole if its home is not in (hole, j] cyclically.
      bool movable = (j > hole) ? (home <= hole || home > j) : (home <= hole && home > j);
      if (movable) {
        table_[hole] = table_[j];
        hole = j;
      }
      j = (j + 1) & mask_;
    }
    table_[hole].obj = nullptr;
    --size_;
    return removed;
  }

  void reserve(std::size_t n) {
    std::size_t cap = 16;
    while (cap * 4 < n * 5) cap <<= 1;
    if (cap > table_.size()) rehash(cap);
  }

  std::size_t size() const { return size_; }
  std::size_t capacity() const { return table_.size(); }
  std::size_t memory_bytes() const { return table_.size() * sizeof(Entry); }

  template <typename F>
  void for_each(F&& fn) const {
    for (const Entry& e : table_) {
      if (e.obj) fn(e.obj);
    }
  }

 private:
  struct Entry {
    ObjectHeader* obj = nullptr;
  };

  std::size_t index_of(const ObjectID& id) const { return snc_hash64(id) & mask_; }

  void grow() { rehash(table_.size() * 2); }

  void rehash(std::size_t cap) {
    std::vector<Entry> old;
    old.swap(table_);
    table_.assign(cap, Entry{});
    mask_ = cap - 1;
    size_ = 0;
    for (const Entry& e : old) {
      if (e.obj) upsert(e.obj);
    }
  }

  std::vector<Entry> table_;
  std::size_t mask_ = 0;
  std::size_t size_ = 0;
};

}  // namespace snc
