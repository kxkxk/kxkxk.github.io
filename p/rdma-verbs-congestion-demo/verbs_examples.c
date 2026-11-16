/*
 * Traditional userspace libibverbs teaching helpers, C11.
 * No main(), connection exchange, resource allocation, or worker threads.
 * See ../02-ibverbs接口与通信流程.md and ../03-代码示例与实验排错.md.
 *
 * Preconditions: ordinary ibv_reg_mr() host-memory MRs (no zero-based/IOVA
 * remapping), RC QP, matching PDs, valid permissions, negotiated capabilities,
 * and buffers kept alive until all relevant operations have finished.
 * Each function returns 0 on success or a positive error code.
 * A successful post is submission only. A poll timeout does NOT cancel a WR.
 */
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <infiniband/verbs.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

/* Host-order teaching descriptor. Serialize fields explicitly for transport. */
struct demo_remote_region {
    uint64_t addr;
    uint64_t length;
    uint32_t rkey;
};

int demo_qp_init(struct ibv_qp *qp, uint8_t port, uint16_t pkey_index,
                 unsigned int remote_access)
{
    struct ibv_qp_attr attr = {0};

    if (!qp || !port)
        return EINVAL;
    attr.qp_state = IBV_QPS_INIT;
    attr.port_num = port;
    attr.pkey_index = pkey_index;
    attr.qp_access_flags = remote_access;
    return ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_PORT |
                         IBV_QP_PKEY_INDEX | IBV_QP_ACCESS_FLAGS);
}

/* path must already describe this host's route to the peer. */
int demo_qp_rtr(struct ibv_qp *qp, const struct ibv_ah_attr *path,
                enum ibv_mtu mtu, uint32_t peer_qpn, uint32_t peer_psn,
                uint8_t responder_depth)
{
    struct ibv_qp_attr attr = {0};

    if (!qp || !path || peer_psn > 0xffffffU)
        return EINVAL;
    attr.qp_state = IBV_QPS_RTR;
    attr.ah_attr = *path;
    attr.path_mtu = mtu;
    attr.dest_qp_num = peer_qpn;
    attr.rq_psn = peer_psn;
    attr.max_dest_rd_atomic = responder_depth;
    attr.min_rnr_timer = 12; /* Illustrative encoded value, not milliseconds. */
    return ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_AV |
                         IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                         IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER);
}

int demo_qp_rts(struct ibv_qp *qp, uint32_t local_psn,
                uint8_t initiator_depth)
{
    struct ibv_qp_attr attr = {0};

    if (!qp || local_psn > 0xffffffU)
        return EINVAL;
    attr.qp_state = IBV_QPS_RTS;
    attr.sq_psn = local_psn;
    attr.max_rd_atomic = initiator_depth;
    attr.timeout = 14;   /* Illustrative ACK timeout encoding. */
    attr.retry_cnt = 6;
    attr.rnr_retry = 6;  /* Finite retries; 7 has special infinite semantics. */
    return ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_SQ_PSN |
                         IBV_QP_MAX_QP_RD_ATOMIC | IBV_QP_TIMEOUT |
                         IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY);
}

static int demo_sge(struct ibv_mr *mr, const void *buf, uint32_t length,
                    struct ibv_sge *sge)
{
    uintptr_t base;
    uintptr_t address;
    uintptr_t offset;

    if (!mr || !buf || !sge || !length)
        return EINVAL;
    base = (uintptr_t)mr->addr;
    address = (uintptr_t)buf;
    if (address < base)
        return EINVAL;
    offset = address - base;
    if (offset > mr->length || length > mr->length - offset)
        return EINVAL;
    sge->addr = (uint64_t)address;
    sge->length = length;
    sge->lkey = mr->lkey;
    return 0;
}

static int demo_remote_address(const struct demo_remote_region *remote,
                                uint64_t offset, uint32_t length,
                                uint64_t *address)
{
    if (!remote || !address || offset > remote->length ||
        length > remote->length - offset ||
        offset > UINT64_MAX - remote->addr)
        return EINVAL;
    *address = remote->addr + offset;
    if (length && (uint64_t)(length - 1) > UINT64_MAX - *address)
        return EINVAL;
    return 0;
}

int demo_post_recv(struct ibv_qp *qp, struct ibv_mr *mr, void *buf,
                   uint32_t capacity, uint64_t id)
{
    struct ibv_sge sge = {0};
    struct ibv_recv_wr wr = {0};
    struct ibv_recv_wr *bad_wr = NULL;
    int rc;

    if (!qp)
        return EINVAL;
    rc = demo_sge(mr, buf, capacity, &sge);
    if (rc)
        return rc;
    wr.wr_id = id;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    /* Receiver MR needs IBV_ACCESS_LOCAL_WRITE. */
    return ibv_post_recv(qp, &wr, &bad_wr);
}

/* RC WRITE_WITH_IMM notification only; not a data-bearing SEND receive. */
int demo_post_notification(struct ibv_qp *qp, uint64_t id)
{
    struct ibv_recv_wr wr = {0};
    struct ibv_recv_wr *bad_wr = NULL;

    if (!qp)
        return EINVAL;
    wr.wr_id = id;
    return ibv_post_recv(qp, &wr, &bad_wr);
}

int demo_post_send(struct ibv_qp *qp, struct ibv_mr *mr, const void *buf,
                   uint32_t length, uint64_t id)
{
    struct ibv_sge sge = {0};
    struct ibv_send_wr wr = {0};
    struct ibv_send_wr *bad_wr = NULL;
    int rc;

    if (!qp)
        return EINVAL;
    rc = demo_sge(mr, buf, length, &sge);
    if (rc)
        return rc;
    wr.wr_id = id;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_SEND;
    wr.send_flags = IBV_SEND_SIGNALED;
    return ibv_post_send(qp, &wr, &bad_wr);
}

/*
 * WRITE/WRITE_WITH_IMM: local_buf is source, remote is destination.
 * READ: local_buf is destination, remote is source.
 * WRITE_WITH_IMM also requires an available receive WR on the peer.
 */
int demo_post_rdma(struct ibv_qp *qp, struct ibv_mr *local_mr,
                   void *local_buf, uint32_t length,
                   const struct demo_remote_region *remote,
                   uint64_t remote_offset, enum ibv_wr_opcode opcode,
                   uint32_t immediate_host_order, uint64_t id)
{
    struct ibv_sge sge = {0};
    struct ibv_send_wr wr = {0};
    struct ibv_send_wr *bad_wr = NULL;
    uint64_t address;
    int rc;

    if (!qp || (opcode != IBV_WR_RDMA_WRITE &&
                opcode != IBV_WR_RDMA_WRITE_WITH_IMM &&
                opcode != IBV_WR_RDMA_READ))
        return EINVAL;
    rc = demo_sge(local_mr, local_buf, length, &sge);
    if (rc)
        return rc;
    rc = demo_remote_address(remote, remote_offset, length, &address);
    if (rc)
        return rc;
    wr.wr_id = id;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = opcode;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.wr.rdma.remote_addr = address;
    wr.wr.rdma.rkey = remote->rkey;
    if (opcode == IBV_WR_RDMA_WRITE_WITH_IMM)
        wr.imm_data = htonl(immediate_host_order);
    return ibv_post_send(qp, &wr, &bad_wr);
}

/* The old remote value is DMA-written to local_result after completion. */
int demo_post_atomic(struct ibv_qp *qp, struct ibv_mr *result_mr,
                      uint64_t *local_result,
                      const struct demo_remote_region *remote,
                      uint64_t offset, enum ibv_wr_opcode opcode,
                      uint64_t compare_or_add, uint64_t swap, uint64_t id)
{
    struct ibv_sge sge = {0};
    struct ibv_send_wr wr = {0};
    struct ibv_send_wr *bad_wr = NULL;
    uint64_t address;
    int rc;

    if (!qp || !local_result || ((uintptr_t)local_result & 7U) ||
        (opcode != IBV_WR_ATOMIC_CMP_AND_SWP &&
         opcode != IBV_WR_ATOMIC_FETCH_AND_ADD))
        return EINVAL;
    rc = demo_sge(result_mr, local_result, sizeof(*local_result), &sge);
    if (rc)
        return rc;
    rc = demo_remote_address(remote, offset, sizeof(*local_result), &address);
    if (rc || (address & 7U))
        return rc ? rc : EINVAL;
    wr.wr_id = id;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = opcode;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.wr.atomic.remote_addr = address;
    wr.wr.atomic.rkey = remote->rkey;
    wr.wr.atomic.compare_add = compare_or_add;
    wr.wr.atomic.swap = swap; /* Ignored for FETCH_AND_ADD. */
    return ibv_post_send(qp, &wr, &bad_wr);
}

static int demo_now_ms(int64_t *value)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now))
        return errno;
    *value = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
    return 0;
}

/*
 * Busy polls one WC and never discards an unrelated completion.
 * Caller must dispatch by wc->wr_id (and qp_num for a shared CQ).
 * On a WC error, wc remains available but only the documented error fields
 * are valid. EIO is a helper-level error; see wc->status for verbs details.
 */
int demo_poll_one(struct ibv_cq *cq, int timeout_ms, struct ibv_wc *wc)
{
    int64_t start;
    int64_t now;
    int rc;

    if (!cq || !wc || timeout_ms <= 0)
        return EINVAL;
    rc = demo_now_ms(&start);
    if (rc)
        return rc;
    for (;;) {
        int count = ibv_poll_cq(cq, 1, wc);

        if (count < 0)
            return EIO;
        if (count == 1) {
            if (wc->status != IBV_WC_SUCCESS) {
                fprintf(stderr, "WC failed: id=%" PRIu64
                        " qp=%u status=%s vendor=%u\n",
                        wc->wr_id, wc->qp_num,
                        ibv_wc_status_str(wc->status), wc->vendor_err);
                return EIO;
            }
            return 0;
        }
        rc = demo_now_ms(&now);
        if (rc)
            return rc;
        if (now - start >= timeout_ms)
            return ETIMEDOUT;
    }
}
