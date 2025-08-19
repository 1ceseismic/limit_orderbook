#include <iostream>
#include <fstream>
#include <cstdint>
#include <stdexcept>
#include <sstream>
#include <vector>
#include <unordered_map>
#include <string>

struct OrderBook;
struct PriceLevel;
struct OrderQueue;
struct OrderPool;
struct PriceLevelPool;


struct Order
{
    uint32_t id;
    int client_id;
    uint32_t book_id;
    uint32_t token;
    bool is_buy;
    uint32_t quantity;
    uint32_t price;
    Order* next = nullptr;
};

struct OrderQueue {
    Order* head = nullptr;
    Order* tail = nullptr;

    bool empty() const { return head == nullptr; }

    void push_back(Order* order) {
        order->next = nullptr;
        if (empty()) {
            head = order;
            tail = order;
        } else {
            tail->next = order;
            tail = order;
        }
    }

    Order* pop_front(){
        if (empty()) return nullptr;
        Order* po = head;
        head = head->next;
        if (head == nullptr) {
            tail = nullptr;
        }
        return po;
    }

    void remove_order(Order* order_to_remove) {
        if (head == order_to_remove) {
            pop_front();
            return;
        }
        Order* current = head;
        while (current && current->next != order_to_remove) {
            current = current->next;
        }
        if (current && current->next) {
            current->next = order_to_remove->next;
            if (order_to_remove == tail) {
                tail = current;
            }
        }
    }
};

struct PriceLevel{
    int price;
    OrderQueue orders;
    PriceLevel* prev = nullptr;
    PriceLevel* next = nullptr;
};


const int POOL_CAP = 10000; //one of the arbitrary assumptions

struct OrderPool
{
    Order pool[POOL_CAP];
    Order* free_head = nullptr;

    OrderPool() {
        for (int i = 0; i < POOL_CAP - 1; ++i) {
            pool[i].next = &pool[i+1];
        }
        pool[POOL_CAP - 1].next = nullptr;
        free_head = &pool[0];
    }
    
    Order* allocate(){
        if (!free_head) return nullptr;
        Order* alloc_order = free_head;
        free_head = free_head->next;
        return alloc_order;
    }

    //called on fully executed / cancelled ; we insert at beginning again (cache friendly)
    void deallocate(Order* order){
        order->next = free_head;
        free_head = order;
    }
};

struct PriceLevelPool{
    PriceLevel pool[POOL_CAP];
    PriceLevel* free_head = nullptr;

    PriceLevelPool(){
        for (int i = 0; i < POOL_CAP - 1; ++i) {
            pool[i].next = &pool[i+1];
        }
        pool[POOL_CAP - 1].next = nullptr;
        free_head = &pool[0];
    }

    PriceLevel* allocate(){
        if (!free_head) return nullptr;
        PriceLevel* allocated_lvl = free_head;
        free_head = free_head->next;
        allocated_lvl->prev = nullptr;
        allocated_lvl->next = nullptr;
        allocated_lvl->orders.head = nullptr; // Ensure new levels are clean
        allocated_lvl->orders.tail = nullptr;
        return allocated_lvl;
    }

    void deallocate(PriceLevel* level) {
        level->next = free_head;
        free_head = level;
    }
};


OrderPool g_order_pool;
PriceLevelPool g_price_level_pool;

std::unordered_map<uint32_t, std::string> g_bookid_to_name;
std::unordered_map<std::string, uint32_t> g_bookname_to_id;
std::unordered_map<uint32_t, OrderBook> g_all_books;
std::unordered_map<uint32_t, Order*> g_token_to_order;

uint32_t g_next_book_id = 0;
uint32_t g_next_id = 1;

int parse_int(const std::string_view& sv) {
    if (sv.empty()) return 0;
    int val = 0;
    bool found_digit = false;
    for (char c : sv) {
        if (c >= '0' && c <= '9') {
            val = val * 10 + (c - '0');
            found_digit = true;
        } else if (found_digit) {
            break;
        }
    }
    return val;
}

std::vector<std::string_view> tokenize(const std::string& line, char delim){
    std::vector<std::string_view> tokens;
    size_t start = 0;
    size_t end = 0;
    while ((end = line.find(delim, start)) != std::string::npos) {
        tokens.push_back(std::string_view(line.data() + start, end - start));
        start = end + 1;
    }
    tokens.push_back(std::string_view(line.data() + start, line.size() - start));
    return tokens;
}

std::string_view trim(std::string_view sv) {
    size_t first = sv.find_first_not_of(" \t\n\r");
    if (std::string::npos == first) {
        return sv.substr(0, 0);
    }
    size_t last = sv.find_last_not_of(" \t\n\r");
    return sv.substr(first, (last - first + 1));
}

struct OrderBook
{
    PriceLevel* bids_head = nullptr;
    PriceLevel* asks_head = nullptr;

    PriceLevel* create_insert_pl(int price, PriceLevel* prev_level, PriceLevel* next_level) {
        PriceLevel* new_level = g_price_level_pool.allocate();
        if (!new_level) return nullptr;

        new_level->price = price;
        new_level->prev = prev_level;
        new_level->next = next_level;

        if (prev_level) {
            prev_level->next = new_level;
        }
        if (next_level) {
            next_level->prev = new_level;
        }
        return new_level;
    }

    PriceLevel* find_pricelvl(int price, bool is_buy) {
        PriceLevel* curr = is_buy ? bids_head : asks_head;
        while (curr) {
            if (curr->price == price) return curr;
            if ((is_buy && curr->price < price) || (!is_buy && curr->price > price)) break;
            curr = curr->next;
        }
        return nullptr;
    }

    PriceLevel* get_create_pricelvl(int price, bool is_buy){
        PriceLevel* curr = is_buy ? bids_head : asks_head;
        PriceLevel* prev = nullptr;
        
        while (curr) {
            if (curr->price == price) return curr;
            
            if (is_buy && curr->price < price) {
                PriceLevel* new_level = create_insert_pl(price, prev, curr);
                if (!prev) bids_head = new_level;
                return new_level;
            }

            if (!is_buy && curr->price > price) {
                PriceLevel* new_level = create_insert_pl(price, prev, curr);
                if (!prev) asks_head = new_level;
                return new_level;
            }

            prev = curr;
            curr = curr->next;
        }
        
        PriceLevel* new_level = create_insert_pl(price, prev, nullptr);
        if (!prev) {
            if (is_buy) bids_head = new_level;
            else asks_head = new_level;
        }
        return new_level;
    }
};

void cleanup_order(Order* order){
    g_token_to_order.erase(order->token);
    
    OrderBook& ob = g_all_books.at(order->book_id);
    PriceLevel* pl = ob.find_pricelvl(order->price, order->is_buy);
    if(pl){
        pl->orders.remove_order(order);
    }
    g_order_pool.deallocate(order);
}

void process_order(Order* incoming_o) {
    OrderBook& ob = g_all_books.at(incoming_o->book_id);
    PriceLevel* cur_level = incoming_o->is_buy ? ob.asks_head : ob.bids_head;
    std::vector<std::pair<int, uint32_t>> executions;

    while (incoming_o->quantity > 0 && cur_level) {
        if (incoming_o->is_buy && incoming_o->price < cur_level->price ||
         !incoming_o->is_buy && incoming_o->price > cur_level->price) {
            break; //no price
         }
        
 
        OrderQueue& q = cur_level->orders;
        
        while (!q.empty() && incoming_o->quantity > 0) {
            Order* resting_order = q.head;
            
            uint32_t traded_qty = std::min(incoming_o->quantity, resting_order->quantity);

            printf("E, Client %d, Token %u, %u, %d\n", resting_order->client_id, resting_order->token, traded_qty, cur_level->price);
            
            bool found_ex = false;
            for(auto& ex : executions){
                if(ex.first == cur_level->price){
                    ex.second += traded_qty;
                    found_ex = true;
                    break;
                }
            }
            if(!found_ex){
                executions.push_back({cur_level->price, traded_qty});
            }

            incoming_o->quantity -= traded_qty;
            resting_order->quantity -= traded_qty;

            if (resting_order->quantity == 0) {
                q.pop_front();
                cleanup_order(resting_order);
            }
        }
        
        PriceLevel* next_level = cur_level->next;
        if (q.empty()) {
            if (cur_level->prev) {
                cur_level->prev->next = cur_level->next;
            } else {
                if(incoming_o->is_buy) ob.asks_head = cur_level->next;
                else ob.bids_head = cur_level->next;
            }
            if (cur_level->next) {
                cur_level->next->prev = cur_level->prev;
            }
            g_price_level_pool.deallocate(cur_level);
        }
        cur_level = next_level;
    }

    uint32_t total_executed_qty = 0;
    for (const auto& ex : executions) {
        total_executed_qty += ex.second;
        printf("E, Client %d, Token %u, %u, %d\n", incoming_o->client_id, incoming_o->token, ex.second, ex.first);
    }
    
    if (incoming_o->quantity > 0) { 
        PriceLevel* resting_level = ob.get_create_pricelvl(incoming_o->price, incoming_o->is_buy);
        resting_level->orders.push_back(incoming_o);
    } else {
        g_token_to_order.erase(incoming_o->token);
        g_order_pool.deallocate(incoming_o);
    }
}


void create_order(const std::vector<std::string_view>& tokens) {
    if (tokens.size() != 7) return;

    std::string book_name(trim(tokens[2]));
    uint32_t cur_book_id;
    auto it = g_bookname_to_id.find(book_name);
    if (it == g_bookname_to_id.end()) {
        cur_book_id = g_next_book_id++;
        g_bookname_to_id[book_name] = cur_book_id;
        g_bookid_to_name[cur_book_id] = book_name;
        g_all_books[cur_book_id] = OrderBook();
    } else {
        cur_book_id = it->second;
    }

    Order* order = g_order_pool.allocate();
    if (!order) return;

    order->id = g_next_id++;
    order->client_id = parse_int(tokens[1]);
    order->book_id = cur_book_id;
    order->token = static_cast<uint32_t>(parse_int(tokens[3]));
    order->is_buy = (trim(tokens[4]) == "B");
    order->quantity = static_cast<uint32_t>(parse_int(tokens[5]));
    order->price = static_cast<uint32_t>(parse_int(tokens[6]));  
    
    printf("A, Client %d, Token %u\n", order->client_id, order->token);
    g_token_to_order[order->token] = order;
    
    process_order(order);
}

void cancel_order(const std::vector<std::string_view>& tokens){
    if (tokens.size() != 3) return;
    uint32_t token = static_cast<uint32_t>(parse_int(tokens[2]));
    auto it = g_token_to_order.find(token);
    if (it == g_token_to_order.end()) return;

    Order* order_to_cancel = it->second;
    printf("C, Client %d, Token %u\n", order_to_cancel->client_id, order_to_cancel->token);
    cleanup_order(order_to_cancel);
}


void parseOB(std::istream& input){
    std::string line;
    while (getline(input, line)) {
        if (line.empty() || line =="\r") continue;
        
        auto tokens = tokenize(line, ',');
        if (tokens.empty()) continue;
        std::string_view type = trim(tokens[0]);

        if (type == "O") {
            create_order(tokens);
        } else if (type == "X") {
            cancel_order(tokens);
        }
    }
}

void printOB() {
    std::cout << "\n";

    for (auto const& [book_id, ob] : g_all_books) {
        PriceLevel* current_bid_level = ob.bids_head;
        while (current_bid_level) {
            Order* current_order = current_bid_level->orders.head;
            while (current_order) {
                if (current_order->quantity > 0) {
                    std::string book_name = g_bookid_to_name.at(current_order->book_id); //i dont like doing this ..
                    printf("O, Client %d, %s, Token %u, B, %u, %u\n", current_order->client_id, book_name.c_str(), current_order->token,
                           current_order->quantity, current_order->price);
                }
                current_order = current_order->next;
            }
            current_bid_level = current_bid_level->next;
        }

        PriceLevel* current_ask_level = ob.asks_head;
        while (current_ask_level) {
            Order* current_order = current_ask_level->orders.head;
            while (current_order) {
                if (current_order->quantity > 0) {
                    std::string book_name = g_bookid_to_name.at(current_order->book_id);
                    printf("O, Client %d, %s, Token %u, S, %u, %u\n",
                           current_order->client_id, book_name.c_str(), current_order->token,
                           current_order->quantity, current_order->price);
                }
                current_order = current_order->next;
            }
            current_ask_level = current_ask_level->next;
        }
    }
}

int main() {
    std::ifstream infile("input_orders.txt");
    if (!infile) {
        std::cerr << "cant open file\n";
        return 1;
    }
    parseOB(infile);
    printOB();

    infile.close();
    return 0;
}