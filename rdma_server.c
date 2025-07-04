#include <arpa/inet.h>
#include <infiniband/verbs.h>
#include <pthread.h>
#include <rdma/rdma_cma.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PORT 7741
#define MSG_SIZE 64
#define MAX_CLIENTS 10

typedef struct {
	struct rdma_cm_id *id;
	struct ibv_pd *pd;
	struct ibv_cq *cq;
	struct ibv_comp_channel *comp_chan;
	struct ibv_mr *mr;
	char *msg;
} client_context;

void *client_handler(void *arg)
{
	client_context *ctx = (client_context *)arg;
	struct ibv_wc wc;
	struct ibv_sge sge;
	struct ibv_recv_wr wr, *bad_wr = NULL;

	struct ibv_context *verbs = ctx->id->verbs;
	struct ibv_qp_init_attr qp_attr = { 0 };

	ctx->pd = ibv_alloc_pd(verbs);
	ctx->comp_chan = ibv_create_comp_channel(verbs);
	ctx->cq = ibv_create_cq(verbs, 10, NULL, ctx->comp_chan, 0);
	ibv_req_notify_cq(ctx->cq, 0);

	qp_attr.send_cq = ctx->cq;
	qp_attr.recv_cq = ctx->cq;
	qp_attr.qp_type = IBV_QPT_RC;
	qp_attr.cap.max_send_wr = 10;
	qp_attr.cap.max_recv_wr = 10;
	qp_attr.cap.max_send_sge = 1;
	qp_attr.cap.max_recv_sge = 1;

	rdma_create_qp(ctx->id, ctx->pd, &qp_attr);

	struct rdma_conn_param conn_param = { 0 };
	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;
	conn_param.retry_count = 7;
	conn_param.rnr_retry_count = 7;

	rdma_accept(ctx->id, &conn_param);

	// Wait for established event
	struct rdma_event_channel *ec = ctx->id->channel;
	struct rdma_cm_event *event;
	rdma_get_cm_event(ec, &event);
	rdma_ack_cm_event(event);

	ctx->msg = malloc(MSG_SIZE);
	ctx->mr = ibv_reg_mr(ctx->pd, ctx->msg, MSG_SIZE, IBV_ACCESS_LOCAL_WRITE);

	memset(&sge, 0, sizeof(sge));
	sge.addr = (uintptr_t)ctx->msg;
	sge.length = MSG_SIZE;
	sge.lkey = ctx->mr->lkey;

	memset(&wr, 0, sizeof(wr));
	wr.wr_id = (uintptr_t)ctx->msg;
	wr.sg_list = &sge;
	wr.num_sge = 1;

	ibv_post_recv(ctx->id->qp, &wr, &bad_wr);

	while (1) {
		struct ibv_cq *cq;
		void *ctx_ptr;
		ibv_get_cq_event(ctx->comp_chan, &cq, &ctx_ptr);
		ibv_ack_cq_events(cq, 1);
		ibv_req_notify_cq(cq, 0);

		while (ibv_poll_cq(cq, 1, &wc) > 0) {
			if (wc.status != IBV_WC_SUCCESS || wc.opcode != IBV_WC_RECV) {
				fprintf(stderr, "Error: %s\n", ibv_wc_status_str(wc.status));
				continue;
			}

			printf("Received from client: %s", ctx->msg);

			if (strcmp(ctx->msg, "exit\n") == 0)
				goto done;

			ibv_post_recv(ctx->id->qp, &wr, &bad_wr);
		}
	}

done:
	rdma_disconnect(ctx->id);
	rdma_destroy_qp(ctx->id);
	ibv_dereg_mr(ctx->mr);
	free(ctx->msg);
	ibv_destroy_cq(ctx->cq);
	ibv_destroy_comp_channel(ctx->comp_chan);
	ibv_dealloc_pd(ctx->pd);
	rdma_destroy_id(ctx->id);
	free(ctx);
	return NULL;
}

int main()
{
	struct rdma_event_channel *ec = rdma_create_event_channel();
	struct rdma_cm_id *listen_id;
	struct rdma_cm_event *event;
	struct sockaddr_in addr;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(PORT);
	addr.sin_addr.s_addr = INADDR_ANY;

	rdma_create_id(ec, &listen_id, NULL, RDMA_PS_TCP);
	rdma_bind_addr(listen_id, (struct sockaddr *)&addr);
	rdma_listen(listen_id, MAX_CLIENTS);

	printf("Server listening on port %d\n", PORT);

	while (1) {
		if (rdma_get_cm_event(ec, &event)) {
			perror("rdma_get_cm_event");
			continue;
		}

		if (event->event != RDMA_CM_EVENT_CONNECT_REQUEST) {
			fprintf(stderr, "Unexpected event %d\n", event->event);
			rdma_ack_cm_event(event);
			continue;
		}

		struct rdma_cm_id *conn_id = event->id;
		rdma_ack_cm_event(event);

		client_context *ctx = calloc(1, sizeof(client_context));
		ctx->id = conn_id;

		pthread_t tid;
		pthread_create(&tid, NULL, client_handler, ctx);
		pthread_detach(tid); // Clean up automatically when thread exits
	}

	rdma_destroy_id(listen_id);
	rdma_destroy_event_channel(ec);
	return 0;
}
