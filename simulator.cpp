#include <iostream>
#include <fstream>
#include <cstdint>
#include <stdexcept>
#include <sstream>
#include <vector>
#include <unordered_map>

struct OrderPool;
OrderPool orderpool;

std::unordered_map<uint32_t, std::string> bookid_to_name;
std::unordered_map<std::string, uint32_t> bookname_to_id;
std::unordered_map<uint32_t, OrderBook> all_books;
std::unordered_map<uint32_t, Order*> token_to_order;

uint32_t next_book_id = 0;
uint32_t next_id = 1;


struct OrderBook;
struct Order
{
    uint32_t id;
    int client_id;
    uint32_t book_id;
    uint32_t token;
    bool is_buy;
    uint32_t quantity;
    uint32_t price;
    Order* next_in_q = nullptr; //only singly since we push_back and pop_front
};


const int POOL_CAP = 10000;

struct OrderPool{
    Order pool[POOL_CAP];
    Order* free_head = nullptr;

    OrderPool(){
        for (int i=0; i< POOL_CAP-1; ++i){
            pool[i].next_in_q = &pool[i+1];
        }
        pool[POOL_CAP -1].next_in_q = nullptr;
        free_head = &pool[0];
    }

    
    Order* allocate(){ 
        if (!free_head) return nullptr;
        Order* alloc_order = free_head; //O(1 alloc
        free_head = free_head->next_in_q; 
        return alloc_order;
    }

    void deallocate(Order* order){
        order->next_in_q = free_head;
        free_head = order;
    }


};

struct OrderQueue{ //fifo singly LL
    Order* head = nullptr;
    Order* tail = nullptr;

    bool empty() {return head == nullptr; }

    void push_back(Order* order){
        order->next_in_q = nullptr;
        if (empty()){
            head = order;
            tail = order;
        } else{
            tail->next_in_q = order;
            tail = order;
        }
    }

    Order* pop_front(){
        if (empty()) return nullptr;

        Order* po = head;
        head = head->next_in_q;
        if (head == nullptr){
            tail = nullptr;
        }
        return po;
    }
};

struct PriceLevel{
    int price;
    OrderQueue orders;
    PriceLevel* prev = nullptr;
    PriceLevel* next = nullptr;
};

struct OrderBook
{
    PriceLevel* bids_head = nullptr;
    PriceLevel* asks_head = nullptr;

    PriceLevel* create_insert_pl(int price, PriceLevel* prev_level, PriceLevel* next_level){
        
    }

    PriceLevel* get_create_pricelvl(int price, bool is_buy){
        PriceLevel* curr = is_buy ? bids_head : asks_head;
        PriceLevel* prev = nullptr;
    
    }
};


void create_order(const auto& tokens){
    if (!tokens.size() != 7) return;

    std::string book_name_v = std::string(tokens[2]);

    uint32_t cur_book_id;;
    auto it = bookname_to_id.find(std::string(book_name_v));
    if (it ==bookname_to_id.end()){
        cur_book_id = next_book_id++;
        bookname_to_id[std::string(book_name_v)] = cur_book_id;
        bookid_to_name[cur_book_id] = std::string(book_name_v);
    }
    else  cur_book_id = it->second;

    Order* order = order_pool.allocate();
    order->id = next_id++;
    order->client_id = parse_int(tokens[1]);
    order->book_id = cur_book_id;
    order->is_buy = (tokens[4] == "B");
    order->quantity = static_cast<uint32_t> (parse_int(tokens[5]));
    order->price = static_cast<uint32_t> (parse_int(tokens[6]));  

    OrderBook* ob = all_books[cur_book_id];

}

void cleanup_order(Order* order){


}
bool cancel_order(const auto& token){

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
            cancel_order(tokens[2]);
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
