# High-Frequency Trading (HFT) Matching Engine

An ultra-low-latency High-Frequency Trading (HFT) matching engine architected for sub-microsecond order execution. This project demonstrates advanced system-level optimizations, including lock-free data structures, memory pooling, and a robust microservices ecosystem for parsing financial protocols and broadcasting market data.

## What it does

- Maintains a two-sided order book (bids and asks)
- Matches incoming orders against resting orders using price-time priority
- Supports Limit, Market, IOC, and FOK order types
- Cancel and modify in O(1)
- Reports trades via zero-allocation callback


lob.cpp is a working matching engine written twice: a textbook version (MapBook) and a low-latency version (FastBook). On 2,000,000 messages the low-latency version is about 3.6x faster on average, and both produce identical trades.

## The concepts you need

- Limit order: "buy (or sell) up to this quantity at this price or better." If nothing matches, it waits in the book.
- Order book: all waiting orders. Buy orders are bids; sell orders are asks. The highest bid and lowest ask are the "best" prices; the gap between them is the spread.
- Matching: an incoming buy trades when its price is at or above the best ask (and the mirror for sells). The trade happens at the resting order's price.
- Price-time priority: better price trades first; at the same price, whoever arrived first trades first. This rule is why each price needs a first-in-first-out queue.
- Market order: trade now at any price. In the code, it is a limit order with the most generous price whose remainder is dropped ("immediate or cancel").
- Cancel: remove a waiting order. Most orders in real markets are cancelled, so this must be as fast as adding one.

FastBook is faster because it never allocates memory while trading and keeps its data packed together, so the CPU cache is hit far more often. The comments in the file explain each line; read sections 1 to 3, then trace the demo in section 4 by hand against the printed output.

##Running it

g++ -O2 -std=c++17 lob.cpp -o lob && ./lob

In Colab, start a cell with **%%writefile lob.cpp**, paste the file contents under it, then run the command above in a new cell with ! in front.
