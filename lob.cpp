// =============================================================================
//  lob.cpp : a limit order book + matching engine, written twice.
//
//    MapBook  : the textbook version   (std::map + std::list + unordered_map)
//    FastBook : the low-latency version (flat array + intrusive lists + pool)
//
//  Both follow exactly the same trading rules. The benchmark at the bottom
//  replays one stream of orders through both, proves they produce identical
//  trades, and reports how fast each one was.
//
//  Build :  g++ -O2 -std=c++17 lob.cpp -o lob
//  Run   :  ./lob             demo + benchmark on 2,000,000 messages
//           ./lob 5000000     choose the message count yourself
//
//  Reading order: sections 1 -> 2 -> 3, then the demo in section 4, then 5.
// =============================================================================

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <list>
#include <map>
#include <random>
#include <unordered_map>
#include <vector>

// =============================================================================
// 1. VOCABULARY
// =============================================================================
//
// Prices are integers counted in TICKS (the smallest allowed price step), never
// doubles. If the tick is 0.05 rupees, a price of 101.35 is stored as 2027.
// Two reasons: integer compares are exact (0.1 + 0.2 != 0.3 in floating point,
// which is fatal when "same price" decides who trades first), and an integer
// price can be used directly as an array index, which FastBook relies on.

using Price   = int32_t;
using Qty     = uint32_t;
using OrderId = uint32_t;   // assumed unique; a real exchange assigns these itself

enum class Side : uint8_t { Buy, Sell };

constexpr Price NO_BID = -1;          // "there are no buy orders in the book"
constexpr Price NO_ASK = INT32_MAX;   // "there are no sell orders in the book"

// Every time two orders trade, the book calls on_trade(). A real system would
// publish this to the market-data feed and to both traders. We only count, and
// fold every trade into a running checksum so that two engines can be compared
// trade-for-trade without storing millions of records.
//
// "Maker" = the order that was already resting in the book.
// "Taker" = the incoming order that hit it.
// The trade always happens at the MAKER's price.
struct TradeSink {
    uint64_t count = 0, volume = 0, checksum = 0;
    bool print = false;

    void on_trade(OrderId maker, OrderId taker, Price px, Qty qty) {
        ++count;
        volume += qty;
        for (uint64_t field : {uint64_t(maker), uint64_t(taker), uint64_t(px), uint64_t(qty)})
            checksum = (checksum ^ field) * 1099511628211ULL;   // FNV-1a mixing step
        if (print)
            std::printf("    TRADE  %3u @ %d   (resting order #%u, incoming order #%u)\n",
                        qty, px, maker, taker);
    }
};

// =============================================================================
// 2. MapBook : the textbook version
// =============================================================================
//
// Three containers, one per question the engine must answer:
//
//   "What is the best price?"          -> std::map sorted by price. begin() is
//                                         the best level. O(log L) to insert a
//                                         new price level, L = number of levels.
//   "Who is first in line at a price?" -> std::list at each level, oldest first.
//   "Where is order #id?" (for cancel) -> unordered_map from id to a list
//                                         iterator, so a cancel is O(1) and does
//                                         not have to search the queue.
//
// This is correct and easy to read. It is slow for one reason: every node of a
// map, a list and an unordered_map is a separate heap allocation, scattered
// around memory. Walking them means cache miss after cache miss.

class MapBook {
    struct Order { OrderId id; Qty qty; };
    using Queue = std::list<Order>;

    std::map<Price, Queue, std::greater<Price>> bids_;   // highest price first
    std::map<Price, Queue>                      asks_;   // lowest price first

    struct Handle { Side side; Price price; Queue::iterator pos; };
    std::unordered_map<OrderId, Handle> index_;

    // Match an incoming order against the opposite side of the book.
    // `crosses(p)` answers "is resting price p acceptable to the incoming order?"
    // Returns whatever quantity is left unfilled.
    template <class BookSide, class Crosses>
    Qty match(BookSide& book, OrderId taker, Qty qty, Crosses crosses) {
        while (qty > 0 && !book.empty()) {
            auto level = book.begin();              // best price on the other side
            if (!crosses(level->first)) break;      // best price isn't good enough: stop
            Queue& queue = level->second;
            while (qty > 0 && !queue.empty()) {
                Order& maker = queue.front();       // oldest order at this price goes first
                Qty fill = std::min(qty, maker.qty);
                sink.on_trade(maker.id, taker, level->first, fill);
                maker.qty -= fill;
                qty       -= fill;
                if (maker.qty == 0) {               // resting order fully filled: remove it
                    index_.erase(maker.id);
                    queue.pop_front();
                }
            }
            if (queue.empty()) book.erase(level);   // price level used up: remove it
        }
        return qty;
    }

    // Put the unfilled remainder into the book, at the BACK of its price queue.
    template <class BookSide>
    void rest(BookSide& book, OrderId id, Side side, Price px, Qty qty) {
        Queue& queue = book[px];                    // creates the level if it's new
        queue.push_back({id, qty});
        index_[id] = {side, px, std::prev(queue.end())};
    }

public:
    TradeSink sink;

    MapBook(Price /*num_ticks*/, size_t expected_orders) { index_.reserve(expected_orders); }

    // A limit order: "buy/sell up to qty, at price px or better".
    //   Step 1: trade against the other side for as long as prices cross.
    //   Step 2: whatever is left rests in the book (unless ioc, where it is dropped).
    // ioc = "immediate or cancel".
    void limit(OrderId id, Side side, Price px, Qty qty, bool ioc = false) {
        if (side == Side::Buy) {
            qty = match(asks_, id, qty, [px](Price ask) { return ask <= px; });
            if (qty > 0 && !ioc) rest(bids_, id, side, px, qty);
        } else {
            qty = match(bids_, id, qty, [px](Price bid) { return bid >= px; });
            if (qty > 0 && !ioc) rest(asks_, id, side, px, qty);
        }
    }

    // A market order: "trade now at whatever price is available".
    // It is just an IOC limit order with the most generous possible price.
    void market(OrderId id, Side side, Qty qty) {
        limit(id, side, side == Side::Buy ? INT32_MAX : INT32_MIN, qty, true);
    }

    // Cancel a resting order. Returns false if the id is unknown, which happens
    // in real life whenever the order was filled just before the cancel arrived.
    bool cancel(OrderId id) {
        auto found = index_.find(id);
        if (found == index_.end()) return false;
        Handle h = found->second;
        if (h.side == Side::Buy) {
            auto level = bids_.find(h.price);
            level->second.erase(h.pos);
            if (level->second.empty()) bids_.erase(level);
        } else {
            auto level = asks_.find(h.price);
            level->second.erase(h.pos);
            if (level->second.empty()) asks_.erase(level);
        }
        index_.erase(found);
        return true;
    }

    Price  best_bid()    const { return bids_.empty() ? NO_BID : bids_.begin()->first; }
    Price  best_ask()    const { return asks_.empty() ? NO_ASK : asks_.begin()->first; }
    size_t live_orders() const { return index_.size(); }
};

// =============================================================================
// 3. FastBook : the low-latency version
// =============================================================================
//
// Same rules, same public interface. Four changes, each removing a specific cost:
//
//  (a) PRICE LEVELS LIVE IN A FLAT ARRAY indexed by price.
//      levels_[px] is one array lookup instead of a walk down a red-black tree.
//      Cost: prices must fit in [0, num_ticks). Real exchanges do have a
//      bounded price band per instrument per day, so this is a fair assumption.
//
//      One array serves both sides. A book is never "crossed" (best bid is
//      always below best ask), so any price holds bids or asks, never both.
//
//  (b) ORDERS LIVE IN ONE PRE-ALLOCATED POOL (a std::vector<Order>).
//      No malloc/free while trading. A freed slot goes onto a free list and
//      is reused by the next order, so the hot slots stay in the CPU cache.
//
//  (c) THE FIFO QUEUE IS AN INTRUSIVE LIST.
//      The prev/next links are stored inside the Order itself, as 32-bit pool
//      indices. Compared to std::list<Order> there is no separate node, and
//      indices are half the size of pointers and stay valid if the pool grows.
//
//  (d) ORDER LOOKUP IS A PLAIN ARRAY: slot_of_[id] -> pool slot.
//      No hashing. This works because ids are handed out as 1, 2, 3, ...
//      If your ids were arbitrary numbers you would use an open-addressing
//      hash table here instead.
//
// We also keep best_bid_ and best_ask_ as two integers, so "what is the best
// price?" costs nothing. When the best level empties we step to the next
// non-empty price. That scan is short in practice because real books are dense
// near the top; replacing it with a bitmap is one of the suggested extensions.

class FastBook {
    static constexpr uint32_t NIL = 0xFFFFFFFFu;   // "no slot", the null of pool indices

    struct Order {            // 24 bytes: two and a half orders fit in one 64-byte cache line
        uint32_t prev, next;  // neighbours in this price's queue (next doubles as the free-list link)
        OrderId  id;
        Qty      qty;
        Price    price;
        Side     side;
    };
    struct Level {            // 8 bytes: eight price levels per cache line
        uint32_t head = NIL;  // oldest order = first to trade
        uint32_t tail = NIL;  // newest order = where new arrivals join
    };

    Price                 num_ticks_;
    std::vector<Level>    levels_;     // (a) indexed by price
    std::vector<Order>    pool_;       // (b) every order, resting or free
    std::vector<uint32_t> slot_of_;    // (d) order id -> pool slot, NIL if not resting
    uint32_t              free_head_ = NIL;
    Price                 best_bid_  = NO_BID;   // -1 when there are no bids
    Price                 best_ask_;             // num_ticks_ when there are no asks
    size_t                live_      = 0;

    // ---- pool -------------------------------------------------------------
    uint32_t acquire() {
        if (free_head_ == NIL) {                 // pool exhausted: grow (never happens if pre-sized well)
            pool_.emplace_back();
            return uint32_t(pool_.size() - 1);
        }
        uint32_t slot = free_head_;
        free_head_ = pool_[slot].next;
        return slot;
    }
    void release(uint32_t slot) {
        slot_of_[pool_[slot].id] = NIL;
        pool_[slot].next = free_head_;           // push onto the free list
        free_head_ = slot;
        --live_;
    }

    // ---- intrusive doubly linked list --------------------------------------
    // Remove one order from its queue in O(1), wherever it sits in the queue.
    void unlink(Level& level, uint32_t slot) {
        Order& o = pool_[slot];
        if (o.prev != NIL) pool_[o.prev].next = o.next; else level.head = o.next;
        if (o.next != NIL) pool_[o.next].prev = o.prev; else level.tail = o.prev;
    }

    // ---- matching -----------------------------------------------------------
    // Trade the incoming order against the queue at one price, oldest first.
    Qty fill_level(Price px, OrderId taker, Qty qty) {
        Level& level = levels_[px];
        while (qty > 0 && level.head != NIL) {
            uint32_t slot  = level.head;
            Order&   maker = pool_[slot];
            Qty fill = std::min(qty, maker.qty);
            sink.on_trade(maker.id, taker, px, fill);
            maker.qty -= fill;
            qty       -= fill;
            if (maker.qty == 0) { unlink(level, slot); release(slot); }
        }
        return qty;
    }

    void rest(OrderId id, Side side, Price px, Qty qty) {
        uint32_t slot  = acquire();
        Level&   level = levels_[px];
        pool_[slot] = {level.tail, NIL, id, qty, px, side};          // join at the back
        if (level.tail != NIL) pool_[level.tail].next = slot; else level.head = slot;
        level.tail = slot;
        if (id >= slot_of_.size()) slot_of_.resize(std::max<size_t>(id + 1, slot_of_.size() * 2), NIL);
        slot_of_[id] = slot;
        ++live_;
    }

    // Move the best price to the next level that still has orders.
    void next_ask() { do ++best_ask_; while (best_ask_ < num_ticks_ && levels_[best_ask_].head == NIL); }
    void next_bid() { do --best_bid_; while (best_bid_ >= 0         && levels_[best_bid_].head == NIL); }

public:
    TradeSink sink;

    FastBook(Price num_ticks, size_t expected_orders)
        : num_ticks_(num_ticks), levels_(num_ticks), slot_of_(expected_orders + 1, NIL),
          best_ask_(num_ticks) {
        pool_.reserve(expected_orders);          // all memory is claimed here, before trading starts
    }

    void limit(OrderId id, Side side, Price px, Qty qty, bool ioc = false) {
        if (px < 0 || px >= num_ticks_ || qty == 0) return;   // reject: outside the price band

        if (side == Side::Buy) {
            // If there are no asks, best_ask_ == num_ticks_ > px, so the loop ends by itself.
            while (qty > 0 && best_ask_ <= px) {
                qty = fill_level(best_ask_, id, qty);
                if (levels_[best_ask_].head == NIL) next_ask();
            }
            if (qty > 0 && !ioc) {
                rest(id, side, px, qty);
                if (px > best_bid_) best_bid_ = px;           // we may be the new best bid
            }
        } else {
            // If there are no bids, best_bid_ == -1 < px, so the loop ends by itself.
            while (qty > 0 && best_bid_ >= px) {
                qty = fill_level(best_bid_, id, qty);
                if (levels_[best_bid_].head == NIL) next_bid();
            }
            if (qty > 0 && !ioc) {
                rest(id, side, px, qty);
                if (px < best_ask_) best_ask_ = px;           // we may be the new best ask
            }
        }
    }

    void market(OrderId id, Side side, Qty qty) {
        limit(id, side, side == Side::Buy ? num_ticks_ - 1 : 0, qty, true);
    }

    bool cancel(OrderId id) {
        if (id >= slot_of_.size() || slot_of_[id] == NIL) return false;
        uint32_t slot = slot_of_[id];
        Price px   = pool_[slot].price;
        Side  side = pool_[slot].side;
        Level& level = levels_[px];
        unlink(level, slot);
        release(slot);
        if (level.head == NIL) {                              // we emptied the level...
            if      (side == Side::Buy  && px == best_bid_) next_bid();   // ...and it was the best bid
            else if (side == Side::Sell && px == best_ask_) next_ask();   // ...or the best ask
        }
        return true;
    }

    Price  best_bid()    const { return best_bid_; }
    Price  best_ask()    const { return best_ask_ == num_ticks_ ? NO_ASK : best_ask_; }
    size_t live_orders() const { return live_; }

    // Total quantity resting at one price (walks the queue; for display only).
    Qty volume_at(Price px) const {
        Qty total = 0;
        for (uint32_t s = levels_[px].head; s != NIL; s = pool_[s].next) total += pool_[s].qty;
        return total;
    }

    // Print the top `depth` levels on each side, asks above bids, like a trading screen.
    void print(int depth = 5) const {
        std::vector<Price> asks;
        for (Price p = best_ask_; p < num_ticks_ && int(asks.size()) < depth; ++p)
            if (levels_[p].head != NIL) asks.push_back(p);
        std::printf("      BID qty | price | ASK qty\n");
        for (auto p = asks.rbegin(); p != asks.rend(); ++p)
            std::printf("              | %5d | %u\n", *p, volume_at(*p));
        int shown = 0;
        for (Price p = best_bid_; p >= 0 && shown < depth; --p)
            if (levels_[p].head != NIL) { std::printf("      %7u | %5d |\n", volume_at(p), p); ++shown; }
        std::printf("\n");
    }
};

// =============================================================================
// 4. DEMO : a tiny scenario you can trace by hand
// =============================================================================

#define CHECK(cond) \
    do { if (!(cond)) { std::printf("CHECK FAILED line %d: %s\n", __LINE__, #cond); std::exit(1); } } while (0)

void demo() {
    std::printf("================ DEMO ================\n\n");
    FastBook book(1000, 64);
    book.sink.print = true;

    std::printf("Five orders arrive. None of them cross, so they all rest:\n");
    book.limit(1, Side::Sell, 101, 100);
    book.limit(2, Side::Sell, 101,  50);   // same price as #1, arrived later: queues behind #1
    book.limit(3, Side::Sell, 102,  70);
    book.limit(4, Side::Buy,   99,  80);
    book.limit(5, Side::Buy,  100,  40);
    book.print();
    CHECK(book.best_bid() == 100 && book.best_ask() == 101);
    CHECK(book.sink.count == 0);

    std::printf("Order #6: BUY 120 @ 101. It crosses the asks at 101.\n");
    std::printf("#1 was first in line so it fills completely, then #2 gives the last 20:\n");
    book.limit(6, Side::Buy, 101, 120);
    book.print();
    CHECK(book.sink.count == 2 && book.sink.volume == 120);
    CHECK(book.volume_at(101) == 30);       // 30 left of order #2
    CHECK(book.best_ask() == 101);

    std::printf("Cancel order #3 (the 70 @ 102):\n");
    CHECK(book.cancel(3));
    CHECK(!book.cancel(3));                 // cancelling twice fails the second time
    CHECK(!book.cancel(1));                 // #1 was already fully filled
    book.print();

    std::printf("Order #7: SELL 60 at market. It takes the best bid first (100), then 99:\n");
    book.market(7, Side::Sell, 60);
    book.print();
    CHECK(book.sink.count == 4 && book.sink.volume == 180);
    CHECK(book.best_bid() == 99 && book.volume_at(99) == 60);
    CHECK(book.live_orders() == 2);         // what's left of #2 and of #4

    std::printf("All demo checks passed.\n\n");
}

// =============================================================================
// 5. BENCHMARK : same order stream through both books
// =============================================================================
//
// Rules of honest measurement used here:
//   - The whole stream is generated BEFORE the clock starts.
//   - A fixed seed makes every run and both engines see identical input.
//   - Throughput (average) and per-message latency (percentiles) are measured
//     in separate passes, because reading the clock twice per message costs
//     about as much as the work being measured.
//   - Each measurement is repeated and the best run is kept, to filter out
//     noise from other programs on the machine.

enum class MsgType : uint8_t { Limit, Ioc, Cancel };
struct Msg { MsgType type; Side side; OrderId id; Price px; Qty qty; };

constexpr Price NUM_TICKS = 1 << 14;   // 16,384 possible prices

// A synthetic market: a "fair price" that drifts as a random walk, with
//   50% passive limit orders placed a few ticks away from it,
//   42% cancels of earlier orders (in real markets most orders are cancelled),
//    8% aggressive IOC orders that cross the spread and cause trades.
std::vector<Msg> make_flow(size_t n, uint64_t seed) {
    std::mt19937_64 rng(seed);
    auto rnd = [&](uint32_t k) { return uint32_t(rng() % k); };   // roughly uniform in [0, k)

    std::vector<Msg> flow;
    flow.reserve(n);
    std::vector<OrderId> maybe_live;       // ids we could cancel (some will have traded already)
    Price   mid     = NUM_TICKS / 2;
    OrderId next_id = 1;
    size_t  warmup  = std::min<size_t>(n / 10, 50000);   // start by building some depth

    while (flow.size() < n) {
        if (rnd(40) == 0) mid += Price(rnd(3)) - 1;      // fair price drifts by -1, 0 or +1
        mid = std::clamp(mid, Price(200), Price(NUM_TICKS - 200));
        // mid = std::min(std::max(mid, Price(200)), Price(NUM_TICKS - 200));

        uint32_t r    = flow.size() < warmup ? 0 : rnd(100);
        Side     side = rnd(2) ? Side::Buy : Side::Sell;

        if (r < 50) {                                    // passive: most land close to the mid
            Price off = 1 + Price(std::min(rnd(40), rnd(40)));
            Price px  = side == Side::Buy ? mid - off : mid + off;
            flow.push_back({MsgType::Limit, side, next_id, px, 1 + rnd(100)});
            maybe_live.push_back(next_id++);
        } else if (r < 92 && !maybe_live.empty()) {      // cancel a random earlier order
            size_t k = rng() % maybe_live.size();
            flow.push_back({MsgType::Cancel, Side::Buy, maybe_live[k], 0, 0});
            maybe_live[k] = maybe_live.back();
            maybe_live.pop_back();
        } else {                                         // aggressive: priced through the mid
            Price px = side == Side::Buy ? mid + 3 : mid - 3;
            flow.push_back({MsgType::Ioc, side, next_id++, px, 1 + rnd(150)});
        }
    }
    return flow;
}

template <class Book>
inline void apply(Book& book, const Msg& m) {
    switch (m.type) {
        case MsgType::Limit:  book.limit(m.id, m.side, m.px, m.qty);       break;
        case MsgType::Ioc:    book.limit(m.id, m.side, m.px, m.qty, true); break;
        case MsgType::Cancel: book.cancel(m.id);                           break;
    }
}

using Clock = std::chrono::steady_clock;
inline uint64_t ns_between(Clock::time_point a, Clock::time_point b) {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
}

// What the book looked like after the whole stream: used to compare engines.
struct Outcome {
    uint64_t trades, volume, checksum;
    Price    bid, ask;
    size_t   live;
    bool operator==(const Outcome& o) const {
        return trades == o.trades && volume == o.volume && checksum == o.checksum &&
               bid == o.bid && ask == o.ask && live == o.live;
    }
};

template <class Book>
Outcome bench(const char* name, const std::vector<Msg>& flow) {
    const size_t n = flow.size();
    Outcome out{};

    // Pass 1: throughput. One clock read before, one after. Best of 3 runs.
    uint64_t best_total = UINT64_MAX;
    for (int run = 0; run < 3; ++run) {
        Book book(NUM_TICKS, n);
        auto t0 = Clock::now();
        for (const Msg& m : flow) apply(book, m);
        auto t1 = Clock::now();
        best_total = std::min(best_total, ns_between(t0, t1));
        out = {book.sink.count, book.sink.volume, book.sink.checksum,
               book.best_bid(), book.best_ask(), book.live_orders()};
    }

    // Pass 2: latency of every single message, to see the slow outliers.
    std::vector<uint32_t> lat(n);
    {
        Book book(NUM_TICKS, n);
        for (size_t i = 0; i < n; ++i) {
            auto t0 = Clock::now();
            apply(book, flow[i]);
            auto t1 = Clock::now();
            lat[i] = uint32_t(std::min<uint64_t>(ns_between(t0, t1), UINT32_MAX));
        }
    }
    std::sort(lat.begin(), lat.end());
    auto pct = [&](double p) { return lat[std::min(n - 1, size_t(p * double(n)))]; };

    double mean = double(best_total) / double(n);
    std::printf("  %-9s %8.1f %10.2f %8u %8u %8u %10u\n",
                name, mean, 1000.0 / mean, pct(0.50), pct(0.99), pct(0.999), lat.back());
    return out;
}

int main(int argc, char** argv) {
    size_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 2000000;
    // std::printf("LALALALALALAAALALALAL\n");
    demo();

    std::printf("============== BENCHMARK ==============\n\n");
    std::vector<Msg> flow = make_flow(n, 42);

    // How long does reading the clock itself take? Pass 2 numbers include this.
    uint64_t overhead = 0;
    for (int i = 0; i < 1000000; ++i) {
        auto a = Clock::now();
        auto b = Clock::now();
        overhead += ns_between(a, b);
    }

    std::printf("  %zu messages, times in nanoseconds\n\n", n);
    std::printf("  %-9s %8s %10s %8s %8s %8s %10s\n",
                "engine", "mean", "M msgs/s", "p50", "p99", "p99.9", "max");
    Outcome a = bench<MapBook>("MapBook", flow);
    Outcome b = bench<FastBook>("FastBook", flow);

    std::printf("\n  clock overhead: about %.0f ns, included in p50/p99/p99.9/max but not in mean\n",
                double(overhead) / 1e6);
    std::printf("  trades: %llu   volume traded: %llu   orders still resting: %zu\n",
                (unsigned long long)b.trades, (unsigned long long)b.volume, b.live);
    std::printf("  final best bid / ask: %d / %d\n\n", b.bid, b.ask);

    if (a == b) {
        std::printf("  PASS: both engines produced identical trades and identical final books.\n");
        return 0;
    }
    std::printf("  FAIL: the two engines disagree. One of them has a bug.\n");
    return 1;
}
