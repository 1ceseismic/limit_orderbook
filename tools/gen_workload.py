import random
import argparse

def generate_workload(num_orders, num_clients, num_books, cancel_ratio, price_base, price_volatility, output_file):

    active_orders = [] 
    token_id = 0

    with open(output_file, 'w') as f:
        for i in range(num_orders):
            should_cancel = random.random() < cancel_ratio and len(active_orders) > 0
            
            if not should_cancel:

                token_id += 1
                client_id = random.randint(1, num_clients)
                book_id = random.randint(1, num_books)
                side = random.choice(['B', 'S'])
                quantity = random.randint(1, 200) * 5 


                if side == 'B':
                    price = price_base + random.randint(0, price_volatility)
                else: 
                    price = price_base + random.randint(-price_volatility, 0)
                
                price = max(1, price)

                f.write(f"O, Client {client_id}, Orderbook {book_id}, Token {token_id}, {side}, {quantity}, {price}\n")
                
                active_orders.append({'token': token_id, 'client_id': client_id, 'book_id': book_id})

            else:
                order_to_cancel_idx = random.randint(0, len(active_orders) - 1)
                order_to_cancel = active_orders.pop(order_to_cancel_idx) 
                
                f.write(f"X, Client {order_to_cancel['client_id']}, Token {order_to_cancel['token']}\n")

    print(f"Successfully generated '{output_file}' with {num_orders} messages.")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Generate a workload for a financial exchange simulator.")
    parser.add_argument("-n", "--num-orders", type=int, default=1000000, help="Total number of messages to generate.")
    parser.add_argument("-c", "--num-clients", type=int, default=50, help="Number of unique clients.")
    parser.add_argument("-b", "--num-books", type=int, default=10, help="Number of unique order books.")
    parser.add_argument("-x", "--cancel-ratio", type=float, default=0.1, help="Probability of a message being a cancellation (0.0 to 1.0).")
    parser.add_argument("-p", "--price-base", type=int, default=1000, help="The base price around which orders are generated.")
    parser.add_argument("-v", "--price-volatility", type=int, default=50, help="The range (+/-) of price variation from the base.")
    parser.add_argument("-o", "--output-file", type=str, default="workload.txt", help="The name of the output file.")
    
    args = parser.parse_args()
    
    generate_workload(args.num_orders, args.num_clients, args.num_books, 
                      args.cancel_ratio, args.price_base, args.price_volatility, 
                      args.output_file)