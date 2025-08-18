#include <iostream>
#include <bits/stdc++.h>
#include <stdexcept>
#include <sstream>

struct OrderBook;
struct Order
{
    uint64_t id;
    int client_id;
    int book_id;
    uint64_t token;
    bool is_buy;
    uint32_t quantity;
    uint32_t price;
};


std::unordered_map<uint64_t, Order*> id_index;
uint64_t next_id = 1;
std::deque<Order> orders;

static std::unordered_map<std::string, OrderBook> all_books;  
static std::unordered_map<uint64_t, Order*> token_to_order;  //unclear whether we need multiple orderbook support  due to mmissing ids in  cancellaion input


bool cancel(uint64_t token_id){ 
    auto token_it = token_to_order.find(token_id);
    if (token_it == token_to_order.end()) return false;

    Order* o = token_it->second;
    if (o->quantity ==0) return false;
    o->quantity = 0;  // inactive now i.e we can skip during checks instead of reallocating

    return true;
}


struct OrderBook
{
    std::map<int, std::deque<Order*>, std::greater<int>> bids;
    std::map<int, std::deque<Order*>, std::less<int>> asks;
    

    void add_order(Order order, auto& opp){

        orders.push_back(order);
        Order* o = &orders.back();
        id_index[o->id] = o;
        bool is_buy = o->is_buy;
        uint32_t total_qty;

        
        //now we try matching
        while (o->quantity > 0 && !opp.empty()){
            auto best_it  = opp.begin();
            int best_price = best_it->first;
            if ((is_buy && o->price < best_price) || !is_buy && o->price > best_price) break;  //no match

            //loop through quantities to fullfill bid
            auto& q = best_it ->second; //q is queue/level of orders for that price 
            while(!q.empty() && o->quantity > 0){

                Order* m = q.front();
                if (!(m->quantity > 0)) {
                    q.pop_front(); 
                    id_index.erase(m->id); 
                    continue;
                }

                int traded = std::min(o->quantity, m->quantity);
                o->quantity -=traded;
                m->quantity -= traded;
                printf("E, Client %d, Token %d, %d, %d\n", m->client_id, m->token, traded, best_price); //resting order

                if (m->quantity ==0) {
                    q.pop_front();
                    id_index.erase(m->id);
                }

                printf("E, Client %d, Token %ld, %d, %d\n", o->client_id, o->token, traded, best_price);

            }

        }

        //leftover for book
        if (o->quantity > 0){
            if (is_buy) bids[o->price].push_back(o);
            else asks[o->price].push_back(o);
        }  
        else{
            id_index.erase(o->id); 
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
                .token = extractId(tokens[3]),
                .is_buy = (tokens[4] == "B"),
                .quantity =std::stoul(tokens[5]),
                .price = std::stoul(tokens[6]),
            };            
            printf("A, Client %d, Token %d\n", order.client_id, order.token);

            token_to_order[order.token] = &order;

            if (order.is_buy) {
                ob.add_order(order, ob.asks); 
            } else { 
                ob.add_order(order, ob.bids); 
            }

        } else if ((tokens[0] == "X" || tokens[0] == "C") && tokens.size() ==3){

            uint64_t tok_cancel = extractId(tokens[2]);
            auto it = token_to_order.find(tok_cancel);
            
            if (it != token_to_order.end()){
                Order* cancel_order = it->second;
                
                if (cancel(cancel_order->id)){
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
    if (!infile) {std::cerr << "cant open input file\n"; return 1;}

    parseOB(infile, ob);
    std::cout <<"O: {order book status} \n";
    
    return 0;
}
