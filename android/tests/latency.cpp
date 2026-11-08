#include "../src/feed/latency.hpp"
#include <cassert>
#include <cstdio>

int main() {
    recam::ByteTimeline t;
    t.append(10, 100);
    t.append(7, 200);
    assert(t.at(0) == 100 && t.at(9) == 100);
    assert(t.at(10) == 200 && t.at(16) == 200 && t.at(17) == 0);
    t.discard(8); // a NAL may span reads and survive buffer compaction
    assert(t.consumed == 8 && t.at(t.consumed + 1) == 100);
    t.discard(2);
    assert(t.at(t.consumed) == 200);
    t.discard(7);
    assert(t.received == t.consumed);
    t.append(3, 300);
    assert(t.at(17) == 300 && t.at(19) == 300);
    puts("receive timeline checks passed");
}
