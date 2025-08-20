#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>

struct PriceLevel;

struct Order {
    PriceLevel* price_lvl; //for O(1) erasures
    int client_id;
    uint32_t token;
    bool is_buy;
    uint32_t quantity;
    uint32_t price;
    Order* next = nullptr;
    Order*  prev = nullptr;
};

//an intrusive linked list alternative to let us remove from many index in queue
struct OrderQueue {
    Order* head = nullptr;
    Order* tail = nullptr;

    bool empty() const { return head == nullptr; }

    void push_back(Order* order) {
        order->next = nullptr;
        order->prev = tail;
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
        if (head == nullptr) head->prev = nullptr;
        else tail = nullptr;
        
        return po;
    }

    void remove_order(Order* o) { //O(1)  for K orders in queue as we have prev pointer
        if (o->prev) o->prev->next = o->next;
        else head = o->next;

        if (o->next) o->next->prev = o->prev;
        else tail = o->prev;
    }
};

struct PriceLevel{
    int price;
    OrderQueue orders;
    PriceLevel* prev = nullptr;
    PriceLevel* next = nullptr;
};

/*
orderbook struct for holding heads for O(1) retrieving est bid & offer
if lookup miss , we find where to insert new price level per order type
*/
struct OrderBook
{
    PriceLevel* bids_head = nullptr;
    PriceLevel* asks_head = nullptr;

    PriceLevel* get_create_pricelvl(int price, bool is_buy){

        PriceLevel*& head = is_buy ? bids_head : asks_head;
        PriceLevel* curr = head;
        PriceLevel* prev = nullptr;
        
        while (curr) {
            if (curr->price == price) return curr; 
            
            //insert new level befor i.e higher priority to front respectively - this is our main slowdown O(P)
            bool insert_before = (is_buy && curr->price < price) || (!is_buy && curr->price > price);
            if (insert_before) {
                PriceLevel* new_level = new PriceLevel{price, {}, prev, curr};
                if (prev) 
                    prev->next = new_level;
                else head = new_level;
                
                if (curr) curr->prev = new_level;
                return new_level;
            }
            prev = curr;
            curr = curr->next;
        }
        
        PriceLevel* new_level = new PriceLevel{price, {}, prev, nullptr};
        if (prev) prev->next = new_level; else head = new_level;
        return new_level;
    }
};

OrderBook g_order_book; //single book implementation as i realized instructs said only 1
std::unordered_map<uint32_t, Order*> g_token_to_order;

std::string_view trim(std::string_view sv) {
    sv.remove_prefix(std::min(sv.find_first_not_of(" \t\n\r"), sv.size()));
    sv.remove_suffix(std::min(sv.size() - sv.find_last_not_of(" \t\n\r") - 1, sv.size()));
    return sv;
}

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

//clean called when we erase orders
void cleanup_order(Order* order){ 
    g_token_to_order.erase(order->token);
    
    if (order->price_lvl){ //this should always be true anyway
        order->price_lvl->orders.remove_order(order);
    }
    delete order;
}

/*
matching and executing logic ; we traverse all price levels (the ladder);
for each pricelevel we go through the sorted queue of orders, trying to fulfill the incoming order
anything leftover of incoming is placed onto book, and any used-up resting orders will be removed from book + cleaned up
  got rid of our memory pooling so deleting manually agaub - but its slightly cleaner

*/
void process_order(Order* incoming_o) {
    PriceLevel* cur_level = incoming_o->is_buy ? g_order_book.asks_head : g_order_book.bids_head;
    std::vector<std::pair<int, uint32_t>> executions;

    while (incoming_o->quantity > 0 && cur_level) {
        bool price_match = (incoming_o->is_buy && incoming_o->price >= cur_level->price) || 
                           (!incoming_o->is_buy && incoming_o->price <= cur_level->price);
        if (!price_match) break;
        
        OrderQueue& q = cur_level->orders;
        while (!q.empty() && incoming_o->quantity > 0) {
            Order* resting_ord= q.head;
            uint32_t traded_qty = std::min(incoming_o->quantity, resting_ord->quantity);

            printf("E, Client %d, Token %u, %u, %d\n", resting_ord->client_id, resting_ord->token, traded_qty, cur_level->price);
            
            bool found_ex = false;
            for(auto& ex : executions) if(ex.first == cur_level->price) { ex.second += traded_qty; found_ex = true; break; }
            if(!found_ex) executions.push_back({cur_level->price, traded_qty});

            incoming_o->quantity -= traded_qty;
            resting_ord->quantity -= traded_qty;

            if (resting_ord->quantity == 0) {
                q.pop_front();
                cleanup_order(resting_ord);
            }
        }
        
        PriceLevel* next_level = cur_level->next;
        if (q.empty()) {
            if (cur_level->prev) cur_level->prev->next = cur_level->next; //link before to after us
            else {
                if(incoming_o->is_buy) g_order_book.asks_head = cur_level->next;
                else g_order_book.bids_head = cur_level->next;
            }
            if (cur_level->next) cur_level->next->prev = cur_level->prev;
            delete cur_level;
        }
        cur_level = next_level;
    }

    for (const auto& ex : executions) {
        printf("E, Client %d, Token %u, %u, %d\n", incoming_o->client_id, incoming_o->token, ex.second, ex.first);
    }
    
    if (incoming_o->quantity > 0) { 
        PriceLevel* newlvl = g_order_book.get_create_pricelvl(incoming_o->price, incoming_o->is_buy);
        newlvl->orders.push_back(incoming_o);
    } else {
        g_token_to_order.erase(incoming_o->token);
        delete incoming_o; //no need to use cleanup func as we never placed o on main map
    }
}

void create_order(const std::vector<std::string_view>& tokens) {
    Order* order = new Order{
        .client_id =  parse_int(tokens[1]),
        .token = static_cast<uint32_t>(parse_int(tokens[3])),
        .is_buy = (trim(tokens[4]) == "B"),
        .quantity = static_cast<uint32_t>(parse_int(tokens[5])),
        .price =  static_cast<uint32_t>(parse_int(tokens[6]))
    };
    
    printf("A, Client %d, Token %u\n", order->client_id, order->token);
    g_token_to_order[order->token] = order;
    process_order(order);
}



void cancel_order(const std::vector<std::string_view>& tokens) {
    uint32_t token = static_cast<uint32_t>(parse_int(tokens[2]));
    auto it = g_token_to_order.find(token);
    if (it == g_token_to_order.end()) return;

    Order* order_to_cancel = it->second;
    printf("C, Client %d, Token %u\n", order_to_cancel->client_id, order_to_cancel->token);
    cleanup_order(order_to_cancel);
}

int main() {

    std::ifstream infile("input_orders.txt");
    if (!infile) {
        std::cerr << "cant open file\n";
        return 1;
    }

    std::string line;
    while (getline(infile, line)) {
        if (line.empty()) continue;
        
        std::vector<std::string_view> tokens;
        size_t start = 0;
        size_t end = 0;
        while ((end = line.find(',', start)) != std::string::npos) {
            tokens.push_back(std::string_view(line.data() + start, end - start));
            start = end + 1;
        }
        tokens.push_back(std::string_view(line.data() + start, line.size() - start));

        if (trim(tokens[0]) == "O") create_order(tokens);
        else if (trim(tokens[0])== "X") cancel_order(tokens);
    }

    std::cout << "\n";
    for (PriceLevel* pl = g_order_book.bids_head; pl != nullptr; pl = pl->next) {
        for(Order* o = pl->orders.head; o != nullptr; o = o->next){
            printf("O, Client %d, Orderbook 1, Token %u, B, %u, %u\n", o->client_id, o->token, o->quantity, o->price);
        }
    }
    for (PriceLevel* pl = g_order_book.asks_head; pl != nullptr; pl = pl->next) {
        for(Order* o = pl->orders.head; o != nullptr; o = o->next) {
            printf("O, Client %d, Orderbook 1, Token %u, S, %u, %u\n", o->client_id, o->token, o->quantity, o->price);
        }
    }

    return 0;
}
