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
doubly-linked order so can access its price level immediately; 
we store both next and prev pointers (intrusive) to allow linking the 
removed orders neighbours immediately in queues remove_order
*/
struct Order {
    int client_id=0;
    int book_id=0;
    uint32_t token=0;
    bool is_buy = false;
    uint32_t quantity=0;
    int price=0;
    Order* next = nullptr; 
    Order* prev = nullptr;
    OrderQueue* plvl_q = nullptr;
};

struct PoolAlloc{
    std::vector<Order> mem_block;
    std::vector<uint32_t> free_list;

    PoolAlloc(size_t size) {
        mem_block.resize(size);
        free_list.reserve(size);
        for (uint32_t i =0; i<size; ++i){
            free_list.push_back(i);
        }
    }

    Order* allocate(){   //we receive the next available order object via LIFO which leverages hot cache 
        if (free_list.empty()) return nullptr;
        uint32_t o_idx = free_list.back();
        free_list.pop_back();
        Order* o = &mem_block[o_idx];
        return o;
    }

    void deallocate(Order* o) {
        uint32_t idx = static_cast<uint32_t> (o - mem_block.data());
        free_list.push_back(idx);
    }
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

    void remove_order(Order* o) { //O(1)  for K orders in queue as we have prev pointer
        if (o->prev) o->prev->next = o->next;
        else head = o->next;

        if (o->next) o->next->prev = o->prev;
        else tail = o->prev;

        o->plvl_q = nullptr;
    }
};

struct OrderBook {

    static constexpr int MAX_PRICE = 10000;
    std::vector<OrderQueue> bids;
    std::vector<OrderQueue> asks;
    std::vector<uint64_t> bid_bits;
    std::vector<uint64_t> ask_bits;

    int best_bid_pr = -1;
    int best_ask_pr = -1;
    
    OrderBook() : bids(MAX_PRICE), asks(MAX_PRICE), bid_bits((MAX_PRICE + 63) / 64, 0), ask_bits((MAX_PRICE + 63) / 64, 0) {}

    void cancel(Order*);
    void match_process(Order*, PoolAlloc&, auto& token_map);
    int find_next(const std::vector<uint64_t>&, int, int);
    int find_prev(const std::vector<uint64_t>&, int);


    inline void set_bit(std::vector<uint64_t>&bits, int idx){
        bits[idx >> 6] |= (1ull << (idx & 63));
    }
    inline void clear_Bit(std::vector<uint64_t> &bits, int idx){
        bits[idx >> 6] &= ~(1ull << (idx & 63));
    }
    inline bool test_bit(const std::vector<uint64_t>&bits, int idx){
        return bits[idx >> 6] & (1ull << (idx & 63));
    }

    void add_to_book(Order* o) {   
        if (o->is_buy) {
            bids[o->price].push_back(o);
            set_bit(bid_bits, o->price);
            if (o->price > best_bid_pr) {
                best_bid_pr = o->price;
            }
        } else {
            asks[o->price].push_back(o);
            set_bit(ask_bits, o->price);
            if (best_ask_pr == -1 || o->price < best_ask_pr) {
                best_ask_pr = o->price;
            }
        }
    }
};

//we perform linear scans which is only fine under non-sparse assumtpion, 
//we could have a set to store iterator to next, but may as well use a self-balancing map, 
//best option is bitmap

//the following function math I did not know, I had to look up; this is someone elses code
int OrderBook::find_next(const std::vector<uint64_t>& bits, int idx, int max_idx){
    if (idx >= max_idx) return -1;
    int word= idx >> 6; // division by 64  i.e 2^6

    uint64_t mask = ~((1ull << (idx & 63)) - 1); //create mask to ignore all price levels *before idx in this word
    uint64_t val = bits[word] & mask;
    while(true){
        if(val) return (word << 6) + __builtin_ctzll(val);  //return absolute index of set bit we found
        if(++word > ( max_idx >> 6)) break; //if not, we move to the next 64bit word and check again
        val = bits[word];
    }
    return -1;
}

int OrderBook::find_prev(const std::vector<uint64_t>& bits, int idx){
    if (idx < 0) return -1;
    int word= idx >> 6;
    uint64_t mask = ((1ull << ((idx & 63) + 1)) - 1);
    uint64_t val = bits[word] & mask;
    while(true){
        if(val) return (word << 6) + (63 - __builtin_clzll(val));
        if(--word < 0) break;
        val = bits[word];
    }
    return -1;
}
/*
matching and executing logic ; we traverse all price levels
while order isnt fulfilled we go through each next sorted queue of orders, trying to fulfill it
anything leftover of incoming is placed onto book, and any used-up resting orders will be removed from book + cleaned up
*/
void OrderBook::match_process(Order* o_inc, PoolAlloc& pool, auto& token_map) {
    std::map<int, uint32_t> execs;
    bool is_buy = o_inc->is_buy;
    
    while (o_inc->quantity > 0 ) {
        int& best_pr = is_buy ? best_ask_pr : best_bid_pr;
        bool prices_cross = is_buy ? (o_inc->price >= best_pr) : (o_inc->price <= best_pr);
        if (!prices_cross || best_pr == -1) break;

        OrderQueue& cur_lvl = is_buy ? asks[best_pr] : bids[best_pr];
        Order* o_rest = cur_lvl.head;

        while (o_rest && o_inc->quantity > 0) {
            Order* o_rest_next = o_rest->next; 

            // if (o_rest->client_id == o_inc->client_id) { //self trade
            //     o_rest = o_rest_next; 
            //     continue; 
            // }
      
            uint32_t traded_qty = std::min(o_inc->quantity, o_rest->quantity);
            printf("E, Client %d, Token %u, %u, %d\n", o_rest->client_id, o_rest->token, traded_qty, best_pr);
            
            execs[best_pr] += traded_qty; 
            o_inc->quantity -= traded_qty;
            o_rest->quantity -= traded_qty;

            if (o_rest->quantity == 0) {
                cur_lvl.remove_order(o_rest); 
                token_map.erase(o_rest->token);
                pool.deallocate(o_rest);
            }

            o_rest = o_rest_next; 
        }
        
        if (cur_lvl.empty()) {
            clear_Bit(is_buy ? ask_bits : bid_bits, best_pr);
        }

        if (o_inc->quantity > 0) { 
            if (is_buy) {  ///find next best ask
                best_pr = find_next(ask_bits, best_pr + 1, MAX_PRICE - 1);
            } else { //find next best bid
                best_pr = find_prev(bid_bits, best_pr - 1);
            }
        } else break; 
        
    }

    for (const auto& e : execs) {
        printf("E, Client %d, Token %u, %u, %d\n", o_inc->client_id, o_inc->token, e.second, e.first);
    }

    if (o_inc->quantity > 0) {
        add_to_book(o_inc);
    } else {
        token_map.erase(o_inc->token);
        pool.deallocate(o_inc);
    }
}

void OrderBook::cancel(Order* order) {
    if (!order || !order->plvl_q) return;
    OrderQueue* q = order->plvl_q;
    q->remove_order(order);

    if (q->empty()) {
        clear_Bit(order->is_buy ? bid_bits : ask_bits, order->price);
        if (order->is_buy && (order->price == best_bid_pr)) {
                best_bid_pr = find_prev(bid_bits, best_bid_pr - 1); 
        } else if (order->price == best_ask_pr) {
                best_ask_pr = find_next(ask_bits, best_ask_pr + 1, MAX_PRICE - 1);
        }
    }
}

struct simulator{
    PoolAlloc order_pool;
    std::unordered_map<uint32_t, Order*> token_to_order;
    std::map<int, OrderBook> all_books;

    simulator(size_t pool_size = 11000) : order_pool(pool_size) {};

    void create_order(Order* order);
    void cancel_order(uint32_t token);
    void process_message(const std::string_view& line);
    void print_fstate() const;
};


void simulator::create_order(Order* order) {
    //out of bounds, or somehow duplicate order 
    if (order->price < 0 || order->price >= OrderBook::MAX_PRICE || token_to_order.count(order->token)) {
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
    size_t start = 0, end = 0;
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
    for (const auto& [book_id, book] : all_books){
        for (const auto& queue : book.bids){
            for (Order* o = queue.head; o!=nullptr; o=o->next){
                printf("O, Client %d, Orderbook %d, Token %u, B, %u, %d\n", o->client_id, book_id, o->token, o->quantity, o->price);
            }
        }
        for (const auto& queue : book.asks){
            for (Order* o = queue.head; o!=nullptr; o=o->next){
                printf("O, Client %d, Orderbook %d, Token %u, S, %u, %d\n", o->client_id, book_id, o->token, o->quantity, o->price);
            }
        }
    }
}


int main(int argc, char* argv[]) {
    simulator sim;
    std::string filename = argc > 1 ? argv[1] : "data/input_orders.txt";
    std::ifstream infile(filename);
    if (!infile) {
        std::cerr << "cant open file\n";
        return 1;
    }

    std::string line;
    while (getline(infile, line)) {
        sim.process_message(line);
    }
    sim.print_fstate();

    return 0;
}
