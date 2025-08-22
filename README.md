[some slides I made to detail things](https://docs.google.com/presentation/d/1cTItXUDj4uLFCZI2_ZeLM9N24XDMve74x1uy0T8qtGk/edit?usp=sharing)

simulator's core is OrderBook , the orderbook contains the two sides of bids and asks

Each side (bid/ask) there is a storage of Orders, sorted by price priority (bids=high-to-low, asks= low-to-high) either via map or linked list.

at each pricelevel there is a queue/list (non custom queue impl version) of orders listed for that price ;  this queue is time prioritised  i.e first-in, first-out


#### Matching / processing:

Upon every order entered from input:

- this incoming order is checked against the opposite side of its designated book for a price match, if not then we create a new level and place this new order in it 

- a match will then accumulate a quantity off of order trades in that matched price level's queue until either incoming is fulfilled or level is empty. Each trade occurs at the resting order's price

- If order is fully filled; it's removed from the book and separately if the price level becomes empty, it's also removed to keep book clean

- if this incoming order is still only partially filled after going through all matching resting orders ;  its remaining quantity is added to the book as a new resting order at its specified price level

Any cancellations are handled by looking up the order's unique token in the token->Order ptr hashmap and removing it from its price queue

also,  for simplicity and since its very small project ; manually managing the memory is easier + more suitable over smart pointers overhead (even though completely negligible for this)



#### multiple books:

simulator Class:
- top-manager that contains a map i.e std::map<int, OrderBook> all_books, its the main router

When an order arrives for a new orderbook ID;  an `OrderBook` is dynamically created and inserted into that map,  all messages with that book id are routed to it

Orderbook struct:
- Each instance represents a single instrument
- has its own bid and ask sides and is completely independent of other book instances

Our pooling covers all books though, so one large allocation to handle all books simulatenously


#### flatvec_intrusive:
```mermaid
classDiagram
    direction LR

    class simulator {
        <<Manager>>
        -OrderPool order_pool
        -map~int, OrderBook~ all_books
        -unordered_map~uint32_t, Order*~ token_to_order
        +process_message(string_view) void
        +create_order(Order*) void
    }

    class OrderBook {
        <<Instrument>>
        -vector~OrderQueue~ bids
        -vector~OrderQueue~ asks
        -int best_bid_pr
        -int best_ask_pr
        +process_incoming_order(Order*) void
        +add_to_book(Order*) void
    }

    class OrderQueue {
      <<Intrusive FIFO Queue>>
    }

    simulator "1" o-- "1" OrderPool : uses
    simulator "1" *-- "0..*" OrderBook : manages
    OrderBook "1" *-- "0..*" OrderQueue : contains price levels (indexed by price)
```

#### map_LL_intrusive:
```mermaid
classDiagram
    direction LR

    class simulator {
        <<Manager>>
        -OrderPool order_pool
        -map~int, OrderBook~ all_books
        -unordered_map~uint32_t, Order*~ token_to_order
        +process_msg(string) void
        +cancel_order(Order*) void
    }

    class OrderBook {
        <<Instrument>>
        -map~int, OrderQueue~ bids
        -map~int, OrderQueue~ asks
        -OrderQueue* best_bid_q
        -OrderQueue* best_ask_q
        +process_order(Order*) void
        +cancel_order(Order*) void
    }

    class OrderQueue {
      <<Intrusive FIFO Queue>>
    }

    simulator "1" o-- "1" OrderPool : uses
    simulator "1" *-- "0..*" OrderBook : manages
    OrderBook "1" *-- "0..*" OrderQueue : contains price levels
```


#### map_stdlist:
```mermaid
classDiagram
    class Order {
        +string client_id
        +string token
        +bool is_buy
        +int quantity
        +int price
        +PriceLevel* price_lvl
        +list~Order*~::iterator q_pos
    }

    class PriceLevel {
        +list~Order*~ orders
    }

    class OrderBook {
        +map~int, PriceLevel~ bids
        +map~int, PriceLevel~ asks
        +unordered_map~string, Order*~ token_to_order
    }

    OrderBook "1" -- "0..*" PriceLevel : manages
    OrderBook "1" -- "0..*" Order : tracks via map
    PriceLevel "1" -- "0..*" Order : contains pointer to
    Order "1" -- "1" PriceLevel : points to

```

#### Order struct helpers:

```mermaid
classDiagram
    direction LR

    class OrderPool {
        <<Memory Manager>>
        -vector~unique_ptr~Order~~ mem_block
        -vector~Order*~ free_list
        +allocate() Order*
        +deallocate(Order*) void
    }

    class OrderQueue {
        <<Intrusive FIFO Queue>>
        -Order* head
        -Order* tail
        +push_back(Order*) void
        +pop_front() Order*
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
        +OrderQueue* plvl_q
    }

    OrderPool "1" o-- "0..*" Order : manages memory for
    OrderQueue "1" o-- "0..*" Order : links (head/tail)
    Order "1" -- "1" OrderQueue : knows its queue


```