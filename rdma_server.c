#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#define PORT 7741
#define MSG_SIZE 64

int main() {
    struct rdma_event_channel *ec = rdma_create_event_channel();
    struct rdma_cm_id *listen_id = NULL, *conn_id = NULL;
    struct rdma_cm_event *event;
    struct sockaddr_in addr;

    struct ibv_pd *pd;
    struct ibv_cq *cq;
    void *cq_context;
    struct ibv_comp_channel *comp_chan;
    struct ibv_qp_init_attr qp_attr;
    struct ibv_mr *mr;
    char *msg;
    struct ibv_wc wc;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    rdma_create_id(ec, &listen_id, NULL, RDMA_PS_TCP);
    rdma_bind_addr(listen_id, (struct sockaddr *)&addr);
    rdma_listen(listen_id, 10);
    printf("Server listening on port %d\n", PORT);

    // Wait for connection request
    rdma_get_cm_event(ec, &event);
    if (event->event != RDMA_CM_EVENT_CONNECT_REQUEST) {
        fprintf(stderr, "Unexpected event: %d\n", event->event);
        return 1;
    }

    conn_id = event->id;
    rdma_ack_cm_event(event);

    pd = ibv_alloc_pd(conn_id->verbs);
    comp_chan = ibv_create_comp_channel(conn_id->verbs);
    cq = ibv_create_cq(conn_id->verbs, 10, NULL, comp_chan, 0);
    ibv_req_notify_cq(cq, 0);

    memset(&qp_attr, 0, sizeof(qp_attr));
    // Same for both send and recv CQ
    qp_attr.send_cq = cq;
    qp_attr.recv_cq = cq;
    qp_attr.qp_type = IBV_QPT_RC;
    qp_attr.cap.max_send_wr = 10;
    qp_attr.cap.max_recv_wr = 10;
    qp_attr.cap.max_send_sge = 1;
    qp_attr.cap.max_recv_sge = 1;

    rdma_create_qp(conn_id, pd, &qp_attr);

    // Accept connection
    struct rdma_conn_param conn_param = {0};
    conn_param.responder_resources = 1;
    conn_param.initiator_depth = 1;
    conn_param.retry_count = 7;
    conn_param.rnr_retry_count = 7;

    rdma_accept(conn_id, &conn_param);

    // Blocks until a client connects
    rdma_get_cm_event(ec, &event);
    if (event->event != RDMA_CM_EVENT_ESTABLISHED) {
        fprintf(stderr, "Failed to establish connection\n");
        return 1;
    }
    rdma_ack_cm_event(event);

    msg = malloc(MSG_SIZE);
    mr = ibv_reg_mr(pd, msg, MSG_SIZE, IBV_ACCESS_LOCAL_WRITE);

    struct ibv_sge sge;
    memset(&sge, 0, sizeof(sge));
    sge.addr = (uintptr_t)msg;
    sge.length = MSG_SIZE;
    sge.lkey = mr->lkey;

    struct ibv_recv_wr wr, *bad_wr = NULL;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id = (uintptr_t)msg;
    wr.sg_list = &sge;
    wr.num_sge = 1;

    ibv_post_recv(conn_id->qp, &wr, &bad_wr);

    while (1) {
        ibv_get_cq_event(comp_chan, &cq, &cq_context);
        ibv_ack_cq_events(cq, 1);
        ibv_req_notify_cq(cq, 0);

        while (ibv_poll_cq(cq, 1, &wc) > 0) {
            if (wc.status != IBV_WC_SUCCESS || wc.opcode != IBV_WC_RECV) {
                fprintf(stderr, "Error: %s\n", ibv_wc_status_str(wc.status));
                continue;
            }

            printf("Received: %s", msg);

            if (strcmp(msg, "exit\n") == 0)
                goto done;

            // Re-post the same receive buffer
            memset(&wr, 0, sizeof(wr));
            wr.wr_id = (uintptr_t)msg;
            wr.sg_list = &sge;
            wr.num_sge = 1;
            ibv_post_recv(conn_id->qp, &wr, &bad_wr);
        }
    }
done:
    // Cleanup
    rdma_disconnect(conn_id);
    rdma_destroy_qp(conn_id);
    ibv_dereg_mr(mr);
    free(msg);
    ibv_destroy_cq(cq);
    ibv_destroy_comp_channel(comp_chan);
    ibv_dealloc_pd(pd);
    rdma_destroy_id(conn_id);
    rdma_destroy_id(listen_id);
    rdma_destroy_event_channel(ec);

    return 0;
}
