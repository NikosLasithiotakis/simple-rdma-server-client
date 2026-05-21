#include "utils.h"
#include <netdb.h>
#include <rdma/rdma_cma.h>
#include <stdio.h>
#include <stdlib.h>

#define TIMEOUT_MS 500

struct client_context {
	struct rdma_event_channel *ec;
	struct rdma_cm_id *cm_id;
	struct ibv_pd *pd;
	struct ibv_comp_channel *comp_chan;
	struct ibv_cq *cq;
	struct ibv_mr *mr;
	char *msg;
};

struct client_context *setup_client_connection(const char *ip, const char *port)
{
	struct client_context *ctx = calloc(1, sizeof(struct client_context));
	if (!ctx) {
		perror("calloc failed");
		exit(EXIT_FAILURE);
	}

	struct addrinfo *addr;
	struct rdma_cm_event *event;
	struct ibv_qp_init_attr qp_attr;

	if (getaddrinfo(ip, port, NULL, &addr)) {
		perror("getaddrinfo failed");
		exit(EXIT_FAILURE);
	}

	ctx->ec = rdma_create_event_channel();
	if (!ctx->ec) {
		perror("rdma_create_event_channel failed");
		exit(EXIT_FAILURE);
	}

	if (rdma_create_id(ctx->ec, &ctx->cm_id, NULL, RDMA_PS_TCP)) {
		perror("rdma_create_id failed");
		exit(EXIT_FAILURE);
	}

	if (rdma_resolve_addr(ctx->cm_id, NULL, addr->ai_addr, TIMEOUT_MS)) {
		perror("rdma_resolve_addr failed");
		exit(EXIT_FAILURE);
	}
	freeaddrinfo(addr);

	if (rdma_get_cm_event(ctx->ec, &event) || event->event != RDMA_CM_EVENT_ADDR_RESOLVED) {
		fprintf(stderr, "Address resolution failed\n");
		exit(EXIT_FAILURE);
	}
	rdma_ack_cm_event(event);

	if (rdma_resolve_route(ctx->cm_id, TIMEOUT_MS)) {
		perror("rdma_resolve_route failed");
		exit(EXIT_FAILURE);
	}

	if (rdma_get_cm_event(ctx->ec, &event) || event->event != RDMA_CM_EVENT_ROUTE_RESOLVED) {
		fprintf(stderr, "Route resolution failed\n");
		exit(EXIT_FAILURE);
	}
	rdma_ack_cm_event(event);

	ctx->pd = ibv_alloc_pd(ctx->cm_id->verbs);
	if (!ctx->pd) {
		perror("ibv_alloc_pd failed");
		exit(EXIT_FAILURE);
	}

	ctx->comp_chan = ibv_create_comp_channel(ctx->cm_id->verbs);
	if (!ctx->comp_chan) {
		perror("ibv_create_comp_channel failed");
		exit(EXIT_FAILURE);
	}

	ctx->cq = ibv_create_cq(ctx->cm_id->verbs, 10, NULL, ctx->comp_chan, 0);
	if (!ctx->cq) {
		perror("ibv_create_cq failed");
		exit(EXIT_FAILURE);
	}

	if (ibv_req_notify_cq(ctx->cq, 0)) {
		perror("ibv_req_notify_cq failed");
		exit(EXIT_FAILURE);
	}

	memset(&qp_attr, 0, sizeof(qp_attr));
	qp_attr.send_cq = ctx->cq;
	qp_attr.recv_cq = ctx->cq;
	qp_attr.qp_type = IBV_QPT_RC;
	qp_attr.cap.max_send_wr = 10;
	qp_attr.cap.max_recv_wr = 10;
	qp_attr.cap.max_send_sge = 1;
	qp_attr.cap.max_recv_sge = 1;

	if (rdma_create_qp(ctx->cm_id, ctx->pd, &qp_attr)) {
		perror("rdma_create_qp failed");
		exit(EXIT_FAILURE);
	}

	struct rdma_conn_param conn_param = { 0 };
	conn_param.initiator_depth = 1;
	conn_param.responder_resources = 1;
	conn_param.retry_count = 7;

	if (rdma_connect(ctx->cm_id, &conn_param)) {
		perror("rdma_connect failed");
		exit(EXIT_FAILURE);
	}

	if (rdma_get_cm_event(ctx->ec, &event) || event->event != RDMA_CM_EVENT_ESTABLISHED) {
		fprintf(stderr, "Connection failed to establish\n");
		exit(EXIT_FAILURE);
	}
	rdma_ack_cm_event(event);

	return ctx;
}

void register_client_memory(struct client_context *ctx)
{
	int ret = posix_memalign((void **)&ctx->msg, 4096, MSG_SIZE);
	if (ret != 0) {
		fprintf(stderr, "posix_memalign failed (Error %d)\n", ret);
		exit(EXIT_FAILURE);
	}

	ctx->mr = ibv_reg_mr(ctx->pd, ctx->msg, MSG_SIZE, IBV_ACCESS_LOCAL_WRITE);
	if (!ctx->mr) {
		perror("ibv_reg_mr failed");
		exit(EXIT_FAILURE);
	}
}

void *client_cm_loop(void *arg)
{
	struct client_context *ctx = (struct client_context *)arg;
	struct rdma_cm_event *event;

	while (rdma_get_cm_event(ctx->ec, &event) == 0) {
		int event_type = event->event;
		rdma_ack_cm_event(event);

		if (event_type == RDMA_CM_EVENT_DISCONNECTED) {
			printf("\n\n[ALERT] The server has crashed or disconnected!\n");
			printf("Exiting client...\n");

			exit(EXIT_FAILURE);
		}
	}
	return NULL;
}

void client_send_loop(struct client_context *ctx)
{
	struct ibv_send_wr wr, *bad_wr = NULL;
	struct ibv_sge sge;
	struct ibv_wc wc;
	struct ibv_cq *ev_cq;
	void *ev_ctx;

	memset(&sge, 0, sizeof(sge));
	sge.addr = (uintptr_t)ctx->msg;
	sge.length = MSG_SIZE;
	sge.lkey = ctx->mr->lkey;

	char input[MSG_SIZE];
	while (1) {
		printf("Enter message to send: ");
		if (!fgets(input, MSG_SIZE, stdin))
			break;

		strcpy(ctx->msg, input);

		memset(&wr, 0, sizeof(wr));
		wr.wr_id = (uintptr_t)ctx->msg;
		wr.opcode = IBV_WR_SEND;
		wr.sg_list = &sge;
		wr.num_sge = 1;
		wr.send_flags = IBV_SEND_SIGNALED;

		if (ibv_post_send(ctx->cm_id->qp, &wr, &bad_wr)) {
			perror("ibv_post_send failed");
			break;
		}

		if (ibv_get_cq_event(ctx->comp_chan, &ev_cq, &ev_ctx)) {
			perror("ibv_get_cq_event failed");
			break;
		}
		ibv_ack_cq_events(ev_cq, 1);

		if (ibv_req_notify_cq(ev_cq, 0)) {
			perror("ibv_req_notify_cq failed");
			break;
		}

		while (ibv_poll_cq(ctx->cq, 1, &wc) == 0)
			;
		if (wc.status != IBV_WC_SUCCESS) {
			fprintf(stderr, "Send failed: %s\n", ibv_wc_status_str(wc.status));
			break;
		}

		if (strcmp(input, "exit\n") == 0)
			break;
	}
}

void cleanup_client(struct client_context *ctx)
{
	if (ctx->cm_id) {
		rdma_disconnect(ctx->cm_id);
		if (ctx->cm_id->qp)
			rdma_destroy_qp(ctx->cm_id);
	}
	if (ctx->mr)
		ibv_dereg_mr(ctx->mr);
	if (ctx->msg)
		free(ctx->msg);
	if (ctx->cq)
		ibv_destroy_cq(ctx->cq);
	if (ctx->comp_chan)
		ibv_destroy_comp_channel(ctx->comp_chan);
	if (ctx->pd)
		ibv_dealloc_pd(ctx->pd);
	if (ctx->cm_id)
		rdma_destroy_id(ctx->cm_id);
	if (ctx->ec)
		rdma_destroy_event_channel(ctx->ec);
	free(ctx);
}

int main()
{
	char ip[64] = { 0 };
	char port[16] = { 0 };

	get_config_from_yaml(CONFIG_FILE, ip, sizeof(ip), port, sizeof(port));
	printf("Read config: IP %s, Port %s\n", ip, port);

	struct client_context *ctx = setup_client_connection(ip, port);

	register_client_memory(ctx);

	pthread_t cm_thread;
	if (pthread_create(&cm_thread, NULL, client_cm_loop, ctx) == 0) {
		pthread_detach(cm_thread);
	}

	client_send_loop(ctx);

	cleanup_client(ctx);

	return 0;
}
