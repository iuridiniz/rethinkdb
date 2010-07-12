
#ifndef __FSM_TCC__
#define __FSM_TCC__

#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include "utils.hpp"
#include "request_handler/memcached_handler.hpp"

template<class config_t>
void conn_fsm<config_t>::init_state() {
    this->state = fsm_socket_connected;
    this->rbuf = NULL;
    this->sbuf = NULL;
    this->nrbuf = 0;
    this->nsbuf = 0;
}

// This function returns the socket to clean connected state
template<class config_t>
void conn_fsm<config_t>::return_to_socket_connected() {
    if(this->rbuf)
        delete (iobuf_t*)(this->rbuf);
    if(this->sbuf)
        delete (iobuf_t*)(this->sbuf);
    init_state();
}

// This state represents a connected socket with no outstanding
// operations. Incoming events should be user commands received by the
// socket.
template<class config_t>
typename conn_fsm<config_t>::result_t conn_fsm<config_t>::do_socket_ready(event_t *event) {
    ssize_t sz;
    conn_fsm *state = (conn_fsm*)event->state;
    assert(state == this);

    if(event->event_type == et_sock) {
        if(state->rbuf == NULL) {
            state->rbuf = (char *)new iobuf_t();
            state->nrbuf = 0;
        }
        if(state->sbuf == NULL) {
            state->sbuf = (char *)new iobuf_t();
            state->nsbuf = 0;
        }
            
        // TODO: we assume the command will fit comfortably into
        // IO_BUFFER_SIZE. We'll need to implement streaming later.

        for (;;) {
            sz = io_calls_t::read(state->source,
                                  state->rbuf + state->nrbuf,
                                  iobuf_t::size - state->nrbuf);
            if(sz == -1) {
                if(errno == EAGAIN || errno == EWOULDBLOCK) {
                    // The machine can't be in
                    // fsm_socket_send_incomplete state here,
                    // since we break out in these cases. So it's
                    // safe to free the buffer.
                    if(state->state != conn_fsm::fsm_socket_recv_incomplete)
                        return_to_socket_connected();
                    break;
                } else {
                    check("Could not read from socket", sz == -1);
                }
            } else if(sz > 0) {
                state->nrbuf += sz;
                typename req_handler_t::parse_result_t handler_res =
                    req_handler->parse_request(event);
                switch(handler_res) {
                case req_handler_t::op_malformed:
                    // Command wasn't processed correctly, send error
                    // Error should already be placed in buffer by parser
                    send_msg_to_client();
                    break;
                case req_handler_t::op_partial_packet:
                    // The data is incomplete, keep trying to read in
                    // the current read loop
                    state->state = conn_fsm::fsm_socket_recv_incomplete;
                    break;
                case req_handler_t::op_req_shutdown:
                    // Shutdown has been initiated
                    return fsm_shutdown_server;
                case req_handler_t::op_req_quit:
                    // The connection has been closed
                    return fsm_quit_connection;
                case req_handler_t::op_req_complex:
                    // Ain't nothing we can do now - the operations
                    // have been distributed accross CPUs. We can just
                    // sit back and wait until they come back.
                    assert(current_request);
                    state->state = fsm_btree_incomplete;
                    return fsm_transition_ok;
                    break;
                default:
                    check("Unknown request parse result", 1);
                }

                if(state->state == conn_fsm::fsm_socket_send_incomplete) {
                    // Wait for the socket to finish sending
                    break;
                }
            } else {
                // Socket has been closed, destroy the connection
                return fsm_quit_connection;
                    
                // TODO: what if the fsm is not in a finished
                // state? What if we free it during an AIO
                // request, and the AIO request comes back? We
                // need an fsm_terminated flag for these cases.

                // TODO: what about application-level keepalive?
            }
        } 
    } else {
        check("fsm_socket_ready: Invalid event", 1);
    }

    return fsm_transition_ok;
}

template<class config_t>
typename conn_fsm<config_t>::result_t conn_fsm<config_t>::do_fsm_btree_incomplete(event_t *event)
{
    if(event->event_type == et_sock) {
        // We're not going to process anything else from the socket
        // until we complete the currently executing command.

        // TODO: This strategy destroys any possibility of pipelining
        // commands on a single socket. We should enable this in the
        // future (fsm would need to associate IO responses with a
        // given command).
    } else if(event->event_type == et_request_complete) {
        send_msg_to_client();
        if(this->state != conn_fsm::fsm_socket_send_incomplete) {
            state = fsm_btree_complete;
        }
    } else {
        check("fsm_btree_incomplete: Invalid event", 1);
    }
    
    return fsm_transition_ok;
}

// The socket is ready for sending more information and we were in the
// middle of an incomplete send request.
template<class config_t>
typename conn_fsm<config_t>::result_t conn_fsm<config_t>::do_socket_send_incomplete(event_t *event) {
    // TODO: incomplete send needs to be tested therally. It's not
    // clear how to get the kernel to artifically limit the send
    // buffer.
    if(event->event_type == et_sock) {
        if(event->op == eo_rdwr || event->op == eo_write) {
            send_msg_to_client();
        }
        if(this->state != conn_fsm::fsm_socket_send_incomplete) {
            state = fsm_btree_complete;
        }
    } else {
        check("fsm_socket_send_ready: Invalid event", 1);
    }
    return fsm_transition_ok;
}

//We've processed a request but there are still outstanding requests in our rbuf
template<class config_t>
        typename conn_fsm<config_t>::result_t conn_fsm<config_t>::do_fsm_outstanding_req(event_t *event) {
            conn_fsm *state = (conn_fsm*)event->state;
            assert(state == this);
            assert(nrbuf > 0);
                typename req_handler_t::parse_result_t handler_res =
                    req_handler->parse_request(event);
                switch(handler_res) {
                    case req_handler_t::op_malformed:
                        // Command wasn't processed correctly, send error
                        // Error should already be placed in buffer by parser
                        send_msg_to_client();
                        break;
                    case req_handler_t::op_partial_packet:
                        // The data is incomplete, keep trying to read in
                        // the current read loop
                        state->state = conn_fsm::fsm_socket_recv_incomplete;
                        break;
                    case req_handler_t::op_req_shutdown:
                        // Shutdown has been initiated
                        return fsm_shutdown_server;
                    case req_handler_t::op_req_quit:
                        // The connection has been closed
                        return fsm_quit_connection;
                    case req_handler_t::op_req_complex:
                        // Ain't nothing we can do now - the operations
                        // have been distributed accross CPUs. We can just
                        // sit back and wait until they come back.
                        assert(current_request);
                        state->state = fsm_btree_incomplete;
                        return fsm_transition_ok;
                        break;
                    default:
                        check("Unknown request parse result", 1);
                }
                return fsm_transition_ok;
        }

        // Switch on the current state and call the appropriate transition
        // function.
        template<class config_t>
        typename conn_fsm<config_t>::result_t conn_fsm<config_t>::do_transition(event_t *event) {
            // TODO: Using parent_pool member variable within state
            // transitions might cause cache line alignment issues. Can we
            // eliminate it (perhaps by giving each thread its own private
            // copy of the necessary data)?

            result_t res;

            //TODO: as things stand we get an event for when a socket is connected
            //and then allocate and free the buffers, fix this
            switch(state) {
                case fsm_socket_connected:
                case fsm_socket_recv_incomplete:
                case fsm_btree_complete:
                    res = do_socket_ready(event);
                    break;
                case fsm_socket_send_incomplete:
                    res = do_socket_send_incomplete(event);
                    break;
                case fsm_btree_incomplete:
                    res = do_fsm_btree_incomplete(event);
                    break;
                default:
                    res = fsm_invalid;
                    check("Invalid state", 1);
            }
            if (state == fsm_btree_complete) {
                if (nrbuf > 0) {
                    //there's still data in our rbuf, deal with it
                    res = do_fsm_outstanding_req(event);
                } else {
                    event->op = eo_read;
                    event->event_type = et_sock;
                    do_socket_ready(event);
                }
            }
            
    return res;
}

template<class config_t>
conn_fsm<config_t>::conn_fsm(resource_t _source, event_queue_t *_event_queue)
    : source(_source), req_handler(NULL), event_queue(_event_queue)
{
    req_handler = new memcached_handler_t<config_t>(event_queue->cache, event_queue);
    init_state();
}

template<class config_t>
conn_fsm<config_t>::~conn_fsm() {
    close(source);
    delete req_handler;
    if(this->rbuf) {
        delete (iobuf_t*)(this->rbuf);
    }
    if(this->sbuf) {
        delete (iobuf_t*)(this->sbuf);
    }
}

// Send a message to the client. The message should be contained
// within buf (nbuf should be the full size). If state has been
// switched to fsm_socket_send_incomplete, then buf must not be freed
// after the return of this function.
template<class config_t>
void conn_fsm<config_t>::send_msg_to_client() {
    // Either number of bytes already sent should be zero, or we
    // should be in the middle of an incomplete send.
    //assert(this->snbuf == 0 || this->state == conn_fsm::fsm_socket_send_incomplete); TODO equivalent thing for seperate buffers

    int len = this->nsbuf;
    int sz = 0;
    while(len > 0) {
        sz = io_calls_t::write(this->source, this->sbuf, len);
        if(sz < 0) {
            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                // If we can't send the message now, wait 'till we can
                this->state = conn_fsm::fsm_socket_send_incomplete;
                return;
            } else {
                // There was some other error
                check("Couldn't send message to client", sz < 0);
            }
        }
        len -= sz;
    }

    // We've successfully sent everything out
    this->nsbuf = 0;
    this->state = conn_fsm::fsm_socket_connected;
}

template<class config_t>
void conn_fsm<config_t>::send_err_to_client() {
    char err_msg[] = "(ERROR) Unknown command\n";
    strcpy(this->sbuf, err_msg);
    this->snbuf = strlen(err_msg) + 1;
    send_msg_to_client();
}

template<class config_t>
void conn_fsm<config_t>::consume(unsigned int bytes) {
    memmove(this->rbuf, this->rbuf + bytes, this->nrbuf - bytes);
    this->nrbuf -= bytes;
}

#endif // __FSM_TCC__

