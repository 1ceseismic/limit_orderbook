#include <iostream>
#include <fstream>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <map>
#include <cstdint>
#include <memory>
#include <string>

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

/*
doubly-linked order object  can access its price level immediately;  we store
both next and prev pointers to allow 'linking' the removed orders neighbours immediately in queues remove_order
*/
struct OrderQueue;
struct Order {
    int client_id;
    int book_id;
    uint32_t token;
    bool is_buy;
    uint32_t quantity;
    int price;
    Order* next = nullptr; 
    Order* prev = nullptr;
    OrderQueue* plvl_q = nullptr;
};

struct OrderPool{
    std::vector<std::unique_ptr<Order>> mem_block;
    std::vector<Order*> free_list;


    OrderPool(size_t size) { 
        mem_block.reserve(size);
        free_list.reserve(size);
        for (size_t i =0; i<size; ++i){
            mem_block.push_back(std::make_unique<Order>());
            free_list.push_back(mem_block.back().get());
        }
    }

    Order* allocate(){   //we give the next available order object LIFO
        if (free_list.empty()) return nullptr;
        
        Order* order = free_list.back();
        free_list.pop_back();
        return order;
    }

    void deallocate(Order* o) {free_list.push_back(o);}
};

//an intrusive linked list alternative to let us remove from any index in queue
struct OrderQueue {
    Order* head = nullptr;
    Order* tail = nullptr;

    bool empty() const { return head == nullptr; }

    void push_back(Order* o) {
        o->next = nullptr;
        o->prev = tail;
        if (tail) tail->next = o;
        else  head = o;
        
        tail = o;
        o->plvl_q = this;
    }

    Order* pop_front(){
        if (empty()) return nullptr;

        Order* o = head;
        head = head->next;
        if (head)  head->prev = nullptr;
        else tail = nullptr;
        
        return o;
    }

    void remove_order(Order* o) { //O(1)  for K orders in queue as we have prev pointer
        if (o->prev) o->prev->next = o->next;
        else head = o->next;

        if (o->next) o->next->prev = o->prev;
        else tail = o->prev;

        o->plvl_q = nullptr;
    }
};


/*
orderbook struct for holding heads for O(1) retrieving est bid & offer
if lookup miss , we find where to insert new price level per order type
*/
struct OrderBook {

    std::map<int, OrderQueue, std::greater<int>> bids; //these only exist for the quick lookup of existing levels we can insert into
    std::map<int, OrderQueue> asks;
    void match_process (Order*, auto&, OrderPool&);
    void cancel (Order* o_to_cancel);
    OrderQueue* best_bid_q = nullptr;
    OrderQueue* best_ask_q = nullptr;

    void update_best_bid(){
        best_bid_q = bids.empty() ? nullptr : &bids.begin()->second;
    }
    void update_best_ask(){
        best_ask_q = asks.empty() ? nullptr : &asks.begin()->second;
    }

    void add_to_book(Order* o){
        if (o->is_buy){
            bids[o->price].push_back(o);
            update_best_bid(); 
        } 
        else{
            asks[o->price].push_back(o);
            update_best_ask();   
        } 
    }
    const auto& get_bids() const { return bids;}
    const auto& get_asks() const {return asks;}
};



class simulator {
    std::unordered_map<uint32_t, Order*> token_to_order;
    OrderPool order_pool;
    std::map<int, OrderBook> all_books;

public:
    simulator( size_t pool_size = 5000) : order_pool(pool_size) {};
    void process_msg(const std::string_view& line);
    void print_fstate() const;
    void cancel_order(Order* order);
    void create_order(Order* order);
};


/*
matching and executing logic ; we traverse all price levels (the ladder);
for each pricelevel we go through the sorted queue of orders, trying to fulfill the incoming order
anything leftover of incoming is placed onto book, and any used-up resting orders will be removed from book + cleaned up
  got rid of our memory pooling so deleting manually agaub - but its slightly cleaner

*/
void OrderBook::match_process(Order* incoming_o, auto& token_map, OrderPool& pool) {
    std::map<int, uint32_t> execs;

    auto match_engine = [&](OrderQueue*& best_q) {
        while (incoming_o->quantity > 0 && best_q != nullptr) { //we check every  available price level to iterate over 
            OrderQueue* q = best_q;
            int trade_price = q->head->price;

            bool prices_cross = incoming_o->is_buy ? (incoming_o->price >= trade_price) : (incoming_o->price <= trade_price);
            if (!prices_cross) break;

            while (!q->empty() && incoming_o->quantity > 0) { // actually traverse the level
                Order* resting_ord = q->head;

                uint32_t traded_qty = std::min(incoming_o->quantity, resting_ord->quantity);
                printf("E, Client %d, Token %u, %u, %d\n", resting_ord->client_id, resting_ord->token, traded_qty, resting_ord->price);
                
                execs[trade_price] += traded_qty; 

                incoming_o->quantity -= traded_qty;
                resting_ord->quantity -= traded_qty;

                if (resting_ord->quantity == 0) {
                    q->pop_front();
                    token_map.erase(resting_ord->token);
                    pool.deallocate(resting_ord);
                }
            }
            
            if (q->empty()) {
                if (incoming_o->is_buy) {
                    asks.erase(trade_price); //we cleanup empty levels
                    update_best_ask();
                } else {
                    bids.erase(trade_price);
                    update_best_bid();
                }
            }
        }
    };

    if (incoming_o->is_buy) {
        match_engine(best_ask_q);
    } else {
        match_engine(best_bid_q);
    }

    for (const auto& entry : execs) {
        printf("E, Client %d, Token %u, %u, %d\n", incoming_o->client_id, incoming_o->token, entry.second, entry.first);
    }
    
    if (incoming_o->quantity > 0) {
        add_to_book(incoming_o);
    } else { //fully fulfilled
        token_map.erase(incoming_o->token);
        pool.deallocate(incoming_o);
    }
}

void OrderBook::cancel(Order* order){

    if (!order || !order->plvl_q)  return;
    OrderQueue* q = order->plvl_q;

    if (!q) return;
    q->remove_order(order);

    if (q->empty()){
        if (order->is_buy){
            bids.erase(order->price);
            update_best_bid();
        } else {
            asks.erase(order->price);
            update_best_ask();
        }
    }
}

void simulator::process_msg(const std::string_view& line) {
    if (line.empty()) return;

    std::vector<std::string_view> tokens;
    size_t start = 0, end = 0;
    while ((end = line.find(',', start)) != std::string_view::npos) {
        tokens.push_back(trim({line.data() + start, end - start}));
        start = end + 1;
    }
    tokens.push_back(trim({line.data() + start, line.length() - start}));
    if (tokens.empty()) return;

    if (tokens[0] == "O") {
        Order* order = order_pool.allocate();
        if (!order) {
            printf("order pool exhausted\n");
            return;
        }
        order->client_id = parse_int(tokens[1]);
        order->book_id = parse_int(tokens[2]);
        order->token = parse_int(tokens[3]);
        order->is_buy = (tokens[4] == "B");
        order->quantity = parse_int(tokens[5]);
        order->price = parse_int(tokens[6]);

        create_order(order);

    } else if (tokens[0] == "X") {
        uint32_t token_to_cancel = parse_int(tokens[2]);
        auto it = token_to_order.find(token_to_cancel);
        if (it != token_to_order.end()) {
            cancel_order(it->second);
        }
    }
}


void simulator::cancel_order(Order* order) {
    auto it = token_to_order.find(order->token);
    if (it == token_to_order.end()) return; //probably already fulfilled
    
    auto book_it = all_books.find(order->book_id);
    if (book_it == all_books.end()) return;
    
    printf("C, Client %d, Token %u\n", order->client_id, order->token);

    OrderBook& book = book_it->second;
    book.cancel(order);
    token_to_order.erase(order->token);
    order_pool.deallocate(order);
}


void simulator::create_order(Order* order) {
    if (order->price < 0) { 
        order_pool.deallocate(order);
        return; 
    }
    if (token_to_order.count(order->token)) { //duplicate
        order_pool.deallocate(order);
        return;
    }
    printf("A, Client %d, Token %u\n", order->client_id, order->token);
    
    token_to_order[order->token] = order;
    all_books[order->book_id].match_process(order, token_to_order, order_pool);
}


void simulator::print_fstate() const{
    printf("\n");
    for (const auto& [book_id, book] : all_books) {
        for (const auto& [price, queue] : book.get_bids()) {
            for (Order* o = queue.head; o != nullptr; o = o->next) {
                printf("O, Client %d, Orderbook %d, Token %u, B, %u, %d\n", o->client_id, book_id, o->token, o->quantity, o->price);
            }
        }
        for (const auto& [price, queue] : book.get_asks()) {
            for (Order* o = queue.head; o != nullptr; o = o->next) {
                printf("O, Client %d, Orderbook %d, Token %u, S, %u, %d\n", o->client_id, book_id, o->token, o->quantity, o->price);
            }
        }
    }

}
int main(int argc, char* argv[]) {
    std::string filename = argc > 1 ? argv[1] : "input_orders.txt";
    std::ifstream infile(filename);
    if (!infile) {
        std::cerr << "cant open file\n";
        return 1;
    }

    simulator sim;

    std::string line;
    while (getline(infile, line)) {
        sim.process_msg(line);
    }
    sim.print_fstate();

    return 0;
}
