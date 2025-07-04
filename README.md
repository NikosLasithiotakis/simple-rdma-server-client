# Simple RDMA Server-Client Example

This project demonstrates a minimal example of RDMA (Remote Direct Memory Access) communication using libibverbs and librdmacm libraries.  
It includes two components:  
**server**: Accepts multiple client connections concurrently using threads. Receives messages from each connected client.  
**client**: Connects to the server and sends messages interactively.

## Build & Run
```sh
make # Compiles both server and client

Usage:

#Terminal 1 (server):
./rdma_server

#Terminal 2, 3, ... (multiple clients):
./rdma_client # Type messages and press Enter to send, 'exit' to close the client connection
