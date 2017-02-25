/**
 * @file rpc_datapath.cc
 * @brief Performance-critical Rpc datapath functions
 */

#include <iostream>
#include <stdexcept>

#include "rpc.h"
#include "util/udp_client.h"

namespace ERpc {

template <class Transport_>
int Rpc<Transport_>::send_request(Session *session, uint8_t req_type,
                                  Buffer pkt_buffer, size_t msg_size) {
  if (!kDatapathChecks) {
    assert(session != nullptr && session->role == Session::Role::kClient);
    assert(session->state == SessionState::kConnected);
    assert(pkt_buffer.is_valid() && check_pkthdr(pkt_buffer));
    assert(msg_size > 0 && msg_size <= kMaxMsgSize);
  } else {
    /* If datapath checks are enabled, return meaningful error codes */
    if (unlikely(session == nullptr ||
                 session->role != Session::Role::kClient)) {
      return static_cast<int>(RpcDatapathErrCode::kInvalidSessionArg);
    }

    if (unlikely(session->state != SessionState::kConnected)) {
      return static_cast<int>(RpcDatapathErrCode::kInvalidSessionArg);
    }

    if (unlikely(!pkt_buffer.is_valid() || !check_pkthdr(pkt_buffer))) {
      return static_cast<int>(RpcDatapathErrCode::kInvalidPktBufferArg);
    }

    if (unlikely(msg_size == 0 || msg_size > kMaxMsgSize)) {
      return static_cast<int>(RpcDatapathErrCode::kInvalidPktBufferArg);
    }
  }

  if (session->msg_arr_free_vec.size() == 0) {
    /* No free message slots left in session, so we can't queue this request */
    return static_cast<int>(RpcDatapathErrCode::kNoSessionMsgSlots);
  }

  // Fill in the packet header
  pkthdr_t *pkthdr = pkt_buffer_hdr(pkt_buffer);
  pkthdr->req_type = req_type;
  pkthdr->msg_size = msg_size;
  pkthdr->rem_session_num = session->server.session_num;
  pkthdr->is_req = 1;
  pkthdr->is_first = 1;
  pkthdr->is_expected = 0;
  pkthdr->pkt_num = 0;
  /* pkthdr->magic is already filled in */

  /* Find a free message slot in the session */
  size_t msg_arr_slot = session->msg_arr_free_vec.pop_back();
  assert(msg_arr_slot < Session::kSessionReqWindow);
  assert(!session->msg_arr[msg_arr_slot].in_use);

  /* Generate a request number for this slot */
  pkthdr->req_num =
      (req_num_arr[msg_arr_slot] * Session::kSessionReqWindow) + /* Bit shift */
      msg_arr_slot;
  req_num_arr[msg_arr_slot]++;

  /* Fill in the session message slot */
  session->msg_arr[msg_arr_slot].pkt_buffer = pkt_buffer;
  session->msg_arr[msg_arr_slot].msg_size = msg_size;
  session->msg_arr[msg_arr_slot].msg_bytes_sent = 0;
  session->msg_arr[msg_arr_slot].in_use = true;

  /* Add \p session to the work queue if it's not already present */
  if (!session->in_datapath_work_queue) {
    session->in_datapath_work_queue = true;
    datapath_work_queue.push_back(session);
  }

  return 0;
}

}  // End ERpc