#include <iostream>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <map>
#include <cstdint>
#include <chrono>
#include <random>
#include <cassert>
#include <algorithm> // For std::min
#include <memory>    // For std::unique_ptr
#include <list>      // For std::list based order book variant
#include <type_traits> // For std::is_same

#if defined(_WIN32)
  #define NOMINMAX
  #include <windows.h>
  #include <psapi.h>
#else
  #include <unistd.h>
  #include <fcntl.h>
  #include <sys/types.h>
  #include <sys/stat.h>
#endif

// ============================================================================
// GLOBAL UTILITIES
// ============================================================================

int parse_int(const std::string_view& sv) {
    int val = 0;
    for (char c : sv) {
        if (c >= '0' && c <= '9') {
            val = val * 10 + (c - '0');
        }
    }
    return val;
}

std::string_view trim(std::string_view sv) {
    sv.remove_prefix(std::min(sv.find_first_not_of(" \t\n\r"), sv.size()));
    sv.remove_suffix(std::min(sv.size() - sv.find_last_not_of(" \t\n\r") - 1, sv.size()));
    return sv;
}

// approximate RSS in bytes
uint64_t get_rss_bytes() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return (uint64_t)pmc.WorkingSetSize;
    }
    return 0;
#else
    // read /proc/self/statm: size resident share...
    FILE* f = fopen("/proc/self/statm", "r");
    if (!f) return 0;
    long size=0, resident=0;
    if (fscanf(f, "%ld %ld", &size, &resident) != 2) {
        fclose(f);
        return 0;
    }
    fclose(f);
    long page_size = sysconf(_SC_PAGESIZE);
    return (uint64_t)resident * (uint64_t)page_size;
#endif
}

// ============================================================================
// CORE DATA STRUCTURES: ORDER, ORDER QUEUES, ALLOCATORS
// ============================================================================

// Forward declaration for OrderQueue variants
struct IntrusiveOrderQueue; // Custom intrusive linked list
struct StdListOrderQueue;   // std::list based price level

// Unified Order struct
struct Order {
    int client_id = 0;
    int book_id = 0;
    uint32_t token = 0;
    bool is_buy = false;
    uint32_t quantity = 0;
    int price = 0;
    // For intrusive lists
    Order* next = nullptr;
    Order* prev = nullptr;
    IntrusiveOrderQueue* plvl_q_intrusive = nullptr;
    // For std::list based price levels
    std::list<Order*>::iterator q_pos_stdlist; // iterator within the std::list for O(1) removal
    StdListOrderQueue* plvl_q_stdlist = nullptr;

    // Default constructor/destructor/assignment needed for pools/arenas
    Order() = default;
    ~Order() = default;
    Order(const Order&) = default;
    Order& operator=(const Order&) = default;

    // Reset for pooling/arena reuse (only clear necessary fields)
    void reset() {
        client_id = 0;
        book_id = 0;
        token = 0;
        is_buy = false;
        quantity = 0;
        price = 0;
        next = nullptr;
        prev = nullptr;
        plvl_q_intrusive = nullptr;
        plvl_q_stdlist = nullptr;
        // q_pos_stdlist is managed by StdListOrderQueue, no need to reset here
    }
};

// Intrusive Linked List (used by OrderPool and BumpAllocator variants)
struct IntrusiveOrderQueue {
    Order* head = nullptr;
    Order* tail = nullptr;

    bool empty() const { return head == nullptr; }

    void push_back(Order* o) {
        o->next = nullptr;
        o->prev = tail;
        if (tail) {
            tail->next = o;
        } else {
            head = o;
        }
        tail = o;
        o->plvl_q_intrusive = this;
    }

    void remove_order(Order* o) { // O(1) for K orders in queue as we have prev pointer
        if (o->prev) o->prev->next = o->next;
        else head = o->next;

        if (o->next) o->next->prev = o->prev;
        else tail = o->prev;

        o->plvl_q_intrusive = nullptr; // Clear reference to this queue
    }
};

// PriceLevel using std::list (used by MapStdListOrderBook)
struct StdListOrderQueue {
    std::list<Order*> orders;

    void add_order(Order* order){
        orders.push_back(order);
        order->q_pos_stdlist = std::prev(orders.end()); // Store iterator for O(1) removal
        order->plvl_q_stdlist = this;
    }

    void remove_order(Order* order) {
        orders.erase(order->q_pos_stdlist);
        order->plvl_q_stdlist = nullptr; // Clear reference
    }

    bool empty() const { return orders.empty(); }
    Order* front() { return orders.empty() ? nullptr : orders.front(); }

    void pop_front() {
         if (!orders.empty()) {
            orders.front()->plvl_q_stdlist = nullptr; // Clear reference
            orders.pop_front();
        }
    }
};

// Base allocator interface
class IAllocator {
public:
    virtual ~IAllocator() = default;
    virtual Order* allocate() = 0;
    virtual void deallocate(Order* o) = 0;
    virtual void reset() = 0; // For arena-like allocators, or full pool reset
};

// 1. Order Pool Allocator (reuses memory with a free list)
class OrderPoolAllocator : public IAllocator {
    std::vector<Order> mem_block;
    std::vector<uint32_t> free_list; // Stores indices of free orders

public:
    OrderPoolAllocator(size_t pool_size) {
        mem_block.resize(pool_size);
        free_list.reserve(pool_size);
        for (uint32_t i = 0; i < pool_size; ++i) {
            free_list.push_back(i);
        }
    }

    Order* allocate() override {
        if (free_list.empty()) return nullptr;
        uint32_t o_idx = free_list.back();
        free_list.pop_back();
        Order* o = &mem_block[o_idx];
        o->reset(); // Reset fields to default state
        return o;
    }

    void deallocate(Order* o) override {
        uint32_t idx = static_cast<uint32_t>(o - mem_block.data());
        // Basic check for valid index
        if (idx < mem_block.size()) {
            free_list.push_back(idx);
        }
    }

    void reset() override {
        // Full reset for benchmarks. Re-adds all orders to free list.
        free_list.clear();
        for (uint32_t i = 0; i < mem_block.size(); ++i) {
            free_list.push_back(i);
        }
    }
};

// 2. Bump Allocator (arena-like, sequential allocation, no individual deallocation)
class BumpAllocator : public IAllocator {
    std::vector<Order> mem_block;
    size_t next_idx = 0;

public:
    BumpAllocator(size_t capacity) {
        mem_block.resize(capacity);
    }

    Order* allocate() override {
        if (next_idx >= mem_block.size()) return nullptr;
        Order* o = &mem_block[next_idx++];
        o->reset(); // Reset fields to default state
        return o;
    }

    void deallocate(Order* /*o*/) override {
        // No individual deallocation for a bump allocator; memory is released on reset/destruction
    }

    void reset() override {
        next_idx = 0;
        // Orders are re-initialized on allocate(), no need to clear entire block
    }
};

// 3. Standard Allocator (uses new/delete for each object)
class StandardAllocator : public IAllocator {
    // Keep track of allocated orders for cleanup on reset/destruction
    std::vector<Order*> allocated_orders;
public:
    StandardAllocator() {
        allocated_orders.reserve(100000); // Reserve for common cases to reduce reallocations
    }
    ~StandardAllocator() override {
        reset(); // Clean up any remaining orders
    }

    Order* allocate() override {
        Order* o = new Order();
        allocated_orders.push_back(o); // Track for deletion
        return o;
    }

    void deallocate(Order* o) override {
        // Find and remove from tracking list for efficient reset.
        // For benchmarks, this erase might be slow, but it reflects actual new/delete overhead.
        // A more optimized approach might be to use a `std::unordered_set` for faster lookup/removal.
        auto it = std::find(allocated_orders.begin(), allocated_orders.end(), o);
        if (it != allocated_orders.end()) {
            std::swap(*it, allocated_orders.back()); // Swap with back and pop for O(1) amortized removal
            allocated_orders.pop_back();
        }
        delete o;
    }

    void reset() override {
        for (Order* o : allocated_orders) {
            delete o;
        }
        allocated_orders.clear();
    }
};


// ============================================================================
// ORDER BOOK IMPLEMENTATIONS (using IAllocator)
// ============================================================================

// Base OrderBook interface
class IOrderBook {
public:
    virtual ~IOrderBook() = default;
    virtual void add_to_book(Order* o, std::unordered_map<uint32_t, Order*>& token_map, IAllocator& pool) = 0;
    virtual void cancel_order(Order* o_to_cancel, std::unordered_map<uint32_t, Order*>& token_map, IAllocator& pool) = 0;
    virtual void print_final_state() const = 0; // For verification, not timed
    virtual void reset_book() = 0; // Clear internal state of the order book
};

// 1. Map-based Intrusive Linked List Order Book (like map_LL_intrusive.cpp)
class MapIntrusiveOrderBook : public IOrderBook {
    std::map<int, IntrusiveOrderQueue, std::greater<int>> bids; // max price for bids
    std::map<int, IntrusiveOrderQueue> asks;                   // min price for asks
    IntrusiveOrderQueue* best_bid_q = nullptr;
    IntrusiveOrderQueue* best_ask_q = nullptr;

    void update_best_bid() {
        best_bid_q = bids.empty() ? nullptr : &bids.begin()->second;
    }
    void update_best_ask() {
        best_ask_q = asks.empty() ? nullptr : &asks.begin()->second;
    }

public:
    void add_to_book(Order* incoming_o, std::unordered_map<uint32_t, Order*>& token_map, IAllocator& pool) override {
        // This function now combines matching and adding any remaining quantity to the book.
        if (incoming_o->is_buy) { // Incoming is BUY, match against ASKS
            auto current_level_it = asks.begin();
            while (incoming_o->quantity > 0 && current_level_it != asks.end()) {
                IntrusiveOrderQueue& current_q = current_level_it->second;
                int trade_price = current_level_it->first;

                if (!current_q.head || incoming_o->price < trade_price) { // No orders at this level or price doesn't cross
                    ++current_level_it; // Move to the next ask level
                    continue;
                }

                Order* resting_ord = current_q.head;
                while (resting_ord && incoming_o->quantity > 0) { // Traverse orders at this price level
                    Order* rest_next = resting_ord->next;

                    if (incoming_o->client_id == resting_ord->client_id) { // Skip self-trades
                        resting_ord = rest_next;
                        continue;
                    }

                    uint32_t traded_qty = std::min(incoming_o->quantity, resting_ord->quantity);
                    incoming_o->quantity -= traded_qty;
resting_ord->quantity -= traded_qty;

                    if (resting_ord->quantity == 0) { // Resting order fully fulfilled
                        current_q.remove_order(resting_ord);
                        token_map.erase(resting_ord->token);
                        pool.deallocate(resting_ord);
                    }
                    resting_ord = rest_next;
                }

                // After trying to match all orders at current_level_it:
                if (current_q.empty()) {
                    current_level_it = asks.erase(current_level_it); // Erase returns iterator to next element
                    update_best_ask(); // Ensure best_ask_q is updated
                } else {
                    ++current_level_it; // Move to the next ask level
                }
            }
        } else { // Incoming is SELL, match against BIDS
            auto current_level_it = bids.begin();
            while (incoming_o->quantity > 0 && current_level_it != bids.end()) {
                IntrusiveOrderQueue& current_q = current_level_it->second;
                int trade_price = current_level_it->first;

                if (!current_q.head || incoming_o->price > trade_price) { // No orders at this level or price doesn't cross
                    ++current_level_it; // Move to the next bid level
                    continue;
                }

                Order* resting_ord = current_q.head;
                while (resting_ord && incoming_o->quantity > 0) { // Traverse orders at this price level
                    Order* rest_next = resting_ord->next;

                    if (incoming_o->client_id == resting_ord->client_id) { // Skip self-trades
                        resting_ord = rest_next;
                        continue;
                    }

                    uint32_t traded_qty = std::min(incoming_o->quantity, resting_ord->quantity);
                    incoming_o->quantity -= traded_qty;
                    resting_ord->quantity -= traded_qty;

                    if (resting_ord->quantity == 0) { // Resting order fully fulfilled
                        current_q.remove_order(resting_ord);
                        token_map.erase(resting_ord->token);
                        pool.deallocate(resting_ord);
                    }
                    resting_ord = rest_next;
                }

                // After trying to match all orders at current_level_it:
                if (current_q.empty()) {
                    current_level_it = bids.erase(current_level_it); // Erase returns iterator to next element
                    update_best_bid(); // Ensure best_bid_q is updated
                } else {
                    ++current_level_it; // Move to the next bid level
                }
            }
        }

        if (incoming_o->quantity > 0) { // If incoming order still has remaining quantity, add to book
            if (incoming_o->is_buy) {
                bids[incoming_o->price].push_back(incoming_o);
                update_best_bid();
            } else {
                asks[incoming_o->price].push_back(incoming_o);
                update_best_ask();
            }
        } else { // incoming_o fully fulfilled, deallocate
            token_map.erase(incoming_o->token); // Remove from simulator's tracking map
            pool.deallocate(incoming_o); // Return order to pool/deallocate
        }
    }

    void cancel_order(Order* order, std::unordered_map<uint32_t, Order*>& token_map, IAllocator& pool) override {
        if (!order || !order->plvl_q_intrusive) return;

        IntrusiveOrderQueue* q = order->plvl_q_intrusive;
        // The previous check `if (!q) return;` is redundant if `!order->plvl_q_intrusive` covers null cases.
        // Also, `q` must be valid here if `order->plvl_q_intrusive` is not null.

        // Store price and buy/sell status before removing from queue, as 'order' fields might be reset during deallocation.
        int price_to_check = order->price;
        bool is_buy_to_check = order->is_buy;

        q->remove_order(order);

        if (q->empty()) { // If price level becomes empty, remove it from the map
            if (is_buy_to_check) {
                bids.erase(price_to_check);
                update_best_bid();
            } else {
                asks.erase(price_to_check);
                update_best_ask();
            }
        }
        token_map.erase(order->token); // Remove from simulator's tracking map
        pool.deallocate(order); // Return order to pool/deallocate
    }

    void print_final_state() const override {
        // No printing for benchmark for performance measurement
    }

    void reset_book() override {
        bids.clear();
        asks.clear();
        best_bid_q = nullptr;
        best_ask_q = nullptr;
    }
};

// 2. Flat Vector with Bitmap Indexed Order Book (like flatvec_instrusive.cpp)
class FlatVectorBitmapOrderBook : public IOrderBook {
    static constexpr int MAX_PRICE = 100000;
    std::vector<IntrusiveOrderQueue> bids_levels;
    std::vector<IntrusiveOrderQueue> asks_levels;
    int best_bid_pr = -1;
    int best_ask_pr = -1;

    std::vector<uint64_t> bid_bits;
    std::vector<uint64_t> asks_bits; // Renamed for clarity from `ask_bits` in original

    // Bit manipulation helpers (from flatvec_instrusive.cpp)
    inline void set_bit(std::vector<uint64_t>& bits, int idx) {
        bits[idx >> 6] |= (1ull << (idx & 63));
    }
    inline void clear_bit(std::vector<uint64_t>& bits, int idx) {
        bits[idx >> 6] &= ~(1ull << (idx & 63));
    }
    // `test_bit` is not directly used in the match logic, but kept for completeness if needed.
    // inline bool test_bit(const std::vector<uint64_t>& bits, int idx) {
    //     return bits[idx >> 6] & (1ull << (idx & 63));
    // }

    // Find previous/next bit (using GCC builtins for performance, or std::countr_zero/std::countl_zero for C++20)
    // Assumes 0 <= idx < MAX_PRICE
    int find_prev(const std::vector<uint64_t>& bits, int idx) {
        if (idx < 0) return -1; // No previous price
        int word_idx = idx >> 6;
        uint64_t mask = ((1ull << ((idx & 63) + 1)) - 1); // Mask for bits up to and including idx in current word
        uint64_t val = (word_idx < bits.size() ? bits[word_idx] : 0) & mask;
        while (true) {
            if (val) return (word_idx << 6) + (63 - __builtin_clzll(val)); // Found highest set bit in word
            if (--word_idx < 0) break; // No more words
            val = bits[word_idx]; // Check full previous word
        }
        return -1;
    }

    int find_next(const std::vector<uint64_t>& bits, int idx, int max_idx) {
        if (idx >= max_idx) return -1; // No next price
        int word_idx = idx >> 6;
        uint64_t mask = ~((1ull << (idx & 63)) - 1); // Mask for bits from idx (inclusive) to end of current word
        uint64_t val = (word_idx < bits.size() ? bits[word_idx] : 0) & mask;
        while (true) {
            if (val) return (word_idx << 6) + __builtin_ctzll(val); // Found lowest set bit in word
            if (++word_idx >= bits.size()) break; // No more words
            val = bits[word_idx]; // Check full next word
        }
        return -1;
    }

public:
    FlatVectorBitmapOrderBook() :
        bids_levels(MAX_PRICE), asks_levels(MAX_PRICE),
        bid_bits((MAX_PRICE + 63) / 64, 0), asks_bits((MAX_PRICE + 63) / 64, 0) {}

    void add_to_book(Order* o_inc, std::unordered_map<uint32_t, Order*>& token_map, IAllocator& pool) override {
        // Match process is embedded directly in this add_to_book for FlatVecBitmap
        bool is_buy = o_inc->is_buy;

        while (o_inc->quantity > 0) {
            int& best_pr = is_buy ? best_ask_pr : best_bid_pr;
            bool prices_cross = is_buy ? (o_inc->price >= best_pr) : (o_inc->price <= best_pr);

            if (!prices_cross || best_pr == -1) break; // No cross or no best price available

            IntrusiveOrderQueue& cur_lvl = is_buy ? asks_levels[best_pr] : bids_levels[best_pr];
            Order* o_rest = cur_lvl.head;

            while (o_rest && o_inc->quantity > 0) {
                Order* o_rest_next = o_rest->next;

                if (o_rest->client_id == o_inc->client_id) { // Skip self trade
                    o_rest = o_rest_next;
                    continue;
                }

                uint32_t traded_qty = std::min(o_inc->quantity, o_rest->quantity);

                o_inc->quantity -= traded_qty;
                o_rest->quantity -= traded_qty;

                if (o_rest->quantity == 0) { // Resting order fully fulfilled
                    cur_lvl.remove_order(o_rest);
                    token_map.erase(o_rest->token);
                    pool.deallocate(o_rest);
                }
                o_rest = o_rest_next;
            }

            if (cur_lvl.empty()) { // If price level becomes empty
                clear_bit(is_buy ? asks_bits : bid_bits, best_pr);
            }

            if (o_inc->quantity > 0) { // Incoming order not fully filled, find next best price
                if (is_buy) {
                    best_pr = find_next(asks_bits, best_pr + 1, MAX_PRICE - 1);
                } else {
                    best_pr = find_prev(bid_bits, best_pr - 1);
                }
            } else break; // Incoming order fully fulfilled
        }

        if (o_inc->quantity > 0) { // Add remaining of incoming order to book
            if (is_buy) {
                bids_levels[o_inc->price].push_back(o_inc);
                set_bit(bid_bits, o_inc->price);
                if (o_inc->price > best_bid_pr) {
                    best_bid_pr = o_inc->price;
                }
            } else {
                asks_levels[o_inc->price].push_back(o_inc);
                set_bit(asks_bits, o_inc->price);
                if (best_ask_pr == -1 || o_inc->price < best_ask_pr) {
                    best_ask_pr = o_inc->price;
                }
            }
        } else { // fully fulfilled
            token_map.erase(o_inc->token);
            pool.deallocate(o_inc);
        }
    }

    void cancel_order(Order* order, std::unordered_map<uint32_t, Order*>& token_map, IAllocator& pool) override {
        if (!order || !order->plvl_q_intrusive) return;

        IntrusiveOrderQueue* q = order->plvl_q_intrusive;
        if (!q) return;
        q->remove_order(order);

        if (q->empty()) { // If price level becomes empty
            clear_bit(order->is_buy ? bid_bits : asks_bits, order->price);
            if (order->is_buy && (order->price == best_bid_pr)) {
                best_bid_pr = find_prev(bid_bits, best_bid_pr - 1);
            } else if (order->price == best_ask_pr) {
                best_ask_pr = find_next(asks_bits, best_ask_pr + 1, MAX_PRICE - 1);
            }
        }
        token_map.erase(order->token); // Remove from simulator's tracking map
        pool.deallocate(order); // Return order to pool/deallocate
    }

    void print_final_state() const override {
        // No printing for benchmark for performance measurement
    }

    void reset_book() override {
        // Clear all order queues (by resetting head/tail pointers) and bitmaps
        for (auto& q : bids_levels) { q.head = q.tail = nullptr; }
        for (auto& q : asks_levels) { q.head = q.tail = nullptr; }
        std::fill(bid_bits.begin(), bid_bits.end(), 0);
        std::fill(asks_bits.begin(), asks_bits.end(), 0);
        best_bid_pr = -1;
        best_ask_pr = -1;
    }
};

// 3. Map-based Std::List Order Book (like map_stdlist.cpp)
// This variant uses `std::list<Order*>` for price levels and is typically paired with `StandardAllocator`.
class MapStdListOrderBook : public IOrderBook {
private:
    StdListOrderQueue* best_bid_q = nullptr;
    StdListOrderQueue* best_ask_q = nullptr;
    std::map<int, StdListOrderQueue, std::greater<int>> bids;
    std::map<int, StdListOrderQueue> asks;

    void update_best_bid() {
        best_bid_q = bids.empty() ? nullptr : &(bids.begin()->second);
    }

    void update_best_ask() {
        best_ask_q = asks.empty() ? nullptr : &(asks.begin()->second);
    }

public:
    void add_to_book(Order* incoming_o, std::unordered_map<uint32_t, Order*>& token_map, IAllocator& pool) override {
        // This function combines matching and adding any remaining quantity to the book.
        if (incoming_o->is_buy) { // Incoming is BUY, match against ASKS
            auto current_level_it = asks.begin();
            while (incoming_o->quantity > 0 && current_level_it != asks.end()) {
                StdListOrderQueue& current_q = current_level_it->second;
                int trade_price = current_level_it->first;

                if (current_q.empty() || incoming_o->price < trade_price) { // No orders at this level or price doesn't cross
                    ++current_level_it; // Move to the next ask level
                    continue;
                }

                // For std::list based queue, matching needs to carefully handle front() and pop_front()
                // The original map_stdlist.cpp did not explicitly skip self-trades for std::list based levels.
                // Replicate that behavior to avoid complex iterator management for self-trades in std::list.
                Order* resting_ord_front = current_q.front();
                while (resting_ord_front && incoming_o->quantity > 0) {
                    int trade_qty = std::min(incoming_o->quantity, resting_ord_front->quantity);
                    incoming_o->quantity -= trade_qty;
                    resting_ord_front->quantity -= trade_qty;

                    if (resting_ord_front->quantity == 0) { // Resting order fully fulfilled
                        current_q.pop_front(); // Removes from list
                        token_map.erase(resting_ord_front->token);
                        pool.deallocate(resting_ord_front); // Deallocate using StandardAllocator
                    }
                    resting_ord_front = current_q.front(); // Get the new front, or nullptr if empty
                }

                // After trying to match all orders at current_level_it:
                if (current_q.empty()) {
                    current_level_it = asks.erase(current_level_it); // Erase returns iterator to next element
                    update_best_ask(); // Ensure best_ask_q is updated
                } else {
                    ++current_level_it; // Move to the next ask level
                }
            }
        } else { // Incoming is SELL, match against BIDS
            auto current_level_it = bids.begin();
            while (incoming_o->quantity > 0 && current_level_it != bids.end()) {
                StdListOrderQueue& current_q = current_level_it->second;
                int trade_price = current_level_it->first;

                if (current_q.empty() || incoming_o->price > trade_price) { // No orders at this level or price doesn't cross
                    ++current_level_it; // Move to the next bid level
                    continue;
                }

                Order* resting_ord_front = current_q.front();
                while (resting_ord_front && incoming_o->quantity > 0) {
                    int trade_qty = std::min(incoming_o->quantity, resting_ord_front->quantity);
                    incoming_o->quantity -= trade_qty;
                    resting_ord_front->quantity -= trade_qty;

                    if (resting_ord_front->quantity == 0) { // Resting order fully fulfilled
                        current_q.pop_front();
                        token_map.erase(resting_ord_front->token);
                        pool.deallocate(resting_ord_front);
                    }
                    resting_ord_front = current_q.front();
                }

                // After trying to match all orders at current_level_it:
                if (current_q.empty()) {
                    current_level_it = bids.erase(current_level_it);
                    update_best_bid();
                } else {
                    ++current_level_it;
                }
            }
        }

        if (incoming_o->quantity > 0) { // If incoming order still has remaining quantity, add to book
            if (incoming_o->is_buy) {
                bids[incoming_o->price].add_order(incoming_o);
                update_best_bid();
            } else {
                asks[incoming_o->price].add_order(incoming_o);
                update_best_ask();
            }
        } else { // incoming_o fully fulfilled, deallocate
            token_map.erase(incoming_o->token); // Remove from simulator's tracking map
            pool.deallocate(incoming_o); // Return order to pool/deallocate
        }
    }

    void cancel_order(Order* order, std::unordered_map<uint32_t, Order*>& token_map, IAllocator& pool) override {
        if (!order || !order->plvl_q_stdlist) return;

        StdListOrderQueue* level = order->plvl_q_stdlist;
        int price_to_check = order->price; // Capture price and is_buy before removal
        bool is_buy_to_check = order->is_buy;

        level->remove_order(order); // Remove from the std::list

        if (level->empty()) { // If price level becomes empty
            if (is_buy_to_check) {
                bids.erase(price_to_check);
                update_best_bid();
            } else {
                asks.erase(price_to_check);
                update_best_ask();
            }
        }
        token_map.erase(order->token); // Remove from simulator's tracking map
        pool.deallocate(order); // Deallocate using StandardAllocator
    }

    void print_final_state() const override {
        // No printing for benchmark for performance measurement
    }

    void reset_book() override {
        // For MapStdListOrderBook, orders are deleted via `pool.deallocate`
        // when they are matched or cancelled. Any remaining orders in the maps
        // would be cleaned up by the simulator's `allocator->reset()`
        bids.clear();
        asks.clear();
        best_bid_q = nullptr;
        best_ask_q = nullptr;
    }
};


// ============================================================================
// BENCHMARK SIMULATORS (combining Allocator and OrderBook)
// ============================================================================

struct SimulationStats {
    double total_time_ms = 0.0;
    uint64_t peak_rss_kib = 0;
    // Note: Collecting cache misses and CPU cycles directly from C++ code
    // is platform-specific and often requires external profiling tools (like Intel VTune, perf on Linux)
    // or specific hardware counter APIs (like PAPI, Windows Performance Counters).
    // For this benchmark, we'll focus on wall-clock time and RSS.
};

// Base Simulator interface
class ISimulator {
protected:
    std::unordered_map<uint32_t, Order*> token_to_order; // Tracks active orders by token
    std::unique_ptr<IAllocator> allocator; // Owned by simulator
    std::unique_ptr<IOrderBook> order_book; // Owned by simulator
    size_t pool_size; // Relevant for pool/arena allocators

public:
    ISimulator(size_t ps) : pool_size(ps) {
        token_to_order.reserve(ps); // Initial reserve for token map for efficiency
    }
    virtual ~ISimulator() = default;

    virtual void init() = 0; // Initialize allocator and order book
    virtual void process_msg(const std::string_view& line) = 0; // Process a single message
    virtual void reset() = 0; // Reset for next benchmark run, clearing all state
    virtual SimulationStats run(const std::string& filename) = 0; // Run a full simulation with stats
    virtual std::string get_name() const = 0; // Get a descriptive name for the simulator
};

// Generic Simulator implementation using template parameters for Allocator and OrderBook
template <typename TAllocator, typename TOrderBook>
class GenericSimulator : public ISimulator {
public:
    GenericSimulator(size_t ps) : ISimulator(ps) {}

    void init() override {
        // Allocate the correct allocator type. StandardAllocator doesn't use pool_size.
        if constexpr (std::is_same<TAllocator, StandardAllocator>::value) {
            allocator = std::make_unique<TAllocator>();
        } else {
            allocator = std::make_unique<TAllocator>(pool_size);
        }
        order_book = std::make_unique<TOrderBook>();
    }

    void process_msg(const std::string_view& line) override {
        if (line.empty()) return;

        std::vector<std::string_view> tokens;
        size_t start = 0, end = 0;
        while ((end = line.find(',', start)) != std::string_view::npos) {
            tokens.push_back(trim({line.data() + start, end - start}));
            start = end + 1;
        }
        tokens.push_back(trim({line.data() + start, line.length() - start}));
        if (tokens.empty()) return;

        if (tokens[0] == "O") { // Order message
            // Allocate a new order object
            Order* order = allocator->allocate();
            if (!order) {
                // std::cerr << "Order pool exhausted for " << get_name() << "\n"; // Commented for benchmark
                return;
            }
            // Populate order details
            order->client_id = parse_int(tokens[1]);
            order->book_id = parse_int(tokens[2]);
            order->token = parse_int(tokens[3]);
            order->is_buy = (tokens[4] == "B");
            order->quantity = parse_int(tokens[5]);
            order->price = parse_int(tokens[6]);

            // Check for duplicate token before adding to map and order book
            if (token_to_order.count(order->token)) {
                allocator->deallocate(order); // Deallocate duplicate order
                return;
            }
            token_to_order[order->token] = order; // Track the new order

            order_book->add_to_book(order, token_to_order, *allocator); // Pass to order book for matching/adding

        } else if (tokens[0] == "X") { // Cancel message
            uint32_t token_to_cancel = parse_int(tokens[2]);
            auto it = token_to_order.find(token_to_cancel);
            if (it != token_to_order.end()) {
                order_book->cancel_order(it->second, token_to_order, *allocator); // Process cancellation
            }
        }
    }

    void reset() override {
        token_to_order.clear(); // Clear simulator's tracking map
        order_book->reset_book(); // Clear internal state of the order book
        allocator->reset(); // Reset the allocator (e.g., clear free list, reset bump pointer)
    }

    SimulationStats run(const std::string& filename) override {
        reset(); // Ensure a clean start for each run

        // Initial RSS before processing messages
        uint64_t initial_rss = get_rss_bytes();
        uint64_t peak_rss = initial_rss;

        std::ifstream infile(filename);
        if (!infile) {
            std::cerr << "Error: Could not open file " << filename << "\n";
            return {}; // Return empty stats on error
        }

        using clk = std::chrono::high_resolution_clock;
        auto t0 = clk::now();

        std::string line;
        while (std::getline(infile, line)) {
            process_msg(line);
            peak_rss = std::max(peak_rss, get_rss_bytes()); // Track peak RSS during processing
        }

        auto t1 = clk::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        // Any orders remaining in `token_to_order` after `reset()` are handled by `allocator->reset()`
        // or `allocator`'s destructor, ensuring proper memory cleanup.

        SimulationStats stats;
        stats.total_time_ms = ms;
        stats.peak_rss_kib = peak_rss / 1024; // Convert bytes to KiB for display
        return stats;
    }

    std::string get_name() const override {
        std::string allocator_name;
        if constexpr (std::is_same<TAllocator, OrderPoolAllocator>::value) allocator_name = "OrderPool";
        else if constexpr (std::is_same<TAllocator, BumpAllocator>::value) allocator_name = "BumpArena";
        else if constexpr (std::is_same<TAllocator, StandardAllocator>::value) allocator_name = "StdNewDelete";
        else allocator_name = "UnknownAllocator";

        std::string orderbook_name;
        if constexpr (std::is_same<TOrderBook, MapIntrusiveOrderBook>::value) orderbook_name = "MapIntrusive";
        else if constexpr (std::is_same<TOrderBook, FlatVectorBitmapOrderBook>::value) orderbook_name = "FlatVecBitmap";
        else if constexpr (std::is_same<TOrderBook, MapStdListOrderBook>::value) orderbook_name = "MapStdList";
        else orderbook_name = "UnknownOrderBook";

        return orderbook_name + "_" + allocator_name;
    }
};

// Define specific simulator types for convenience
// Combinations of OrderBook implementations and Allocator types:

// 1. Map-based Intrusive Linked List Order Book (MapIntrusiveOrderBook)
using Simulator_MapIntrusive_OrderPool    = GenericSimulator<OrderPoolAllocator, MapIntrusiveOrderBook>;
using Simulator_MapIntrusive_BumpArena    = GenericSimulator<BumpAllocator, MapIntrusiveOrderBook>;
using Simulator_MapIntrusive_StdNewDelete = GenericSimulator<StandardAllocator, MapIntrusiveOrderBook>;

// 2. Flat Vector with Bitmap Indexed Order Book (FlatVectorBitmapOrderBook)
using Simulator_FlatVecBitmap_OrderPool    = GenericSimulator<OrderPoolAllocator, FlatVectorBitmapOrderBook>;
using Simulator_FlatVecBitmap_BumpArena    = GenericSimulator<BumpAllocator, FlatVectorBitmapOrderBook>;
// FlatVectorBitmap is designed for intrusive lists which work well with pooling/bump.
// While technically possible, using it with StandardAllocator might be less idiomatic due to intrusive nature,
// but included for completeness in testing all combinations.
using Simulator_FlatVecBitmap_StdNewDelete = GenericSimulator<StandardAllocator, FlatVectorBitmapOrderBook>;


// 3. Map-based Std::List Order Book (MapStdListOrderBook)
// This implementation uses std::list<Order*> which naturally pairs with new/delete for Order objects.
// Pairing with pooling/bump allocators would require careful management of `Order*` lifetimes
// within `std::list` and ensuring `std::list` doesn't make copies that break allocator assumptions.
// For now, we pair it with StandardAllocator as its most natural fit.
using Simulator_MapStdList_StdNewDelete = GenericSimulator<StandardAllocator, MapStdListOrderBook>;
// If desired, more complex combinations could be added with appropriate allocator logic.


// ============================================================================
// BENCHMARK HARNESS
// ============================================================================

void run_benchmark_suite(const std::vector<std::string>& filenames) {
    size_t default_pool_arena_size = 100000; // A reasonable default size for allocators
    size_t num_reps = 3; // Number of repetitions to take the best time

    std::vector<std::unique_ptr<ISimulator>> simulators;

    // Add all desired simulator combinations to the suite
    simulators.push_back(std::make_unique<Simulator_MapIntrusive_OrderPool>(default_pool_arena_size));
    simulators.push_back(std::make_unique<Simulator_MapIntrusive_BumpArena>(default_pool_arena_size));
    simulators.push_back(std::make_unique<Simulator_MapIntrusive_StdNewDelete>(0)); // Pool size not used by StdNewDelete

    simulators.push_back(std::make_unique<Simulator_FlatVecBitmap_OrderPool>(default_pool_arena_size));
    simulators.push_back(std::make_unique<Simulator_FlatVecBitmap_BumpArena>(default_pool_arena_size));
    simulators.push_back(std::make_unique<Simulator_FlatVecBitmap_StdNewDelete>(0)); // Pool size not used by StdNewDelete

    simulators.push_back(std::make_unique<Simulator_MapStdList_StdNewDelete>(0)); // Pool size not used by StdNewDelete

    std::cout << "--- Benchmark Suite ---\n";
    std::cout << "Default pool/arena size for relevant allocators: " << default_pool_arena_size << "\n";
    std::cout << "Number of repetitions per test: " << num_reps << "\n\n";

    for (const auto& filename : filenames) {
        std::cout << "Benchmarking with input file: " << filename << "\n";
        for (const auto& sim : simulators) {
            sim->init(); // Initialize allocator and order book (e.g., resize vectors, setup internal state)
            std::cout << "  " << sim->get_name() << ":\n";
            double best_time_ms = 1e308; // Initialize with a very large value
            uint64_t peak_rss_kib = 0;

            for (size_t i = 0; i < num_reps; ++i) {
                SimulationStats stats = sim->run(filename); // Run a single simulation, includes reset
                if (stats.total_time_ms < best_time_ms) {
                    best_time_ms = stats.total_time_ms; // Keep track of the best time
                }
                peak_rss_kib = std::max(peak_rss_kib, stats.peak_rss_kib); // Track the overall peak RSS
            }
            std::cout << "    Best Time: " << best_time_ms << " ms\n";
            std::cout << "    Peak RSS: " << peak_rss_kib << " KiB\n";
            std::cout << "\n";
        }
        std::cout << "-------------------------------------------\n\n";
    }
}


int main() {
    // Collect all workload files available in the project
    std::vector<std::string> workload_files = {
        "input_orders.txt",
        "work_1k.txt",
        "work_10k.txt",
        "work_1m.txt",
        "work_high-churn.txt"
    };

    run_benchmark_suite(workload_files);

    return 0;
}
