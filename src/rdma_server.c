#include "utils.h"
#include <arpa/inet.h>
#include <poll.h>
#include <rdma/rdma_cma.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#define MAX_CLIENTS 128
#define NUM_WORKERS 4
#define ALIGNMENT 4096

volatile sig_atomic_t keep_running = 1;

void handle_signal(int sig)
{
	if (sig == SIGINT || sig == SIGTERM) {
		printf("\nShutdown signal received. Initiating clean teardown...\n");
		keep_running = 0;
	}
}

struct server_context {
	struct ibv_pd *pd;
	int client_counter;
	struct rdma_cm_id *listen_id;
};

struct worker_context {
	int worker_id;
	pthread_t thread;
	struct ibv_comp_channel *comp_chan;
	struct ibv_cq *cq;
};

struct client_context {
	struct rdma_cm_id *cm_id;
	struct ibv_mr *mr;
	char *msg;
	struct worker_context *assigned_worker;
};

struct server_context server;
struct worker_context workers[NUM_WORKERS];
struct client_context *active_clients[MAX_CLIENTS] = { NULL };
pthread_mutex_t clients_lock = PTHREAD_MUTEX_INITIALIZER;

void *worker_loop(void *arg)
{
	struct worker_context *worker = (struct worker_context *)arg;
	struct ibv_cq *ev_cq;
	void *ev_ctx;
	struct ibv_wc wc[16];

	printf("[Worker %d] Started and waiting for events.\n", worker->worker_id);

	struct pollfd pfd = { .fd = worker->comp_chan->fd, .events = POLLIN };

	while (keep_running) {
		int ret = poll(&pfd, 1, 500);
		if (ret == 0)
			continue;
		if (ret < 0)
			break;

		if (ibv_get_cq_event(worker->comp_chan, &ev_cq, &ev_ctx) == 0) {
			ibv_ack_cq_events(ev_cq, 1);
			if (ibv_req_notify_cq(ev_cq, 0))
				break;

			int num_events;
			while ((num_events = ibv_poll_cq(worker->cq, 16, wc)) > 0) {
				for (int i = 0; i < num_events; i++) {
					struct client_context *client = (struct client_context *)(uintptr_t)wc[i].wr_id;

					if (wc[i].status != IBV_WC_SUCCESS) {
						printf("[Worker %d] Client connection closed or flush error.\n",
						       worker->worker_id);
						continue;
					}

					if (wc[i].opcode == IBV_WC_RECV) {
						printf("[Worker %d] Received: %s", worker->worker_id, client->msg);

						if (strcmp(client->msg, "exit\n") != 0) {
							struct ibv_sge sge = { .addr = (uintptr_t)client->msg,
									       .length = MSG_SIZE,
									       .lkey = client->mr->lkey };
							struct ibv_recv_wr wr = { .wr_id = (uintptr_t)client,
										  .sg_list = &sge,
										  .num_sge = 1 };
							struct ibv_recv_wr *bad_wr;
							if (ibv_post_recv(client->cm_id->qp, &wr, &bad_wr)) {
								perror("ibv_post_recv failed inside worker");
							}
						}
					}
				}
			}
			if (num_events < 0) {
				fprintf(stderr, "[Worker %d] ibv_poll_cq failed\n", worker->worker_id);
				break;
			}
		} else {
			perror("ibv_get_cq_event failed");
			break;
		}
	}
	printf("[Worker %d] Shutting down...\n", worker->worker_id);
	return NULL;
}

struct rdma_cm_id *setup_rdma_server(struct rdma_event_channel *ec, const char *ip, const char *port)
{
	struct rdma_cm_id *listen_id;
	if (rdma_create_id(ec, &listen_id, NULL, RDMA_PS_TCP)) {
		perror("rdma_create_id failed");
		exit(EXIT_FAILURE);
	}

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(atoi(port));
	if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
		fprintf(stderr, "Invalid IP address: %s\n", ip);
		exit(EXIT_FAILURE);
	}

	if (rdma_bind_addr(listen_id, (struct sockaddr *)&addr)) {
		perror("rdma_bind_addr failed");
		exit(EXIT_FAILURE);
	}

	if (rdma_listen(listen_id, MAX_CLIENTS)) {
		perror("rdma_listen failed");
		exit(EXIT_FAILURE);
	}
	return listen_id;
}

void setup_worker_pool(struct ibv_context *verbs)
{
	server.pd = ibv_alloc_pd(verbs);
	if (!server.pd) {
		perror("ibv_alloc_pd failed");
		exit(EXIT_FAILURE);
	}

	for (int i = 0; i < NUM_WORKERS; i++) {
		workers[i].worker_id = i;
		workers[i].comp_chan = ibv_create_comp_channel(verbs);
		if (!workers[i].comp_chan) {
			perror("ibv_create_comp_channel failed");
			exit(EXIT_FAILURE);
		}

		workers[i].cq = ibv_create_cq(verbs, 256, NULL, workers[i].comp_chan, 0);
		if (!workers[i].cq) {
			perror("ibv_create_cq failed");
			exit(EXIT_FAILURE);
		}

		if (ibv_req_notify_cq(workers[i].cq, 0)) {
			perror("ibv_req_notify_cq failed");
			exit(EXIT_FAILURE);
		}

		if (pthread_create(&workers[i].thread, NULL, worker_loop, &workers[i])) {
			perror("pthread_create failed");
			exit(EXIT_FAILURE);
		}
	}
}

void handle_connect_request(struct rdma_cm_id *conn_id)
{
	struct client_context *client = calloc(1, sizeof(struct client_context));
	if (!client) {
		perror("calloc failed for client");
		return;
	}
	client->cm_id = conn_id;

	struct worker_context *assigned = &workers[server.client_counter++ % NUM_WORKERS];
	client->assigned_worker = assigned;

	struct ibv_qp_init_attr qp_attr = { 0 };
	qp_attr.send_cq = assigned->cq;
	qp_attr.recv_cq = assigned->cq;
	qp_attr.qp_type = IBV_QPT_RC;
	qp_attr.cap.max_send_wr = 10;
	qp_attr.cap.max_recv_wr = 10;
	qp_attr.cap.max_send_sge = 1;
	qp_attr.cap.max_recv_sge = 1;

	if (rdma_create_qp(conn_id, server.pd, &qp_attr)) {
		perror("rdma_create_qp failed");
		free(client);
		return;
	}

	int ret = posix_memalign((void **)&client->msg, ALIGNMENT, MSG_SIZE);
	if (ret != 0) {
		fprintf(stderr, "posix_memalign failed (Error %d)\n", ret);
		rdma_destroy_qp(conn_id);
		free(client);
		return;
	}

	client->mr = ibv_reg_mr(server.pd, client->msg, MSG_SIZE, IBV_ACCESS_LOCAL_WRITE);
	if (!client->mr) {
		perror("ibv_reg_mr failed");
		free(client->msg);
		rdma_destroy_qp(conn_id);
		free(client);
		return;
	}

	struct ibv_sge sge = { .addr = (uintptr_t)client->msg, .length = MSG_SIZE, .lkey = client->mr->lkey };
	struct ibv_recv_wr wr = { .wr_id = (uintptr_t)client, .sg_list = &sge, .num_sge = 1 };
	struct ibv_recv_wr *bad_wr;
	if (ibv_post_recv(conn_id->qp, &wr, &bad_wr)) {
		perror("ibv_post_recv failed");
	}

	struct rdma_conn_param conn_param = { 0 };
	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;
	conn_param.retry_count = 7;
	conn_param.rnr_retry_count = 7;

	if (rdma_accept(conn_id, &conn_param) == 0) {
		pthread_mutex_lock(&clients_lock);
		for (int i = 0; i < MAX_CLIENTS; i++) {
			if (active_clients[i] == NULL) {
				active_clients[i] = client;
				break;
			}
		}
		pthread_mutex_unlock(&clients_lock);
	} else {
		free(client->msg);
		free(client);
		perror("rdma_accept failed");
	}
}

void connection_manager(struct rdma_event_channel *ec)
{
	struct rdma_cm_event *event;
	struct pollfd pfd = { .fd = ec->fd, .events = POLLIN };

	while (keep_running) {
		int ret = poll(&pfd, 1, 500);
		if (ret == 0)
			continue;
		if (ret < 0)
			break;

		if (rdma_get_cm_event(ec, &event) == 0) {
			int event_type = event->event;
			struct rdma_cm_id *conn_id = event->id;
			rdma_ack_cm_event(event);

			if (event_type == RDMA_CM_EVENT_CONNECT_REQUEST) {
				handle_connect_request(conn_id);
			} else if (event_type == RDMA_CM_EVENT_ESTABLISHED) {
				printf("[CM] New client connected!\n");
			} else if (event_type == RDMA_CM_EVENT_DISCONNECTED) {
				printf("[CM] Client disconnected.\n");
				pthread_mutex_lock(&clients_lock);
				for (int i = 0; i < MAX_CLIENTS; i++) {
					if (active_clients[i] != NULL && active_clients[i]->cm_id == conn_id) {
						struct client_context *client = active_clients[i];

						if (client->cm_id->qp) {
							rdma_destroy_qp(client->cm_id);
						}

						if (client->mr) {
							ibv_dereg_mr(client->mr);
						}
						if (client->msg) {
							free(client->msg);
						}

						rdma_destroy_id(client->cm_id);

						free(client);
						active_clients[i] = NULL;

						printf("[CM] Client slot %d successfully reclaimed.\n", i);
						break;
					}
				}
				pthread_mutex_unlock(&clients_lock);
			}
		}
	}
}

void cleanup_server(struct rdma_event_channel *ec)
{
	printf("Cleaning up active clients...\n");
	pthread_mutex_lock(&clients_lock);
	for (int i = 0; i < MAX_CLIENTS; i++) {
		if (active_clients[i] != NULL) {
			struct client_context *client = active_clients[i];
			rdma_disconnect(client->cm_id);
			rdma_destroy_qp(client->cm_id);
			ibv_dereg_mr(client->mr);
			free(client->msg);
			rdma_destroy_id(client->cm_id);
			free(client);
			active_clients[i] = NULL;
		}
	}
	pthread_mutex_unlock(&clients_lock);

	printf("Waiting for worker threads to exit...\n");
	for (int i = 0; i < NUM_WORKERS; i++) {
		pthread_join(workers[i].thread, NULL);
		ibv_destroy_cq(workers[i].cq);
		ibv_destroy_comp_channel(workers[i].comp_chan);
	}

	printf("Destroying global resources...\n");
	ibv_dealloc_pd(server.pd);
	rdma_destroy_id(server.listen_id);
	rdma_destroy_event_channel(ec);

	printf("Shutdown complete.\n");
}

int main()
{
	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);

	char ip[64] = { 0 };
	char port[16] = { 0 };
	get_config_from_yaml(CONFIG_FILE, ip, sizeof(ip), port, sizeof(port));

	struct rdma_event_channel *ec = rdma_create_event_channel();
	if (!ec) {
		perror("rdma_create_event_channel failed");
		exit(EXIT_FAILURE);
	}

	server.listen_id = setup_rdma_server(ec, ip, port);
	setup_worker_pool(server.listen_id->verbs);

	printf("Listening on %s:%s with %d worker threads\n", ip, port, NUM_WORKERS);
	printf("Press Ctrl+C to initiate a clean shutdown.\n");

	connection_manager(ec);

	cleanup_server(ec);

	return 0;
}
