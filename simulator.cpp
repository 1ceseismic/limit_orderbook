#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <map>
#include <unordered_map>
#include <algorithm>
#include <vector>
#include <list>

struct PriceLevel;

struct Order {
    std::string client_id;
    std::string token;
    bool is_buy; 
    int quantity;
    int price;
    std::list<Order*>::iterator q_pos; //avoid O(k) search in level ; we go straight to order position in o(1)
    PriceLevel* price_lvl; //avoid O(logn) search in std::map lookup

    Order(const std::string& cid, const std::string& tok, char s, int qty, int p) 
        : client_id(cid), token(tok), is_buy(s == 'B'), quantity(qty), price(p), q_pos(), price_lvl(nullptr) {}
};

struct PriceLevel {
    std::list<Order*> orders;

    void add_order(Order* order){ 
        orders.push_back(order);
        order->q_pos = std::prev(orders.end());
        order->price_lvl = this;
     }
    
    void remove_order(Order* order) {
        orders.erase(order->q_pos);
        order->price_lvl = nullptr;
    }
    
    bool empty() const { return orders.empty(); }
    Order* front() { return orders.empty() ? nullptr : orders.front(); }

    void pop_front() {
         if (!orders.empty()) {
            orders.front()->price_lvl = nullptr;
            orders.pop_front(); 
        }
    }
};

class OrderBook {
private:
    PriceLevel* best_bid = nullptr;  
    PriceLevel* best_ask = nullptr;  
    std::map<int, PriceLevel, std::greater<int>> bids; 
    std::map<int, PriceLevel> asks;  
    std::unordered_map<std::string, Order*> token_to_order;
    
    void update_best_bid() {
        best_bid = bids.empty() ? nullptr : &(bids.begin()->second);
    }
    
    void update_best_ask() {
        best_ask = asks.empty() ? nullptr : &(asks.begin()->second);
    }
    

public:
    void add_order(Order* order) {
        token_to_order[order->token] = order;
        
        std::cout << "A, " << order->client_id << ", " << order->token << "\n";
        
        match_order(order); 
        //we matched, then cleanup rest of remaining for that order
        if (order->quantity > 0){
            if (order->is_buy){
                bids[order->price].add_order(order);
                update_best_bid();
            }
            else {
                asks[order->price].add_order(order);
                update_best_ask();
            }
        }
    }
    
    void cancel_order(const std::string& client_id, const std::string& token) {
        auto it = token_to_order.find(token);
        if (it == token_to_order.end()) return;
        
        Order* order = it->second;
        
        if (order->price_lvl){
            PriceLevel* level = order->price_lvl;
            int price = order->price;
            bool is_buy = order->is_buy;

            level->remove_order(order);
            
            if(level->empty()){
                if (is_buy){
                    bids.erase(price);
                    update_best_bid();
                } else{
                    asks.erase(price);
                    update_best_ask();
                }
            }
        }
        
        token_to_order.erase(it);
        std::cout << "C, " << client_id << ", " << token << "\n";
        delete order;
        
    }
    
    void print_rem() const {
        for (const auto& [price, level] : bids) {
            for (const auto& order : level.orders) {
                std::cout << "O, " << order->client_id << ", Orderbook 1, " << order->token << ", B, " << order->quantity << ", " << order->price << "\n";
            }
        }
        
        for (const auto& [price, level] : asks) {
            for (const auto& order : level.orders) {
                std::cout << "O, " << order->client_id << ", Orderbook 1, " << order->token << ", S, "
                 << order->quantity << ", " << order->price << "\n";
            }
        }
    }
    
private:
void match_order(Order* inc_order) {

    int executed_qty_tot = 0;
    int last_tp = 0;

    while (inc_order->quantity > 0 && (inc_order->is_buy ? best_ask != nullptr : best_bid != nullptr)) {

        PriceLevel* opp_lvl = inc_order->is_buy ? best_ask : best_bid;
        int opp_price = opp_lvl->front()->price;

        bool prices_cross = inc_order->is_buy ? (inc_order->price >= opp_price) : (inc_order->price <= opp_price);  
        if (!prices_cross) break;
        
        
        // Match with all orders at this price level
        while (inc_order->quantity > 0 && !opp_lvl->empty()) {

            Order* resting_o = opp_lvl->front();
            int trade_qty = std::min(inc_order->quantity, resting_o->quantity);
            int trade_price = resting_o->price;

            std::cout << "E, " << resting_o->client_id << ", " << resting_o->token << ", " << trade_qty << ", " << trade_price << "\n";

            executed_qty_tot += trade_qty;
            last_tp = trade_price;

            inc_order->quantity -= trade_qty;
            resting_o->quantity -= trade_qty;
            
            // we remove any fully-used resting orders
            if (resting_o->quantity == 0) {
                opp_lvl->pop_front();
                token_to_order.erase(resting_o->token);
                delete resting_o;
            }
        }
        
        //remove empty price level, then we update the next best level in books state
        if (opp_lvl->empty()) {
            if (inc_order->is_buy) {
                asks.erase(opp_price);
                update_best_ask();
            } else {
                bids.erase(opp_price);
                update_best_bid();
            }
        }
         
    }
    if (executed_qty_tot > 0){
        std::cout <<"E, "<<inc_order->client_id<<", "<<inc_order->token<<", "<<executed_qty_tot<<", "<< last_tp<<"\n";
    }

}
};


std::vector<std::string> parse_line(const std::string& line) {
    std::vector<std::string> tokens;

    size_t start_pos =0;
    const std::string whitepsace = " \t";
    
    while (start_pos < line.length()) {
        size_t end_pos = line.find(',', start_pos);

        if (end_pos == std::string::npos) {
            end_pos = line.length();
        }

        size_t token_start = line.find_first_not_of(whitepsace, start_pos);

        if (token_start != std::string::npos && token_start  < end_pos){
            size_t token_end = line.find_last_not_of(whitepsace, end_pos-1);

            tokens.push_back(line.substr(token_start, token_end - token_start  + 1));
        }

        start_pos = end_pos+1;
    }
    return tokens;
}

int main(int argc, char* argv[]) {
    std::string filename = argc > 1 ? argv[1] : "input_orders.txt";
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "file error\n";
        return 1;
    }
    
    OrderBook orderbook;
    std::string line;
    
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        
        auto tokens = parse_line(line);
        if (tokens.empty()) continue;
        
        if (tokens[0] == "O" && tokens.size() >= 7) {
            Order* order = new Order(tokens[1], tokens[3], tokens[4][0], std::stoi(tokens[5]), std::stoi(tokens[6]));
            orderbook.add_order(order);
            
        } else if (tokens[0] == "X" && tokens.size() >= 3) {
            orderbook.cancel_order(tokens[1], tokens[2]);
        }
    }
    
    std::cout << "\n"; 
    orderbook.print_rem();
    
    return 0;
}