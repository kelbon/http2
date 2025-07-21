#pragma once

#include <vector>
#include <cstdint>
#include <cassert>

namespace http2 {

// optimized unordered_set<intmax_t>, creates segments and merge them
// examples: [1, 4] + 5 => [1, 5]
// [0, 3], [5, 6] + 4 = [0, 6]
// used for storing information about already closed http2 streams for correct errors, its likely, that in
// this case there are only one segment (stream ids are 1 3 5 etc, but before add_point /2 makes them 0 1 2)
struct merged_segments {
 private:
  struct segment {
    // invariant: end >= start
    intmax_t start = 0;
    intmax_t end = 0;

    bool contains(intmax_t point) const noexcept {
      return point >= start && point <= end;
    }
  };
  // invariant: there are no overlapping segments
  std::vector<segment> segments;

  void erase_segment(segment& s) noexcept {
    assert(&s >= segments.data() && &s < segments.data() + segments.size());
    std::swap(s, segments.back());
    segments.pop_back();
  }

  struct find_result {
    segment* right_neighbor = nullptr;  // example for 1 [2, 4]
    segment* left_neighbor = nullptr;   // example for 1 [-1, 0]
    segment* containing = nullptr;      // example for 1 [0, 2] or [1, 1]
  };

  find_result search(intmax_t point) {
    find_result r;
    for (auto& s : segments) {
      if (point + 1 == s.start)
        r.right_neighbor = &s;
      else if (point - 1 == s.end)
        r.left_neighbor = &s;
      else if (s.contains(point)) {
        r.containing = &s;
        return r;
      }
    }
    return r;
  }

 public:
  void add_point(intmax_t point) {
    find_result r = search(point);
    if (r.containing) [[unlikely]]
      return;  // already added, unlikely bcs of usage - http will never add same stream id again
    if (r.left_neighbor && r.right_neighbor) {
      // merge left and right
      assert(r.left_neighbor != r.right_neighbor);
      r.left_neighbor->end = r.right_neighbor->end;
      erase_segment(*r.right_neighbor);
    } else if (r.left_neighbor) {
      r.left_neighbor->end = point;
    } else if (r.right_neighbor) {
      r.right_neighbor->start = point;
    } else {
      // add segment [P, P]
      segments.emplace_back(point, point);
    }
  }

  bool has_point(intmax_t point) const noexcept {
    for (auto& s : segments) {
      if (s.contains(point))
        return true;
    }
    return false;
  }

  size_t segments_count() const noexcept {
    return segments.size();
  }
};

}  // namespace http2
