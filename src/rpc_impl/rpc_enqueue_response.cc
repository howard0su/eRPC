#include "rpc.h"

namespace ERpc {

template <class TTr>
void Rpc<TTr>::enqueue_response(ReqHandle *req_handle) {
  assert(req_handle != nullptr);
  SSlot *sslot = (SSlot *)req_handle;

  Session *session = sslot->session;
  assert(session->is_server());
  // The client has a pending request, so the session can't be disconnected
  assert(session->is_connected());

  // Sanity-check the request MsgBuffer. The switch below gets optimized out.
  MsgBuffer *rx_msgbuf = &sslot->rx_msgbuf;
  _unused(rx_msgbuf);
  switch (sslot->req_func_type) {
    case ReqFuncType::kFgTerminal:
      // Response is enqueued before the request handler returns, and the event
      // loop will bury rx_msgbuf after the handler returns.
      assert(rx_msgbuf->buf != nullptr && rx_msgbuf->check_magic());
      break;
    case ReqFuncType::kFgNonterminal:
      // Response is enqueued before (or after) the request handler returns, and
      // the event loop will bury or (has buried) rx_msgbuf after the handler
      // returns
      break;
    case ReqFuncType::kBackground:
      // Background threads can't bury MsgBuffers, so we'll bury rx_msgbuf below
      assert(rx_msgbuf->buf != nullptr && rx_msgbuf->is_dynamic() &&
             rx_msgbuf->check_magic());
      break;
  }

  if (small_rpc_unlikely(sslot->req_func_type == ReqFuncType::kBackground)) {
    // Event loop frees rx_msgbuf in other cases
    bury_sslot_rx_msgbuf(sslot);
  }

  MsgBuffer *resp_msgbuf;
  if (small_rpc_likely(sslot->prealloc_used)) {
    resp_msgbuf = &sslot->pre_resp_msgbuf;
  } else {
    resp_msgbuf = &sslot->dyn_resp_msgbuf;
  }

  // Sanity-check resp_msgbuf
  assert(resp_msgbuf->buf != nullptr && resp_msgbuf->check_magic());
  assert(resp_msgbuf->data_size > 0);

  // Step 1: Fill in packet 0's header
  pkthdr_t *resp_pkthdr_0 = resp_msgbuf->get_pkthdr_0();
  resp_pkthdr_0->req_type = sslot->rx_msgbuf_saved.req_type;
  resp_pkthdr_0->msg_size = resp_msgbuf->data_size;
  resp_pkthdr_0->dest_session_num = session->remote_session_num;
  resp_pkthdr_0->pkt_type = kPktTypeResp;

  if (small_rpc_likely(sslot->req_func_type == ReqFuncType::kFgTerminal)) {
    // Fg terminal req function: 1st resp packet is Expected
    resp_pkthdr_0->is_unexp = 0;
    resp_pkthdr_0->fgt_resp = 1;
  } else {
    // Non-foreground-terminal: all resp packets are Unexpected
    resp_pkthdr_0->is_unexp = 1;
    resp_pkthdr_0->fgt_resp = 0;
  }

  resp_pkthdr_0->pkt_num = 0;
  resp_pkthdr_0->req_num = sslot->rx_msgbuf_saved.req_num;
  assert(resp_pkthdr_0->check_magic());

  // Step 2: Fill in non-zeroth packet headers, if any
  if (small_rpc_unlikely(resp_msgbuf->num_pkts > 1)) {
    // Headers for non-zeroth packets are created by copying the 0th header, and
    // changing only the required fields.
    //
    // All non-first response packets are Unexpected, for both foreground and
    // background request handlers.
    for (size_t i = 1; i < resp_msgbuf->num_pkts; i++) {
      pkthdr_t *resp_pkthdr_i = resp_msgbuf->get_pkthdr_n(i);
      *resp_pkthdr_i = *resp_pkthdr_0;
      resp_pkthdr_i->pkt_num = i;
      resp_pkthdr_i->is_unexp = 1;
    }
  }

  // Step 3: Fill in the slot, reset queueing progress, and upsert session
  sslot->tx_msgbuf = resp_msgbuf;  // Valid response
  sslot->tx_msgbuf->pkts_queued = 0;

  dpath_txq_push_back(sslot);  // Thread-safe
}

}  // End ERpc