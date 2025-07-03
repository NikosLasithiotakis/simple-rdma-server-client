CC = gcc
CFLAGS = -Wall -O2
LDFLAGS = -libverbs -lrdmacm

all: rdma_server rdma_client

rdma_server: rdma_server.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

rdma_client: rdma_client.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f rdma_server rdma_client
