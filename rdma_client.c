#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <netdb.h>

#define IP "192.168.5.120"
#define PORT "7741"
#define TIMEOUT_MS 500
#define MSG_SIZE 64

int main() {
    struct addrinfo *addr;
    struct rdma_cm_id *cm_id = NULL;
    struct rdma_event_channel *ec = NULL;
    struct rdma_cm_event *event;
    struct ibv_pd *pd;
    struct ibv_comp_channel *comp_chan;
    struct ibv_cq *cq;
    void *cq_context;
    struct ibv_mr *mr;
    struct ibv_qp_init_attr qp_attr;
    char *msg;
    struct ibv_send_wr wr, *bad_wr = NULL;
    struct ibv_sge sge;
    struct ibv_wc wc;

    getaddrinfo(IP, PORT, NULL, &addr);
    ec = rdma_create_event_channel();
    rdma_create_id(ec, &cm_id, NULL, RDMA_PS_TCP);
    rdma_resolve_addr(cm_id, NULL, addr->ai_addr, TIMEOUT_MS);
    freeaddrinfo(addr);

    rdma_get_cm_event(ec, &event);
    rdma_ack_cm_event(event);

    rdma_resolve_route(cm_id, TIMEOUT_MS);
    rdma_get_cm_event(ec, &event);
    rdma_ack_cm_event(event);

    pd = ibv_alloc_pd(cm_id->verbs);
    comp_chan = ibv_create_comp_channel(cm_id->verbs);
    cq = ibv_create_cq(cm_id->verbs, 10, NULL, comp_chan, 0);
    ibv_req_notify_cq(cq, 0);

    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.send_cq = cq;
    qp_attr.recv_cq = cq;
    qp_attr.qp_type = IBV_QPT_RC;
    qp_attr.cap.max_send_wr = 10;
    qp_attr.cap.max_recv_wr = 10;
    qp_attr.cap.max_send_sge = 1;
    qp_attr.cap.max_recv_sge = 1;

    rdma_create_qp(cm_id, pd, &qp_attr);

    struct rdma_conn_param conn_param = {0};
    conn_param.initiator_depth = 1;
    conn_param.responder_resources = 1;
    conn_param.retry_count = 7;
    rdma_connect(cm_id, &conn_param);
    rdma_get_cm_event(ec, &event);
    rdma_ack_cm_event(event);

    msg = malloc(MSG_SIZE);
    mr = ibv_reg_mr(pd, msg, MSG_SIZE, IBV_ACCESS_LOCAL_WRITE);

    memset(&sge, 0, sizeof(sge));
    sge.addr = (uintptr_t)msg;
    sge.length = MSG_SIZE;
    sge.lkey = mr->lkey;

    memset(&wr, 0, sizeof(wr));
    wr.wr_id = (uintptr_t)msg;
    wr.opcode = IBV_WR_SEND;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.send_flags = IBV_SEND_SIGNALED;

    char input[MSG_SIZE];
    while (1) {
        printf("Enter message to send: ");
        if (!fgets(input, MSG_SIZE, stdin))
            break;

        strcpy(msg, input);

        memset(&wr, 0, sizeof(wr));
        wr.wr_id = (uintptr_t)msg;
        wr.opcode = IBV_WR_SEND;
        wr.sg_list = &sge;
        wr.num_sge = 1;
        wr.send_flags = IBV_SEND_SIGNALED;

        ibv_post_send(cm_id->qp, &wr, &bad_wr);

        ibv_get_cq_event(comp_chan, &cq, &cq_context);
        ibv_ack_cq_events(cq, 1);
        ibv_req_notify_cq(cq, 0);

        while (ibv_poll_cq(cq, 1, &wc) == 0);
        if (wc.status != IBV_WC_SUCCESS) {
            fprintf(stderr, "Send failed: %s\n", ibv_wc_status_str(wc.status));
            break;
        } else {
            printf("Sent: %s", msg);
        }

        if (strcmp(input, "exit\n") == 0)
            break;
    }

    // Cleanup
    rdma_disconnect(cm_id);
    rdma_destroy_qp(cm_id);
    ibv_dereg_mr(mr);
    free(msg);
    ibv_destroy_cq(cq);
    ibv_destroy_comp_channel(comp_chan);
    ibv_dealloc_pd(pd);
    rdma_destroy_id(cm_id);
    rdma_destroy_event_channel(ec);

    return 0;
}
