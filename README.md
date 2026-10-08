# High-Frequency Trading (HFT) Matching Engine

An ultra-low-latency High-Frequency Trading (HFT) matching engine architected for sub-microsecond order execution. This project demonstrates advanced system-level optimizations, including lock-free data structures, memory pooling, and a robust microservices ecosystem for parsing financial protocols and broadcasting market data.

## What it does

- Maintains a two-sided order book (bids and asks)
- Matches incoming orders against resting orders using price-time priority
- Supports Limit, Market, IOC, and FOK order types
- Cancel and modify in O(1)
- Reports trades via zero-allocation callback
