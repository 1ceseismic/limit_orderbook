
#include <iostream>
#include <bits/stdc++.h>
#include <stdexcept>

struct OrderBook;

void parseOB(std::istream& input, OrderBook& ob){}


struct Order
{
    bool active = true;
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
        return true;
    }
};

int main()
{
    OrderBook ob;
    std::ifstream infile("input_orders.txt");
    if (!infile) {std::cerr << "cant open input file\n"; return 1;}


    parseOB(infile, ob); //will parse whole file as one


    return 0;
}
