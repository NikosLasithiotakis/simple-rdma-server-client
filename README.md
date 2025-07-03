# Simple RDMA Server-Client Example

A minimal demonstration of RDMA (Remote Direct Memory Access) communication using `libibverbs` and `librdmacm`. This project includes a basic **server** (listens for messages) and **client** (sends messages interactively) to showcase RDMA's low-latency capabilities.

## Build & Run
```sh
make # Compiles both server and client

Usage:

#Terminal 1 (server):
./rdma_server

#Terminal 2 (client):
./rdma_client # Type messages to send, 'exit' to quit