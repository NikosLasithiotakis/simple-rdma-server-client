# Simple RDMA Server-Client Example

This project demonstrates a robust example of RDMA (Remote Direct Memory Access) communication using the `libibverbs` and `librdmacm` libraries.

It features decoupled connection management, page-aligned memory registration, and a dedicated worker thread pool.

## Prerequisites

- Linux OS with RDMA-capable hardware (InfiniBand or RoCE) or SoftRoCE (RXE) configured.
- `libibverbs-dev` and `librdmacm-dev` packages installed.
- `cmake` and `build-essential`.

## Configuration

Before running, edit the `config.yml` file in the root directory to match the IP address of your RDMA interface:

```yaml
ip: "192.168.1.100"
port: "7741"

```

## Build Instructions

This project uses CMake. To compile both the server and the client:

```sh
mkdir build
cd build
cmake ..
make

```

## Usage

**Terminal 1 (Server):**
Run the server from the build directory:

```sh
./rdma_server

```

**Terminal 2, 3, ... (Multiple Clients):**
Run the client from the build directory. You can connect multiple clients concurrently.

```sh
./rdma_client

```

Type your messages and press `Enter` to send them via RDMA. Type `exit` to safely tear down the QP, deregister memory, and close the client connection.
