#include <iostream>
#include <fstream>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <map>
#include <cstdint>
#include <memory>
#include <string>
#include <set>
#include <sstream> 

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

struct OrderPool{
    std::vector<Order> mem_block;
    std::vector<uint32_t> free_list;

    OrderPool(size_t size) {
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
    std::unordered_map<int, OrderQueue> bid_levels;
    std::set<int, std::greater<int>> bid_prices;
    std::unordered_map<int, OrderQueue> ask_levels;
    std::set<int> ask_prices;
    OrderQueue* best_bid_q = nullptr;
    OrderQueue* best_ask_q = nullptr;

    void match_process (Order*, auto&, OrderPool&, std::stringstream&);
    void cancel (Order* o_to_cancel);
    void update_best_bid();
    void update_best_ask();
    void add_to_book(Order* o);

    const std::set<int, std::greater<int>>& get_bid_prices() const { return bid_prices; }
    const std::set<int>& get_ask_prices() const { return ask_prices; }
    OrderQueue* get_bid_q(int price) const;
    OrderQueue* get_ask_q(int price) const;
};

OrderQueue* OrderBook::get_bid_q(int price) const {
    auto it = bid_levels.find(price);
    if (it != bid_levels.end()) return const_cast<OrderQueue*>(&it->second);
    return nullptr;
}

OrderQueue* OrderBook::get_ask_q(int price) const {
    auto it = ask_levels.find(price);
    if (it != ask_levels.end()) return const_cast<OrderQueue*>(&it->second);
    return nullptr;
}

void OrderBook::update_best_bid(){
    best_bid_q = bid_prices.empty() ? nullptr : &bid_levels[*bid_prices.begin()];
}

void OrderBook::update_best_ask(){
    best_ask_q = ask_prices.empty() ? nullptr : &ask_levels[*ask_prices.begin()];
}

void OrderBook::add_to_book(Order* o){
    if (o->is_buy){
        bid_levels[o->price].push_back(o);
        bid_prices.insert(o->price);
        update_best_bid(); 
    } else{
        ask_levels[o->price].push_back(o);
        ask_prices.insert(o->price);
        update_best_ask();   
    } 
}



class simulator {
    std::unordered_map<uint32_t, Order*> token_to_order;
    OrderPool order_pool;
    std::map<int, OrderBook> all_books;
    std::stringstream out_buffer;
public:
    simulator( size_t pool_size = 11000) : order_pool(pool_size) {};
    void process_msg(const std::string_view& line);
    void cancel_order(Order* order);
    void create_order(Order* order);
    void print_fstate(std::ostream& out) const;
    void res_to_file(const std::string& filename) const;
};


/*
matching and executing logic ; we traverse all price levels (the ladder);
for each pricelevel we go through the sorted queue of orders, trying to fulfill the incoming order
anything leftover of incoming is placed onto book, and any used-up resting orders will be removed from book + cleaned up
  got rid of our memory pooling so deleting manually agaub - but its slightly cleaner

*/
void OrderBook::match_process(Order* incoming_o, auto& token_map, OrderPool& pool, std::stringstream& out_buffer) {
    std::map<int, uint32_t> execs;

    auto match_engine = [&](OrderQueue*& best_q) {
        while (incoming_o->quantity > 0 && best_q && best_q->head) { 
            Order* resting_ord = best_q->head; 

            int trade_price = resting_ord->price;
            bool prices_cross = incoming_o->is_buy ? (incoming_o->price >= trade_price) : (incoming_o->price <= trade_price);
            if (!prices_cross) break;

            while (resting_ord && incoming_o->quantity > 0) { // actually traverse the level
                Order* rest_next = resting_ord->next;

                // if (incoming_o->client_id == resting_ord->client_id){ 
                //     resting_ord = rest_next; 
                //     continue; 
                // }
                uint32_t traded_qty = std::min(incoming_o->quantity, resting_ord->quantity);
                out_buffer << "E, Client " << resting_ord->client_id << ", Token " << resting_ord->token << ", " << traded_qty << ", " << resting_ord->price << "\n";
                
                execs[trade_price] += traded_qty; 
                incoming_o->quantity -= traded_qty;
                resting_ord->quantity -= traded_qty;

                if (resting_ord->quantity == 0) {
                    best_q->remove_order(resting_ord); 
                    token_map.erase(resting_ord->token);
                    pool.deallocate(resting_ord);
                }

                resting_ord = rest_next; 
            }
            
            if (best_q->empty()) {
                if (incoming_o->is_buy) { 
                    ask_levels.erase(trade_price);
                    ask_prices.erase(trade_price);
                } else { 
                    bid_levels.erase(trade_price);
                    bid_prices.erase(trade_price);
                }
            }
            
            if (incoming_o->quantity > 0) { 
                if (incoming_o->is_buy) { 
                    update_best_ask(); 
                } else { 
                    update_best_bid();
                }
            } else break; //full fulfilled 
        }
    };

    if (incoming_o->is_buy) match_engine(best_ask_q);
    else match_engine(best_bid_q);
    
    for (const auto& entry : execs) {
        out_buffer << "E, Client " << incoming_o->client_id << ", Token " << incoming_o->token << ", " << entry.second << ", " << entry.first << "\n";
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
            bid_levels.erase(order->price);
            bid_prices.erase(order->price);
            update_best_bid();
        } else {
            ask_levels.erase(order->price);
            ask_prices.erase(order->price);
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
            std::cerr << "order pool exhausted\n";
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
    
    out_buffer<<"C, Client "<<order->client_id<< ", Token "<<order->token<<"\n";

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
    out_buffer<< "A, Client " << order->client_id <<", Token " << order->token<<"\n";
    
    token_to_order[order->token] = order;
    all_books[order->book_id].match_process(order, token_to_order, order_pool, out_buffer);
}


void simulator::print_fstate(std::ostream& out) const {
    out << "\n";
    for (const auto& [book_id, book] : all_books) {
        for (int price : book.get_bid_prices()) {
            if (OrderQueue* queue = book.get_bid_q(price)) {
                for (Order* o = queue->head; o != nullptr; o = o->next) {
                    out<<"O, Client "<<o->client_id<<", Orderbook "<<book_id<<", Token "<< o->token<<", B, " << o->quantity << ", " << o->price << "\n";
                }
            }
        }
        for (int price : book.get_ask_prices()) {
            if (OrderQueue* queue = book.get_ask_q(price)) {
                for (Order* o = queue->head; o != nullptr; o = o->next) {
                    out << "O, Client "<<o->client_id<< ", Orderbook " << book_id << ", Token "<<o->token << ", S, " << o->quantity<< ", " << o->price << "\n";
                }
            }
        }
    }
}

void simulator::res_to_file(const std::string& filename) const {
    std::ofstream outfile(filename);
    if (!outfile) {
        std::cerr << "cant open output file "<<filename;
        return;
    }
    
    outfile << out_buffer.str();
    print_fstate(outfile);
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

    sim.res_to_file("mapLL_results.txt");

    std::ifstream resultFile("mapLL_results.txt");
        while (getline(resultFile, line)) {
            std::cout << line << std::endl;
        }
        resultFile.close();
    return 0;
}
