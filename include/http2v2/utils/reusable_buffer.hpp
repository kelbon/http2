
#pragma once

#include "http2v2/utils/memory.hpp"

#include <memory_resource>

namespace http2v2 {

struct reusable_buffer {
private:
  byte_t *m_bytes = nullptr;
  size_t m_sz = 0;
  std::pmr::memory_resource *m_resource = nullptr;

public:
  explicit reusable_buffer(
      std::pmr::memory_resource *mr = std::pmr::new_delete_resource())
      : m_resource(mr) {
    assert(m_resource);
  }

  ~reusable_buffer() { clear(); }

  void reserve(size_t count) { (void)getExactly(count); }

  void clear() noexcept {
    if (m_bytes) {
      m_resource->deallocate(m_bytes, m_sz);
      m_bytes = nullptr;
      m_sz = 0;
    }
  }

  std::span<byte_t> getMaxBuffer() noexcept {
    if (!m_bytes) {
      return {};
    }
    return std::span(m_bytes, m_sz);
  }
  // value will be invalidated on next call
  // may return uninitialized memory
  std::span<byte_t> getExactly(size_t count) {
    if (m_sz >= count) {
      return std::span<byte_t>(m_bytes, count);
    }
    if (m_bytes) {
      clear();
    }
    m_bytes = static_cast<byte_t *>(m_resource->allocate(count));
    m_sz = count;
    return std::span<byte_t>(m_bytes, count);
  }

  std::span<byte_t> getAtleast(size_t count) {
    if (m_sz >= count) {
      return std::span<byte_t>(m_bytes, m_sz);
    }
    return getExactly(count);
  }
};

} // namespace http2v2
