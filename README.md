[some slides I made to detail things](https://docs.google.com/presentation/d/1cTItXUDj4uLFCZI2_ZeLM9N24XDMve74x1uy0T8qtGk/edit?usp=sharing)

simulator's core is OrderBook , the orderbook contains the two sides of bids and asks

Each side is a doubly linked list (pricelevel) objects, sorted by price priority (bids=high-to-low, asks= low-to-high)

at each pricelevel there is a queue (custom `OrderQueue` for pointer reallocation) of orders listed at that price ;  this queue ensures time priority  i.e first-in, first-out


#### Matching / processing:

Upon every order entered from input:

- parser splits line into string_view tokens (no copies)

- route to either create or cancel order on first token

- on new order: we collect next available object from the pool (`allocate()`) - populate it with our fields, and pass to create_order

- we check order within price bounds, then we find existing or create its associated book, then we proceed to matching

- get opposite pricelevl, and compare the head of the queues for cross match; if true we go through the level trading resting order quantities until either the level is exhausted or incoming order is fulfilled. If new level then we have to **re-search** for the next non-empty price level (this is one fault point of flat vector approach)

- if order fulfilled we return memory to pool, else we add it to book 

Any cancellations are handled by removing order from price level, then returning memory to pool by deallocating 



```mermaid
classDiagram
    direction LR

    class simulator {
        <<Manager>>
        -OrderPool order_pool
        -unordered_map~uint32_t, Order*~ token_to_order
        -unordered_map~int, OrderBook~ all_books
        +process_message(string_view) void
        +print_fstate() void
    }

    class OrderBook {
        <<Instrument>>
        -vector~PriceLevel~ bids
        -vector~PriceLevel~ asks
        -int best_bid_pr
        -int best_ask_pr
        +match_process(Order*) void
        +add_to_book(Order*) void
        +cancel(Order*) void
    }

    class OrderPool {
        <<Memory Manager>>
        -vector~Order*~ pool
        +allocate() Order*
        +deallocate(Order*) void
    }

    class PriceLevel {
        <<Price Point>>
        +OrderQueue orders
    }
    
    class OrderQueue {
        <<Intrusive FIFO Queue>>
        +Order* head
        +Order* tail
        +push_back(Order*) void
        +remove_order(Order*) void
    }

    class Order {
        <<Data Object>>
        +int client_id
        +int book_id
        +uint32_t token
        +bool is_buy
        +uint32_t quantity
        +int price
        +Order* next
        +Order* prev
        +PriceLevel* price_lvl
    }

    simulator "1" o-- "1" OrderPool : owns
    simulator "1" *-- "0..*" OrderBook : manages
    simulator "1" o-- "0..*" Order : tracks all by token
    
    OrderBook "1" *-- "*" PriceLevel : contains (bids/asks vectors)
    PriceLevel "1" *-- "1" OrderQueue : contains
    OrderQueue "1" o-- "0..*" Order : manages (head/tail pointers)
```