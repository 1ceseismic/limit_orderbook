#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <map>
#include <cstdint>
#include <string_view>
#include <memory>

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


struct OrderQueue;
/*
doubly-linked order object can access its price level immediately; 
we store both next and prev pointers (intrusive) to allow linking the 
removed orders neighbours immediately in queues remove_order
*/
struct Order {
    int client_id =0;
    int book_id =0;
    uint32_t token =0;
    bool is_buy = false;
    uint32_t quantity =0;
    int price = 0;
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


//an intrusive linked list to let us remove from any index in queue whilst keeping continuous memory
struct OrderQueue {
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
        o->plvl_q = this;
    }

    Order* pop_front(){
        if (empty()) return nullptr;

        Order* o = head;
        head = head->next;
        if (head) {
            head->prev = nullptr;
        } else {
            tail = nullptr;
        }
        o->plvl_q = nullptr;
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

struct OrderBook {

    static constexpr int MAX_PRICE = 100000;
    std::vector<OrderQueue> bids;
    std::vector<OrderQueue> asks;
    int best_bid_pr = -1;
    int best_ask_pr = -1;
    
    OrderBook() : bids(MAX_PRICE), asks(MAX_PRICE) {}

    //we perform linear scans which is only fine under non-sparse assumtpion, 
    //we could have a set to store iterator to next, but may as well use a self-balancing map, 
    //best option is bitmap
    void find_next_best_bid(){
        int start_price = (best_bid_pr == -1) ? MAX_PRICE - 1 : best_bid_pr -1;

        for (int i=start_price; i>=0; --i){
            if (!bids[i].empty()){
                best_bid_pr = i;
                return;
            }
        }
        best_bid_pr = -1;
    }

    void find_next_best_ask(){
        int start_price = (best_ask_pr == -1) ? 0 : best_ask_pr +1;

        for (int i=start_price; i<MAX_PRICE; ++i){
            if (!asks[i].empty()){
                best_ask_pr = i;
                return;
            }
        }
        best_ask_pr = -1;
    }

    void add_to_book(Order* o) {   
        if (o->is_buy) {
            bids[o->price].push_back(o);
            if (o->price > best_bid_pr) {
                best_bid_pr = o->price;
            }
        } else {
            asks[o->price].push_back(o);
            if (best_ask_pr == -1 || o->price < best_ask_pr) {
                best_ask_pr = o->price;
            }
        }
    }

    void cancel(Order* order) {
        if (!order || !order->plvl_q) return;
        OrderQueue* q = order->plvl_q;

        q->remove_order(order);

        if (q->empty()) { //we have to find next best price for that queues next match
            if (order->is_buy && order->price == best_bid_pr) {
                find_next_best_bid();
            } else if (!order->is_buy && order->price == best_ask_pr) {
                find_next_best_ask();
            }
        }
    }

    /*
    matching and executing logic ; we traverse all price levels;
    while order isnt fulfilled we go through each next sorted queue of orders, trying to fulfill it
    anything leftover of incoming is placed onto book, and any used-up resting orders will be removed from book + cleaned up
    */
    void match_process(Order* o_inc, OrderPool& pool, auto& token_map) {
        std::vector<std::pair<int, uint32_t>> execs;

        bool is_buy = o_inc->is_buy;
        
        while (o_inc->quantity > 0 ) {
            int& best_pr = is_buy ? best_ask_pr : best_bid_pr;
            bool prices_cross = is_buy ? o_inc->price >= best_pr : o_inc->price <= best_pr;
            if (!prices_cross || best_pr == -1) break;

            OrderQueue& cur_lvl = is_buy ? asks[best_pr] : bids[best_pr];
            uint32_t qty_traded_onlevel  = 0;
            
            while (!cur_lvl.empty() && o_inc->quantity > 0) {
                Order* o_rest = cur_lvl.head;
                if (o_rest->client_id == o_inc->client_id) break; //no self trades ; we just exit
          
                uint32_t traded_qty = std::min(o_inc->quantity, o_rest->quantity);

                printf("E, Client %d, Token %u, %u, %d\n", o_rest->client_id, o_rest->token, traded_qty, best_pr);
                
                o_inc->quantity -= traded_qty;
                o_rest->quantity -= traded_qty;
                qty_traded_onlevel  += traded_qty;

                if (o_rest->quantity == 0) {
                    cur_lvl.pop_front();
                    token_map.erase(o_rest->token);
                    pool.deallocate(o_rest);
                }
            }
            
            if (qty_traded_onlevel  > 0) {
                execs.push_back({best_pr, qty_traded_onlevel});
            }

            if (cur_lvl.empty()) {
                if (is_buy) find_next_best_ask();
                else find_next_best_bid();
            }
        }

        for (const auto& exec : execs) {
            printf("E, Client %d, Token %u, %u, %d\n", o_inc->client_id, o_inc->token, exec.second, exec.first);
        }

        if (o_inc->quantity > 0) {
            add_to_book(o_inc);
        } else {
            token_map.erase(o_inc->token);
            pool.deallocate(o_inc);
        }
    }

    void print_state() const {
        if (best_ask_pr != -1) {
            for (int i = best_ask_pr; i < MAX_PRICE; ++i) {
                if (!asks[i].empty()) {
                    for (Order* o = asks[i].head; o != nullptr; o = o->next) {
                        printf("O, Client %d, Orderbook %d, Token %u, S, %u, %d\n", o->client_id, o->book_id, o->token, o->quantity, o->price);
                    }
                }
            }
        }
        if (best_bid_pr != -1) {
            for (int i = best_bid_pr; i >= 0; --i) {
                if (!bids[i].empty()) {
                    for (Order* o = bids[i].head; o != nullptr; o = o->next) {
                        printf("O, Client %d, Orderbook %d, Token %u, B, %u, %d\n", o->client_id, o->book_id, o->token, o->quantity, o->price);
                    }
                }
            }
        }
    }

};


struct simulator{
    OrderPool order_pool;
    std::unordered_map<uint32_t, Order*> token_to_order;
    std::map<int, OrderBook> all_books;

    simulator(size_t pool_size = 2000) : order_pool(pool_size) {};

    void create_order(Order* order);
    void cancel_order(uint32_t token);
    void process_message(const std::string_view& line);
    void print_fstate() const;
};


void simulator::create_order(Order* order) {
    
    if (order->price < 0 || order->price >= OrderBook::MAX_PRICE) {
        order_pool.deallocate(order);
        return; 
    }
    if (token_to_order.count(order->token)) { //duplicate
        order_pool.deallocate(order);
        return;
    }

    printf("A, Client %d, Token %u\n", order->client_id, order->token);

    token_to_order[order->token] = order;
    if (all_books.find(order->book_id) == all_books.end()) {
        all_books.emplace(order->book_id, OrderBook());
    }
    all_books.at(order->book_id).match_process(order, order_pool, token_to_order);
}


void simulator::cancel_order(uint32_t token) {
    
    auto it = token_to_order.find(token);
    if (it == token_to_order.end()) return; //probably already fulfilled
    

    Order* order = it->second;
    printf("C, Client %d, Token %u\n", order->client_id, order->token);

    auto book_it = all_books.find(order->book_id);
    if (book_it != all_books.end()){
        book_it->second.cancel(order);
    }
    token_to_order.erase(it);
    order_pool.deallocate(order);

}


void simulator::process_message(const std::string_view& line) {
    if (line.empty()) return;

    std::vector<std::string_view> tokens;
    size_t start = 0;
    size_t end = 0;
    while ((end = line.find(',', start)) != std::string::npos) {
        tokens.push_back(trim({&line[start], end - start}));
        start = end + 1;
    }
    tokens.push_back(trim({&line[start], line.length() - start}));
    if (tokens.empty()) return;


    if (tokens[0] == "O") {
        Order* order = order_pool.allocate();
        if (!order) {
            printf("order pool exhausted\n");
            return;
        }
        order->client_id = parse_int(tokens[1]);
        order->book_id = parse_int(tokens[2]);
        order->token  = parse_int(tokens[3]);
        order->is_buy = (tokens[4] == "B");
        order->quantity = parse_int(tokens[5]);
        order->price  = parse_int(tokens[6]);
      
        create_order(order);
        
    } else if (tokens[0] == "X") {
        uint32_t otoken = parse_int(tokens[2]);
        cancel_order(otoken);
    }
}


void simulator::print_fstate() const{
    printf("\n");
    for (const auto& book_p : all_books){
        book_p.second.print_state();
    }
}


int main(int argc, char* argv[]) {
    simulator sim;
    std::string filename = argc > 1 ? argv[1] : "input_orders.txt";
    std::ifstream infile(filename);
    if (!infile) {
        std::cerr << "cant open file\n";
        return 1;
    }

    std::string line;
    while (getline(infile, line)) {
        if (line.empty()) continue;
        sim.process_message(line);
    }

    sim.print_fstate();

    return 0;
}
