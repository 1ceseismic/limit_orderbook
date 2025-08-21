#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <string_view>

int parse_int(const std::string_view& sv) {
    int val = 0;
    bool found_digit = false;
    for (char c : sv) {
        if (c >= '0' && c <= '9') {
            val = val * 10 + (c- '0');
            found_digit = true;
        } else if (found_digit) break;
    }
    return val;
}

std::string_view trim(std::string_view sv) {
    sv.remove_prefix(std::min(sv.find_first_not_of(" \t\n\r"), sv.size()));
    sv.remove_suffix(std::min(sv.size() - sv.find_last_not_of(" \t\n\r") - 1, sv.size()));
    return sv;
}


struct PriceLevel;
/*
doubly-linked order object  can access its prcie level immediately;  we store
both next and prev pointers to allow 'linking' the removed orders neighbours immediately in queues remove_order
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
    PriceLevel* price_lvl = nullptr; //for O(1) erasures from its level - i do note this is semi-redundant with our bids/asks pindex; but its 1 less hashmap lookup and clearer code 
};

class OrderPool{
    private:

    std::vector<Order*> pool;
    void grow() {
        const int POOL_SZ = 10000;
        pool.reserve(POOL_SZ);

        for (int i=0; i < POOL_SZ; ++i) pool.push_back(new Order());
    }

    public:
    OrderPool() { grow();}
    ~OrderPool() { for (Order* o : pool) delete o;}

    Order* allocate(){   //we give the next available order object  LIFO
        if (pool.empty()) grow();
        
        Order* order = pool.back();
        pool.pop_back();
        return order;
    }

    void deallocate(Order* o) {pool.push_back(o);}
};


//an intrusive linked list alternative to let us remove from any index in queue
struct OrderQueue {
    Order* head = nullptr;
    Order* tail = nullptr;

    bool empty() const { return head == nullptr; }

    void push_back(Order* o) { //FIFO
        o->next = nullptr;
        o->prev = tail;
        if (empty()) {
            head = o;
            tail = o;
        } else {
            tail->next = o;
            tail = o;
        }
    }

    void remove_order(Order* o) { //O(1)  for K orders in queue as we have prev pointer
        if (o->prev) o->prev->next = o->next;
        else head = o->next;

        if (o->next) o->next->prev = o->prev;
        else tail = o->prev;
    }
};

struct PriceLevel{
    OrderQueue orders;
};


struct OrderBook{

    static constexpr int MAX_PRICE = 100000;
    std::vector<PriceLevel> bids;
    std::vector<PriceLevel> asks;
    int best_bid_pr = -1;
    int best_ask_pr = -1;

    std::unordered_map<uint32_t, Order*> token_to_order;
    OrderPool& order_pool;

    int find_next_best_bid(){
        int start_price = (best_bid_pr == -1) ? MAX_PRICE - 1 : best_bid_pr;

        for (int i=start_price; i>=0; --i){
            if (!bids[i].orders.empty()){
                best_bid_pr = i;
                return i;
            }
        }
        best_bid_pr = -1;
        return -1;
    }

    int find_next_best_ask(){
        int start_price = (best_ask_pr == -1) ? 0 : best_ask_pr;

        for (int i=start_price; i<MAX_PRICE; ++i){
            if (!asks[i].orders.empty()){
                best_ask_pr = i;
                return i;
            }
        }
        best_ask_pr = -1;
        return -1;
    }

    void add_to_book(Order* o) {
        if (o->is_buy){
            bids[o->price].orders.push_back(o);
            o->price_lvl = &bids[o->price];
            if (best_bid_pr  == -1 || o ->price > best_bid_pr){
                best_bid_pr = o->price;
            }
        }
        else {
            asks[o->price].orders.push_back(o);
            o->price_lvl = &bids[o->price];
            if (best_ask_pr  == -1 || o ->price > best_ask_pr){
                best_ask_pr = o->price;
            }
        }
    }

    void match_process(Order* incoming);
    void cancel(Order* o);

    void print_state () const;
    OrderBook(std::unordered_map<uint32_t, Order*>& token_map, OrderPool& pool) :
        bids(MAX_PRICE), asks(MAX_PRICE), token_to_order(token_map), order_pool(pool) {};

};


class simulator{
    OrderPool order_pool;
    std::unordered_map<uint32_t, Order*> token_to_order;
    std::unordered_map<int, OrderBook> all_books;

    void create_order(Order* order);
    void cancel_order(Order* order);

public:
    void process_message(std::string_view line);
    void print_fstate() const;

};


void OrderBook::match_process(Order* o_inc) {
    std::vector<std::pair<int, uint32_t>> execs;

    bool is_buy = o_inc->is_buy;
    auto& opp_book = is_buy ? asks : bids;
    int& best_pr = is_buy ? best_ask_pr : best_bid_pr;
    auto next_best_pr = [&]() { is_buy ? find_next_best_ask() : find_next_best_bid(); };
    
    auto prices_cross = [&](int rest_price) {
        return is_buy ? o_inc->price >= rest_price : o_inc->price <= rest_price;
    };

    while (o_inc->quantity > 0 && best_pr != -1 && prices_cross(best_pr)) {
        int current_price = best_pr;
        PriceLevel& cur_lvl = opp_book[current_price];
        OrderQueue& q = cur_lvl.orders;
        uint32_t qty_traded_onlevel  = 0;
        
        while (!q.empty() && o_inc->quantity > 0) {
            Order* o_rest = q.head;
            uint32_t traded_qty = std::min(o_inc->quantity, o_rest->quantity);

            printf("E, Client %d, Token %u, %u, %d\n", o_rest->client_id, o_rest->token, traded_qty, current_price);
            
            o_inc->quantity -= traded_qty;
            o_rest->quantity -= traded_qty;
            qty_traded_onlevel  += traded_qty;

            if (o_rest->quantity == 0) {
                q.remove_order(o_rest);
                token_to_order.erase(o_rest->token);
                order_pool.deallocate(o_rest);
            }
        }
        
        if (qty_traded_onlevel  > 0) {
            execs.push_back({current_price, qty_traded_onlevel});
        }

        if (q.empty()) {
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
        token_to_order.erase(o_inc->token);
        order_pool.deallocate(o_inc);
    }
}

std::vector<std::string_view> parse_line_to_views(std::string_view line) {
    std::vector<std::string_view> tokens;
    size_t start = 0;
    size_t end = 0;
    while ((end = line.find(',', start)) != std::string::npos) {
        tokens.push_back(line.substr(start, end - start));
        start = end + 1;
    }
    tokens.push_back(line.substr(start));
    return tokens;
}


void simulator::process_message(std::string_view line) {
    auto tokens = parse_line_to_views(line);
    if (tokens.empty()) return;

    std::string_view type = trim(tokens[0]);

    if (type == "O" && tokens.size() >= 7) {
        Order* order = order_pool.allocate();
        order->client_id = parse_int(trim(tokens[1]));
        order->book_id   = parse_int(trim(tokens[2]));
        order->token     = parse_int(trim(tokens[3]));
        order->is_buy    = (trim(tokens[4]) == "B");
        order->quantity  = parse_int(trim(tokens[5]));
        order->price     = parse_int(trim(tokens[6]));
        
        
        order->price_lvl = nullptr;
        order->prev      = nullptr;
        order->next      = nullptr;

        create_order(order);
        
    } else if (type == "X" && tokens.size() >= 3) {

        uint32_t token_to_cancel = parse_int(trim(tokens[2]));
        auto it = token_to_order.find(token_to_cancel);
        if (it == token_to_order.end()) return;
        Order* order = it->second;

        cancel_order(order);
    }
}


void simulator::create_order(Order* order) {
    
    if (order->price < 0 || order->price >= OrderBook::MAX_PRICE) {
        order_pool.deallocate(order);
        return; 
    }

    printf("A, Client %d, Token %u\n", order->client_id, order->token);
    token_to_order[order->token] = order;
    
    if (all_books.find(order->book_id) == all_books.end()) {
        all_books.emplace(order->book_id, OrderBook(token_to_order, order_pool));
    }

    OrderBook& book = all_books.at(order->book_id);
    book.match_process(order);

}


void simulator::cancel_order(Order* order){
    printf("C, Client %d, Token %u\n", order->client_id, order->token);

    if (all_books.count(order->book_id)){
        all_books.at(order->book_id).cancel(order);
    }
    
    token_to_order.erase(order->token);
    order_pool.deallocate(order);
}

void OrderBook::cancel(Order* order){
    if (!order || !order->price_lvl) return;

    order->price_lvl->orders.remove_order(order);

    if (order->price_lvl->orders.empty()){
        if (order->is_buy && order->price == best_bid_pr){
            find_next_best_bid();
        } else if (!order->is_buy && order->price == best_ask_pr){
            find_next_best_ask();
        }
    }
}

void OrderBook::print_state() const{

    if (best_bid_pr != -1){
        for (int i = best_bid_pr; i>=0; --i){
            if (!bids[i].orders.empty()){
                for (Order* o = bids[i].orders.head; o!=nullptr; o=o->next){
                    printf("O, Client %d, Orderbook %d, Token %u, B, %u, %u\n", o->client_id, o->book_id, o->token, o->quantity, o->price);
                }
            }
        }
    }
    if (best_ask_pr != -1){
        for (int i = best_ask_pr; i<MAX_PRICE; ++i){
            if (!asks[i].orders.empty()){
                for (Order* o = asks[i].orders.head; o!=nullptr; o=o->next){
                    printf("O, Client %d, Orderbook %d, Token %u, S, %u, %u\n", o->client_id, o->book_id, o->token, o->quantity, o->price);
                }
            }
        }
    }
}


void simulator::print_fstate() const{
    std::cout<<"\n";
    for (const auto& book : all_books){
        book.second.print_state();
    }
}

/*
matching and executing logic ; we traverse all price levels (the ladder);
for each pricelevel we go through the sorted queue of orders, trying to fulfill the incoming order
anything leftover of incoming is placed onto book, and any used-up resting orders will be removed from book + cleaned up
  got rid of our memory pooling so deleting manually agaub - but its slightly cleaner

*/

int main() {
    simulator sim;
    std::ifstream infile("input_orders.txt");
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