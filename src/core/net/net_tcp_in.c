/* net_tcp_in — the input path: parse, find the connection, run the state
 * machine. Everything here reads attacker-supplied bytes, so each field is
 * checked against the segment before it is used and every sequence number
 * is checked against the window before it is believed. */
#include "core/errno.h"
#include "core/net/tcp_internal.h"

/* The pseudo-header sum, as in net_tcp_out.c. */
static uint32_t pseudo_sum(ip4_t src, ip4_t dst, uint32_t len) {
    uint8_t ph[12];
    put32(ph, src);
    put32(ph + 4, dst);
    ph[8] = 0;
    ph[9] = IPPROTO_TCP;
    put16(ph + 10, (uint16_t)len);
    return net_sum(ph, sizeof ph, 0);
}

/* Reads the MSS option, ignoring every other option. Malformed option
 * lists stop the walk rather than being followed. */
static uint32_t parse_mss(const uint8_t *p, uint32_t hlen) {
    for (uint32_t at = TCP_HLEN; at < hlen;) {
        uint8_t kind = p[at];
        uint8_t len;
        if (kind == TCP_OPT_END) {
            return 0;
        }
        if (kind == TCP_OPT_NOP) {
            at++;
            continue;
        }
        if (at + 2u > hlen || (len = p[at + 1]) < 2u || at + len > hlen) {
            return 0;
        }
        if (kind == TCP_OPT_MSS && len == 4u) {
            return get16(p + at + 2);
        }
        at += len;
    }
    return 0;
}

/* Validates the segment and fills seg. KERR_OK or an error. */
static int parse_segment(struct tcp_stack *s, ip4_t src, ip4_t dst, const uint8_t *p, uint32_t len,
                         struct tcp_seg *seg) {
    uint32_t hlen;
    if (len < TCP_HLEN) {
        s->stats.drop_malformed++;
        return KERR_INVAL;
    }
    hlen = (uint32_t)(p[TH_OFF] >> 4) * 4u;
    if (hlen < TCP_HLEN || hlen > len) {
        s->stats.drop_malformed++;
        return KERR_INVAL;
    }
    if (net_checksum(p, len, pseudo_sum(src, dst, len)) != 0) {
        s->stats.drop_checksum++;
        return KERR_FAULT;
    }
    *seg = (struct tcp_seg){.seq = get32(p + TH_SEQ), .ack = get32(p + TH_ACK), .wnd = get16(p + TH_WINDOW),
                            .sport = get16(p + TH_SPORT), .dport = get16(p + TH_DPORT), .flags = p[TH_FLAGS],
                            .mss = (p[TH_FLAGS] & TCP_SYN) ? parse_mss(p, hlen) : 0u, .data = p + hlen,
                            .len = len - hlen};
    return KERR_OK;
}

/* The connection this segment belongs to: an exact four-tuple match. */
static struct tcp_conn *find_exact(struct tcp_stack *s, ip4_t src, const struct tcp_seg *seg) {
    for (uint32_t i = 0; i < TCP_MAX_CONNS; i++) {
        struct tcp_conn *c = &s->conns[i];
        if (c->used && c->state != TCP_LISTEN && c->local_port == seg->dport &&
            c->remote_port == seg->sport && c->remote_ip == src) {
            return c;
        }
    }
    return NULL;
}

/* A listener on the segment's destination port. */
static struct tcp_conn *find_listener(struct tcp_stack *s, ip4_t dst, uint16_t port) {
    for (uint32_t i = 0; i < TCP_MAX_CONNS; i++) {
        struct tcp_conn *c = &s->conns[i];
        if (c->used && c->state == TCP_LISTEN && c->local_port == port &&
            (c->local_ip == 0 || c->local_ip == dst)) {
            return c;
        }
    }
    return NULL;
}

/* Half-open children a listener already has, so a SYN flood is bounded by
 * the backlog rather than by the whole connection table. */
static uint32_t half_open(const struct tcp_stack *s, const struct tcp_conn *listener) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < TCP_MAX_CONNS; i++) {
        const struct tcp_conn *c = &s->conns[i];
        if (c->used && c->listener == listener && c->state == TCP_SYN_RECEIVED) {
            n++;
        }
    }
    return n;
}

/* A SYN for a listener: a child connection in SYN_RECEIVED. */
static void accept_syn(struct tcp_stack *s, struct tcp_conn *listener, ip4_t src, ip4_t dst,
                       const struct tcp_seg *seg) {
    struct tcp_conn *c;
    if (listener->backlog_len + half_open(s, listener) >= listener->backlog_max) {
        s->stats.drop_backlog_full++;
        return;  /* dropping is what makes a SYN flood harmless */
    }
    c = tcp_alloc(s);
    if (c == NULL) {
        s->stats.drop_backlog_full++;
        return;
    }
    c->local_ip = dst;
    c->local_port = seg->dport;
    c->remote_ip = src;
    c->remote_port = seg->sport;
    c->listener = listener;
    c->irs = seg->seq;
    c->rcv_nxt = seg->seq + 1u;
    c->snd_wnd = seg->wnd;
    c->snd_una = s->iss;
    c->snd_nxt = c->snd_una + 1u;
    s->iss += 0x9e3779b9u;
    s->stats.connections++;
    tcp_enter(c, TCP_SYN_RECEIVED);
    tcp_arm_timer(c);
    (void)tcp_emit(c, c->snd_una, TCP_SYN | TCP_ACK, NULL, 0, true);
}

/* The handshake's final ACK: the child joins its listener's backlog. */
static void complete_handshake(struct tcp_conn *c) {
    struct tcp_conn *l = c->listener;
    tcp_cancel_timer(c);
    tcp_enter(c, TCP_ESTABLISHED);
    if (l == NULL) {
        return;
    }
    if (l->backlog_len < TCP_BACKLOG) {
        l->backlog[l->backlog_len++] = c;
        tcp_changed(l);
    } else {
        tcp_free(c);
    }
}

/* SYN_SENT: a SYN-ACK completes our open, a RST refuses it. */
static void in_syn_sent(struct tcp_conn *c, const struct tcp_seg *seg) {
    if (seg->flags & TCP_RST) {
        if (seg->flags & TCP_ACK) {
            tcp_fail(c, TCP_ECONNREFUSED);
        }
        return;
    }
    if (!(seg->flags & TCP_SYN) || !(seg->flags & TCP_ACK)) {
        return;
    }
    if (seg->ack != c->snd_nxt) {
        tcp_reset_peer(c->stack, c->remote_ip, c->local_port, c->remote_port, seg->ack, 0, false);
        return;
    }
    c->irs = seg->seq;
    c->rcv_nxt = seg->seq + 1u;
    c->snd_una = seg->ack;
    c->snd_wnd = seg->wnd;
    tcp_cancel_timer(c);
    tcp_enter(c, TCP_ESTABLISHED);
    tcp_send_ack(c);
    tcp_flush(c);
}

/* True when the segment's data (or its bare sequence number) lies inside
 * the receive window: RFC 9293 §3.4's acceptability test. */
static bool acceptable(const struct tcp_conn *c, const struct tcp_seg *seg) {
    uint32_t wnd = tcp_rcv_wnd(c);
    if (seg->len == 0) {
        return wnd == 0 ? seg->seq == c->rcv_nxt
                        : seq_ge(seg->seq, c->rcv_nxt) && seq_lt(seg->seq, c->rcv_nxt + wnd);
    }
    if (wnd == 0) {
        return false;
    }
    return (seq_ge(seg->seq, c->rcv_nxt) && seq_lt(seg->seq, c->rcv_nxt + wnd)) ||
           (seq_lt(seg->seq, c->rcv_nxt) && seq_gt(seg->seq + seg->len, c->rcv_nxt));
}

/* A reset is believed only when it is exactly what we expect next: an
 * off-path attacker guessing sequence numbers must not kill connections. */
static bool reset_is_valid(const struct tcp_conn *c, const struct tcp_seg *seg) {
    return seg->seq == c->rcv_nxt;
}

/* Applies an acknowledgement: drops acknowledged bytes from the queue. */
static void apply_ack(struct tcp_conn *c, const struct tcp_seg *seg) {
    uint32_t acked;
    if (!(seg->flags & TCP_ACK) || seq_le(seg->ack, c->snd_una) || seq_gt(seg->ack, c->snd_nxt)) {
        c->snd_wnd = (seg->flags & TCP_ACK) ? seg->wnd : c->snd_wnd;
        return;
    }
    acked = seg->ack - c->snd_una;
    c->snd_una = seg->ack;
    c->snd_wnd = seg->wnd;
    c->retries = 0;
    c->rto_ms = TCP_RTO_MIN_MS;
    if (acked > c->tx_len) {
        acked = c->tx_len;  /* the rest acknowledged our SYN or FIN */
    }
    for (uint32_t i = acked; i < c->tx_len; i++) {
        c->tx[i - acked] = c->tx[i];
    }
    c->tx_len -= acked;
    c->rto_at_ms = 0;
    if (tcp_in_flight(c) > 0) {
        tcp_arm_timer(c);
    }
}

/* Takes in-order payload into the receive queue. Out-of-order data is
 * dropped: the peer will resend it (there is no reassembly queue). */
static void take_data(struct tcp_conn *c, const struct tcp_seg *seg) {
    uint32_t skip, n;
    if (seg->len == 0) {
        return;
    }
    if (seq_gt(seg->seq, c->rcv_nxt)) {
        c->stack->stats.drop_out_of_order++;
        tcp_send_ack(c);  /* tell the peer what we are waiting for */
        return;
    }
    skip = c->rcv_nxt - seg->seq;  /* bytes we already have */
    if (skip >= seg->len) {
        tcp_send_ack(c);
        return;
    }
    n = seg->len - skip;
    n = n < tcp_rcv_wnd(c) ? n : tcp_rcv_wnd(c);
    for (uint32_t i = 0; i < n; i++) {
        c->rx[c->rx_len + i] = seg->data[skip + i];
    }
    c->rx_len += n;
    c->rcv_nxt += n;
    tcp_send_ack(c);
    tcp_changed(c);
}

/* The peer's FIN, once every byte before it has arrived. */
static void take_fin(struct tcp_conn *c, const struct tcp_seg *seg) {
    if (!(seg->flags & TCP_FIN) || seg->seq + seg->len != c->rcv_nxt) {
        return;
    }
    c->fin_seen = true;
    c->rcv_nxt++;
    tcp_send_ack(c);
    switch (c->state) {
    case TCP_ESTABLISHED: tcp_enter(c, TCP_CLOSE_WAIT); break;
    case TCP_FIN_WAIT_1: tcp_enter(c, TCP_CLOSING); break;
    case TCP_FIN_WAIT_2: tcp_enter(c, TCP_TIME_WAIT); break;
    default: break;
    }
}

/* Our FIN was acknowledged: move on in the closing sequence. */
static void check_our_fin(struct tcp_conn *c) {
    bool fin_acked = c->fin_queued && c->tx_len == 0 && tcp_in_flight(c) == 0;
    if (!fin_acked) {
        return;
    }
    if (c->state == TCP_FIN_WAIT_1) {
        tcp_enter(c, TCP_FIN_WAIT_2);
    } else if (c->state == TCP_CLOSING || c->state == TCP_LAST_ACK) {
        tcp_cancel_timer(c);
        tcp_enter(c, c->state == TCP_CLOSING ? TCP_TIME_WAIT : TCP_CLOSED);
    }
}

/* The control flags of an acceptable segment. Returns false when the
 * segment ended the connection and nothing more should be done with it. */
static bool handle_control(struct tcp_conn *c, const struct tcp_seg *seg) {
    if (seg->flags & TCP_RST) {
        if (!reset_is_valid(c, seg)) {
            c->stack->stats.drop_bad_reset++;
            return false;
        }
        tcp_fail(c, c->state == TCP_SYN_RECEIVED ? TCP_ECONNREFUSED : TCP_ECONNRESET);
        return false;
    }
    if (seg->flags & TCP_SYN) {
        tcp_fail(c, TCP_ECONNRESET);  /* a SYN inside the window is an error */
        tcp_reset_peer(c->stack, c->remote_ip, c->local_port, c->remote_port, c->snd_nxt, c->rcv_nxt, true);
        return false;
    }
    return true;
}

/* The handshake's last ACK, for a connection a listener created. Returns
 * false when the acknowledgement was wrong and the peer was reset. */
static bool finish_passive_open(struct tcp_conn *c, const struct tcp_seg *seg) {
    if (seg->ack != c->snd_nxt) {
        tcp_reset_peer(c->stack, c->remote_ip, c->local_port, c->remote_port, seg->ack, 0, false);
        return false;
    }
    c->snd_una = seg->ack;
    c->snd_wnd = seg->wnd;
    complete_handshake(c);
    return true;
}

/* Every state from SYN_RECEIVED onwards, which share the sequence checks. */
static void in_synchronised(struct tcp_conn *c, const struct tcp_seg *seg) {
    if (!acceptable(c, seg)) {
        c->stack->stats.drop_out_of_window++;
        if (!(seg->flags & TCP_RST)) {
            tcp_send_ack(c);
        }
        return;
    }
    if (!handle_control(c, seg) || !(seg->flags & TCP_ACK)) {
        return;
    }
    if (c->state == TCP_SYN_RECEIVED && !finish_passive_open(c, seg)) {
        return;
    }
    apply_ack(c, seg);
    take_data(c, seg);
    take_fin(c, seg);
    check_our_fin(c);
    tcp_flush(c);
}

/* A segment for nothing that exists gets a reset, so the peer learns at
 * once instead of retrying. A reset is never answered with a reset. */
static void reject(struct tcp_stack *s, ip4_t src, const struct tcp_seg *seg) {
    s->stats.drop_no_socket++;
    if (seg->flags & TCP_RST) {
        return;
    }
    if (seg->flags & TCP_ACK) {
        tcp_reset_peer(s, src, seg->dport, seg->sport, seg->ack, 0, false);
    } else {
        tcp_reset_peer(s, src, seg->dport, seg->sport, 0, seg->seq + seg->len + 1u, true);
    }
}

/* The ip_handler entry point. */
void tcp_input_packet(void *self, struct netif *nif, ip4_t src, ip4_t dst, const uint8_t *p, uint32_t len) {
    struct tcp_stack *s = self;
    struct tcp_seg seg;
    struct tcp_conn *c;
    (void)nif;
    s->stats.segments_in++;
    if (parse_segment(s, src, dst, p, len, &seg) != KERR_OK) {
        return;
    }
    c = find_exact(s, src, &seg);
    if (c == NULL) {
        struct tcp_conn *l = find_listener(s, dst, seg.dport);
        if (l != NULL && (seg.flags & TCP_SYN) && !(seg.flags & TCP_ACK) && !(seg.flags & TCP_RST)) {
            accept_syn(s, l, src, dst, &seg);
        } else {
            reject(s, src, &seg);
        }
        return;
    }
    if (c->state == TCP_SYN_SENT) {
        in_syn_sent(c, &seg);
    } else if (c->state != TCP_CLOSED) {
        in_synchronised(c, &seg);
    }
}
