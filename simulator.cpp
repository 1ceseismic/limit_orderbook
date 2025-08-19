#include <iostream>
#include <bits/stdc++.h>
#include <stdexcept>
#include <sstream>

struct OrderBook;
struct Order
{
    uint64_t id;
    int client_id;
    short book_id;
    uint64_t token;
    bool is_buy;
    uint32_t quantity;
    uint32_t price;
};

using optr = std::unique_ptr<Order>;

std::unordered_map<uint64_t, optr> id_index;
uint64_t next_id = 1;

static std::unordered_map<std::string, OrderBook> all_books;  
static std::unordered_map<uint64_t, Order*> token_to_order;  //unclear whether we need multiple orderbook support  due to mmissing ids in  cancellaion input


struct OrderBook
{   
    //price -> price level
    std::map<int, std::deque<optr>, std::greater<int>> bids;
    std::map<int, std::deque<optr>, std::less<int>> asks;
    
    bool cancel_order(uint64_t token_id){ 
        auto token_it = token_to_order.find(token_id);
        if (token_it == token_to_order.end()) return false;
    
        Order* o = std::move(token_it->second);
        if (o->quantity ==0) return false;
        o->quantity = 0;  // inactive now i.e we can skip during checks instead of reallocating
    
        return true;
    }


    void add_order(optr order) {
        if (!order) return;
        auto [it, inserted] = id_index.try_emplace(order->id, std::move(order));
        if (!inserted) return;

        Order* o = it->second.get();
        token_to_order[o->token] = o;
    
        if (o->is_buy) {
            auto& opp = asks;
            
            while (o->quantity > 0 && !opp.empty()) {
                auto best_it = opp.begin();
                int best_price = best_it->first;
    
                if (o->price < best_price) break; //no match
    
                auto& q = best_it->second; //the queue/price level of orders for this price
                int initial_quantity = o->quantity;
                
                while (!q.empty() && o->quantity > 0) {
                    Order* m = q.front().get();
                    if (m->quantity <= 0) {
                        q.pop_front();
                        continue;
                    }
                    
                    int traded = std::min(o->quantity, m->quantity);

                    printf("E, Client %d, Token %ld, %d, %d\n", m->client_id, m->token, traded, best_price);
    
                    o->quantity -= traded;
                    m->quantity -= traded;
    
                    if (m->quantity == 0) {
                        q.pop_front();
                    }
                }

                if (initial_quantity > o->quantity) {
                    int total_traded = initial_quantity - o->quantity;
                    printf("E, Client %d, Token %ld, %d, %d\n", o->client_id, o->token, total_traded, best_price);
                }
            }
            
            if (o->quantity > 0) {
                bids[o->price].push_back(std::move(it->second));
            } else {
                id_index.erase(it);
                token_to_order.erase(o->token);
            }
            
        } else { //handle sell, match against bids 
            auto& opp = bids;
    
            while (o->quantity > 0 && !opp.empty()) {
                auto best_it = opp.begin();
                int best_price = best_it->first;
    
                if (o->price > best_price) break; // No match
                
                auto& q = best_it->second;
                int initial_quantity = o->quantity;
    
                while (!q.empty() && o->quantity > 0) {
                    Order* m = q.front().get();
                    if (m->quantity <= 0) {
                        q.pop_front();
                        continue;
                    }
                    int traded = std::min(o->quantity, m->quantity);
                    
                    printf("E, Client %d, Token %ld, %d, %d\n", m->client_id, m->token, traded, best_price); //resting order
    
                    o->quantity -= traded;
                    m->quantity -= traded;
                    
                    if (m->quantity == 0) {
                        q.pop_front();
                    }
                }
                //after all trades at this price ; print incoming order
                if (initial_quantity > o->quantity) {
                    int total_traded = initial_quantity - o->quantity;
                    printf("E, Client %d, Token %ld, %d, %d\n", o->client_id, o->token, total_traded, best_price);
                }
            }
    
            //leftover for book
            if (o->quantity > 0) {
                asks[o->price].push_back(std::move(it->second));
            } else { //fully executed so we can rm
                id_index.erase(it);
                token_to_order.erase(o->token);
            }
        }
    }
    
};


inline uint64_t extractId(const std::string& s) {
    size_t num_pos = s.find_last_of(' ');
    if (num_pos == std::string::npos || num_pos + 1 >= s.size()) {
        throw std::invalid_argument("invalid id format " + s);
    }
    return std::stoull(s.substr(num_pos + 1));
}

inline std::string trim(const std::string& s){
    auto start = s.find_first_not_of(" \t");
    if (start == std::string::npos) return "";

    auto end = s.find_last_not_of(" \t");
    return s.substr(start, end - start + 1);
}


void parseOB(std::istream& input, OrderBook& ob){
    using namespace std;
    string line;

    while (getline(input, line)){
        if (line.empty()) continue;

        std::istringstream sst(line);
        vector<string> tokens;
        string token;

        while (std::getline(sst, token, ',')) tokens.push_back(trim(token));
        if (tokens.empty()) continue;
        if (tokens[0]  == "O" && tokens.size() == 7){
            string book_name = tokens[2];
            OrderBook& ob = all_books[book_name];
            Order order{
                .id =  next_id++,
                .client_id = extractId(tokens[1]),
                .book_id = extractId(book_name),
                .token = extractId(tokens[3]),
                .is_buy = (tokens[4] == "B"),
                .quantity =std::stoul(tokens[5]),
                .price = std::stoul(tokens[6]),
            };            
            printf("A, Client %d, Token %d\n", order.client_id, order.token);

            token_to_order[order.token] = &order;

            if (order.is_buy) {
                ob.add_order(std::make_unique<Order>(order)); 
            } else { 
                ob.add_order(std::make_unique<Order>(order)); 
            }

        } else if ((tokens[0] == "X" || tokens[0] == "C") && tokens.size() ==3){

            uint64_t tok_cancel = extractId(tokens[2]);
            auto it = token_to_order.find(tok_cancel);
            
            if (it != token_to_order.end()){
                Order* c = it->second;
                
                if (ob.cancel_order(c->id)){
                    cout <<"C, " <<tokens[1]<<", "<<tokens[2]<<"\n";
                }      
            }
            
        } else {
            throw std::invalid_argument("invalid line format");
        }
    }
    return;
}


int main(){
    OrderBook ob;
    std::ifstream infile("input_orders.txt");
    if (!infile) {std::cerr << "cant open  file\n"; return 1;}

    parseOB(infile, ob);
    std::cout <<"\n";
    
    for (auto& [bname, _ob] :  all_books){
        for (auto& pricelevel : _ob.bids){

            for (optr& o : pricelevel.second){
                if (o->quantity > 0) {//not fully executed
                    char type = o->is_buy ? 'B' : 'S';
                    printf("O, Client %d, %s, Token %ld, %c, %ld, %ld \n", o->client_id, o->book_id, o->token, type, o->quantity, o->price);
                }
            }
        }

        for (auto& pricelevel : _ob.asks){
            for (optr& o : pricelevel.second){
                if (o->quantity > 0) {//not fully executed
                    char type = o->is_buy ? 'B' : 'S';
                    printf("O, Client %d, Orderbook %d, Token %ld, %c, %ld, %ld \n", o->client_id, o->book_id, o->token, type, o->quantity, o->price);
                }
            }
        }

    }
    return 0;
}
