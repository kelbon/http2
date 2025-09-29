#include <http2/utils/merged_segments.hpp>

#include <iostream>
#include <unordered_set>
#include <vector>
#include <algorithm>
#include <random>

#define error_if(...)                          \
  if ((__VA_ARGS__)) {                         \
    std::cout << "ERROR ON LINE " << __LINE__; \
    std::exit(__LINE__);                       \
  }

struct primitive_merged_segments {
  std::unordered_set<intmax_t> us;

  void add_point(intmax_t point) {
    us.insert(point);
  }

  bool has_point(intmax_t point) const noexcept {
    return us.contains(point);
  }
};

struct node {
  node* next;
  int i;

  node(int i) : i(i) {
  }
};

int main() {
  const int MIN_POINT = 1;
  const int MAX_POINT = 10000;

  http2::merged_segments ms;
  primitive_merged_segments pms;

  // tests points [1, 2, ... MAX_POINT]
  std::vector<int> points;
  for (int i = MIN_POINT; i <= MAX_POINT; ++i)
    points.push_back(i);

  std::mt19937 g(1122);
  auto dist = std::uniform_int_distribution<size_t>(0, points.size() - 1);
  std::shuffle(points.begin(), points.end(), g);

  for (auto x : points)
    error_if(ms.has_point(x));

  for (auto point : points) {
    ms.add_point(point);
    pms.add_point(point);
    error_if(!ms.has_point(point));
    auto randomp = points[dist(g)];
    error_if(ms.has_point(randomp) != pms.has_point(randomp));
  }

  for (int i = MIN_POINT; i <= MAX_POINT; ++i)
    error_if(!ms.has_point(i));

  error_if(ms.segments_count() != 1);

  return 0;
}
