# Simple RDMA Server-Client Example

This project demonstrates a minimal example of RDMA (Remote Direct Memory Access) communication using libibverbs and librdmacm libraries.  
It includes two components:  
**server**: Listens for incoming RDMA connection requests and receives messages from clients.  
**client**: Connects to the server and sends messages interactively.

## Build & Run
```sh
make # Compiles both server and client

Usage:

#Terminal 1 (server):
./rdma_server

#Terminal 2 (client):
./rdma_client # Type messages to send, 'exit' to quit