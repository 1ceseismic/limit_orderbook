#include <iostream>
#include <bits/stdc++.h>
#include <stdexcept>
#include <sstream>


struct OrderBook;

struct Order
{
    int client_id;
    int book_id;
    uint64_t token;
    bool is_buy;
    uint32_t quantity;
    uint32_t price;
};

struct OrderBook
{

    std::deque<Order> orders;
    std::unordered_map<uint64_t, Order*> id_index;
    std::vector<std::string> events;

    bool cancel(uint64_t id){ //well cancel order id in O(1)
        Order* o= id_index[id];
        std::cout <<"cancel\n";
        if (o == nullptr) return false;
        o->quantity = 0;
        return true;
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
            cout <<"tokens[1]: "<<tokens[1]<<"\n";
            Order order{
                .client_id = extractId(tokens[1]),
                .book_id = extractId(tokens[2]),
                .token = extractId(tokens[3]),
                .is_buy = (tokens[4] == "B"),
                .quantity = static_cast<uint32_t>(std::stoul(tokens[5])),
                .price = static_cast<uint32_t> (std::stoul(tokens[6])),
            };

        } else if ((tokens[0] == "X" || tokens[0] == "C") && tokens.size() ==3){
            ob.cancel(extractId(tokens[2]));
        } else {
            throw std::invalid_argument("invalid line format");
        }
    }

}


int main(){
    OrderBook ob;
    std::ifstream infile("input_orders.txt");
    if (!infile) {std::cerr << "cant open input file\n"; return 1;}


    parseOB(infile, ob); //will parse whole file as one

    
    return 0;
}
