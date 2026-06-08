// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// tests/unit/test_buffer_ring.cpp
//
// Host unit tests for drm::present::BufferRing -- the slot state machine, buffer
// age, and damage-union accumulation (the partial-repaint contract). Self-
// contained CHECK harness; non-zero exit on failure.

#include <drm-cxx/present/buffer_ring.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace present = drm::present;

static int g_fail = 0;
#define CHECK(x)                                                        \
  do {                                                                  \
    if (!(x)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
      ++g_fail;                                                         \
    }                                                                   \
  } while (0)

namespace {

present::Rect rect(std::int32_t x, std::int32_t y, std::uint32_t w, std::uint32_t h) {
  return present::Rect{x, y, w, h};
}

// One present cycle helper: present `slot` with the given damage.
void present(present::BufferRing& ring, std::size_t slot, std::vector<present::Rect> damage) {
  ring.present(slot, damage);
}

void test_grow_and_capacity() {
  present::BufferRing ring(2);
  CHECK(ring.max_slots() == 2);
  CHECK(ring.size() == 0);

  const auto a = ring.acquire();
  CHECK(a.has_value());
  CHECK(a->fresh);         // first slot is freshly grown
  CHECK(a->repaint.full);  // ... so it needs a full repaint
  CHECK(ring.size() == 1);
  CHECK(ring.in_flight() == 1);

  present(ring, a->slot, {});  // now scanning

  const auto b = ring.acquire();
  CHECK(b.has_value());
  CHECK(b->slot != a->slot);  // never the scanning slot
  CHECK(b->fresh);
  CHECK(ring.size() == 2);

  // At capacity with one scanning + one in-flight: no slot to hand out.
  CHECK(!ring.acquire().has_value());
}

void test_reuse_released_slot() {
  present::BufferRing ring(2);
  const auto a = ring.acquire();
  present(ring, a->slot, {});  // slot a scanning
  const auto b = ring.acquire();
  present(ring, b->slot, {});  // slot b scanning; a -> pending release
  ring.release(a->slot);       // a back to free

  const auto c = ring.acquire();
  CHECK(c.has_value());
  CHECK(c->slot == a->slot);  // reuse the released slot, not a new one
  CHECK(!c->fresh);
  CHECK(ring.size() == 2);  // did not grow

  // The scanning slot is never handed out: abandon c (frees its slot), re-acquire,
  // and confirm we get that slot back rather than the still-scanning b.
  ring.release(c->slot);
  const auto d = ring.acquire();
  CHECK(d.has_value());
  CHECK(d->slot == c->slot);
  CHECK(d->slot != b->slot);  // b still scanning -> never handed out
}

void test_age_increments() {
  present::BufferRing ring(3);
  const auto a = ring.acquire();
  present(ring, a->slot, {});
  CHECK(ring.age(a->slot) == 0);  // just presented

  const auto b = ring.acquire();
  present(ring, b->slot, {});
  CHECK(ring.age(a->slot) == 1);  // one present since a

  const auto c = ring.acquire();
  present(ring, c->slot, {});
  CHECK(ring.age(a->slot) == 2);  // two presents since a
  CHECK(ring.age(b->slot) == 1);

  // A never-presented (fresh, abandoned) slot reports age 0 = undefined.
  present::BufferRing ring2(2);
  const auto x = ring2.acquire();
  CHECK(ring2.age(x->slot) == 0);
}

void test_damage_union_double_buffer() {
  present::BufferRing ring(2);
  const auto f1 = ring.acquire();
  present(ring, f1->slot, {rect(0, 0, 10, 10)});  // frame 1 damage

  const auto f2 = ring.acquire();
  present(ring, f2->slot, {rect(20, 20, 5, 5)});  // frame 2 damage
  ring.release(f1->slot);

  // Reusing slot f1 (content = frame 1) for frame 3: it is stale by frame 2's
  // damage exactly -- age 1, union == {frame 2 rect}.
  const auto f3 = ring.acquire();
  CHECK(f3->slot == f1->slot);
  CHECK(ring.age(f3->slot) == 1);
  CHECK(!f3->repaint.full);
  CHECK(f3->repaint.region.size() == 1);
  CHECK(f3->repaint.region[0] == rect(20, 20, 5, 5));
}

void test_damage_union_triple_buffer() {
  present::BufferRing ring(3);
  const auto f1 = ring.acquire();
  present(ring, f1->slot, {rect(1, 1, 1, 1)});
  const auto f2 = ring.acquire();
  present(ring, f2->slot, {rect(2, 2, 2, 2)});
  const auto f3 = ring.acquire();
  present(ring, f3->slot, {rect(3, 3, 3, 3)});
  ring.release(f1->slot);

  // Slot f1 is stale by two frames (2 and 3); union must be both, in order.
  const auto f4 = ring.acquire();
  CHECK(f4->slot == f1->slot);
  CHECK(ring.age(f4->slot) == 2);
  CHECK(!f4->repaint.full);
  CHECK(f4->repaint.region.size() == 2);
  CHECK(f4->repaint.region[0] == rect(2, 2, 2, 2));
  CHECK(f4->repaint.region[1] == rect(3, 3, 3, 3));
}

void test_stale_beyond_history_is_full() {
  // A free slot that keeps losing the "freshest" race eventually falls out of the
  // retained damage history (base advances past its last present), and must then
  // get a full repaint rather than an under-painted partial -- the off-by-one
  // safety net for damage correctness.
  present::BufferRing ring(3);
  const auto s0 = ring.acquire();
  present(ring, s0->slot, {rect(0, 0, 1, 1)});  // seq 1
  const auto s1 = ring.acquire();
  present(ring, s1->slot, {rect(0, 0, 2, 2)});  // seq 2
  const auto s2 = ring.acquire();
  present(ring, s2->slot, {rect(0, 0, 3, 3)});  // seq 3, history {1,2,3}

  ring.release(s0->slot);  // free, last presented seq 1
  ring.release(s1->slot);  // free, last presented seq 2

  // acquire() prefers the freshest free slot, pushing s0 ever further behind.
  const auto r4 = ring.acquire();
  CHECK(r4->slot == s1->slot);                  // s1 (seq 2) is fresher than s0 (seq 1)
  present(ring, r4->slot, {rect(0, 0, 4, 4)});  // seq 4, history {2,3,4}, base 1
  ring.release(s2->slot);
  const auto r5 = ring.acquire();
  CHECK(r5->slot == s2->slot);                  // s2 (seq 3) fresher than s0 (seq 1)
  present(ring, r5->slot, {rect(0, 0, 5, 5)});  // seq 5, history {3,4,5}, base 2

  // Now s0 is the only free slot (s1 pending-release, s2 scanning); its last
  // present (seq 1) is below base 2, so its damage is gone -> full repaint.
  const auto r6 = ring.acquire();
  CHECK(r6->slot == s0->slot);
  CHECK(r6->repaint.full);
}

void test_max_damage_rects_collapses_to_full() {
  present::BufferRing ring(2, /*max_damage_rects=*/2);
  const auto a = ring.acquire();
  present(ring, a->slot, {});  // seq 1: a's own content
  const auto b = ring.acquire();
  // Damage AFTER a's present is what a must repaint on reuse; 3 rects > cap 2.
  present(ring, b->slot, {rect(0, 0, 1, 1), rect(1, 1, 1, 1), rect(2, 2, 1, 1)});  // seq 2
  ring.release(a->slot);
  const auto c = ring.acquire();
  CHECK(c->slot == a->slot);
  CHECK(c->repaint.full);  // 3-rect union exceeds the cap -> full repaint
}

}  // namespace

int main() {
  test_grow_and_capacity();
  test_reuse_released_slot();
  test_age_increments();
  test_damage_union_double_buffer();
  test_damage_union_triple_buffer();
  test_stale_beyond_history_is_full();
  test_max_damage_rects_collapses_to_full();

  if (g_fail) {
    std::fprintf(stderr, "%d check(s) failed\n", g_fail);
    return 1;
  }
  std::puts("all buffer_ring tests passed");
  return 0;
}
