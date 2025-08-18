#include <iostream>
#include <fstream>
#include <cstdint>
#include <stdexcept>
#include <sstream>
#include <vector>

struct OrderBook;
struct Order
{
    uint32_t id;
    int client_id;
    short book_id;
    uint32_t token;
    bool is_buy;
    uint32_t quantity;
    uint32_t price;
};




bool cancel(){ 

};

struct OrderPool{

};

struct OrderQueue{


};

struct PriceLevel{

};

struct OrderBook
{

};


void create_order(const auto& tokens){
    
}

bool cancel_order(const auto& tokens){

} 


int parse_int(std::string_view& sv){
    int val =0;
    int sign = 1;
    size_t start = 0;
    if (sv.front() == '-'){
        sign = -1;
        start = 1;
    }
    for (size_t i = start; i<sv.size(); ++i){
        if (sv[i] >= '0' && sv[i] <='9'){
            val = val*10 + (sv[i] - '0');
        }
    }
    return val * sign;
}

bool is_buy(std::string_view sv) { return sv== "B"; }

auto tokenize(std::string& line, char delim){

    std::vector<std::string_view> tokens;
    int start =0, end =0;
    while (end = line.find(delim, start) != std::string::npos){
        tokens.push_back(line.substr(start, end-start));
        start = end +1;
    }
    tokens.push_back(line.substr(start));

    return tokens;
}

void parseOB(std::istream& input){
    std::string line;
    while (getline(input,line)){
        if (line.empty()) continue;
        
        auto tokens = tokenize(line, ',');
        if (tokens.size() <1) continue;
        auto msg_type = tokens[0];
        
        if (msg_type.starts_with('O')){
            create_order(tokens);
        } else if (msg_type.starts_with('C')){
            cancel_order(tokens);
        }
        else throw std::invalid_argument("invalid line");
    }

    return;
}


int main(){
    OrderBook ob;
    std::ifstream infile("input_orders.txt");
    if (!infile) {std::cerr << "cant open  file\n"; return 1;}
    parseOB(infile);
    infile.close();
    std::cout <<"\n";
    
    return 0;
}
