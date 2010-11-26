/*
Cubesat Space Protocol - A small network-layer protocol designed for Cubesats
Copyright (C) 2010 GomSpace ApS (gomspace.com)
Copyright (C) 2010 AAUSAT3 Project (aausat3.space.aau.dk)

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

/**
 * csp_rdp.c
 *
 * @date: 24/06/2010
 * @author: Johan Christiansen
 *
 * This is a implementation of the seq/ack handling taken from the Reliable Datagram Protocol (RDP)
 * For more information read RFC-908. The implementation has been extended to include support for
 * delayed acknowledgements.
 *
 * TODO: Take wrapping sequence/ack numbers into account
 * TODO: Take wrapping timestamps from csp_get_ms into account
 *
 */

#include <stdio.h>
#include <string.h>

#include <csp/csp.h>
#include <csp/csp_config.h>
#include <csp/csp_endian.h>
#include "../arch/csp_queue.h"
#include "../arch/csp_semaphore.h"
#include "../arch/csp_malloc.h"
#include "../arch/csp_time.h"
#include "../csp_port.h"
#include "../csp_conn.h"
#include "../csp_io.h"
#include "csp_transport.h"

#if CSP_USE_RDP

static unsigned int csp_rdp_window_size = 10;
static unsigned int csp_rdp_conn_timeout = 10000;
static unsigned int csp_rdp_packet_timeout = 1000;
static unsigned int csp_rdp_delayed_acks = 1;
static unsigned int csp_rdp_ack_timeout = 1000 / 2;
static unsigned int csp_rdp_ack_delay_count = 10 / 2;

/* Used for queue calls */
static CSP_BASE_TYPE pdTrue = 1;

typedef struct __attribute__((__packed__)) {
    uint8_t padding1[40];       // Interface dependent padding
    uint32_t timestamp;			// Time the message was sent
    uint16_t length;            // Length field must be just before CSP ID
    csp_id_t id;                // CSP id must be just before data
    uint8_t data[];				// This just points to the rest of the buffer, without a size indication.
} rdp_packet_t;

enum csp_rdp_states {
	RDP_CLOSED = 0,
	RDP_LISTEN,
	RDP_SYN_SENT,
	RDP_SYN_RCVD,
	RDP_OPEN,
	RDP_CLOSE_WAIT,
};

typedef struct __attribute__((__packed__)) rdp_header_s {
#if !CSP_RDP_COMP
	union __attribute__((__packed__)) {
		uint8_t flags;
		struct __attribute__((__packed__)) {
#if defined(_CSP_BIG_ENDIAN_) && !defined(_CSP_LITTLE_ENDIAN_)
			unsigned int res : 4;
			unsigned int syn : 1;
			unsigned int ack : 1;
			unsigned int eak : 1;
			unsigned int rst : 1;
#elif defined(_CSP_LITTLE_ENDIAN_) && !defined(_CSP_BIG_ENDIAN_)
			unsigned int rst : 1;
			unsigned int eak : 1;
			unsigned int ack : 1;
			unsigned int syn : 1;
			unsigned int res : 4;
#else
  #error "Must define one of _CSP_BIG_ENDIAN_ or _CSP_LITTLE_ENDIAN_ in csp_platform.h"
#endif
		};
	};
#else
	uint8_t syn;
	uint8_t ack;
	uint8_t eak;
	uint8_t rst;
#endif
	uint16_t seq_nr;
	uint16_t ack_nr;
} rdp_header_t;

/**
 * LOCKING:
 * The RDP protocol stack operates on data that is dynamically allocated.
 * Therefore, if another task calls csp_rdp_close() while RDP may be working
 * on a connection, it may dereference a null pointer. The consequence is
 * to lock the entire RDP stack, so it can only work on one connection at a time.
 * RDP is always called from Task context, so blocking locks are no problem.
 */
static csp_bin_sem_handle_t rdp_lock;
static int rdp_lock_init = 0;

static int inline csp_rdp_wait(unsigned int timeout, csp_conn_t * conn) {

	/* The usual null pointer checking */
	if (conn == NULL)
		return 0;

	/* Init semaphore */
	if (rdp_lock_init == 0) {
		csp_bin_sem_create(&rdp_lock);
		rdp_lock_init = 1;
	}

	/* Nothing in the RDP code should take longer than 1 second = deadlock */
	if (csp_bin_sem_wait(&rdp_lock, timeout) == CSP_SEMAPHORE_ERROR) {
		csp_debug(CSP_ERROR, "Dead-lock in RDP-code found!\r\n");
#if CSP_DEBUG
		csp_conn_print_table();
		csp_buffer_print_table();
#endif
		return 0;
	}

	return 1;

}

static void inline csp_rdp_release(void) {

	if (rdp_lock_init == 1)
		csp_bin_sem_post(&rdp_lock);
	else
		csp_debug(CSP_ERROR, "Attempt to release uninitialized RDP lock\r\n");

}

/**
 * RDP Headers:
 * The following functions are helper functions that handles the extra RDP
 * information that needs to be appended to all data packets.
 */
static rdp_header_t * csp_rdp_header_add(csp_packet_t * packet) {
	rdp_header_t * header = (rdp_header_t *) &packet->data[packet->length];
	packet->length += sizeof(rdp_header_t);
	memset(header, 0, sizeof(rdp_header_t));
	return header;
}

static rdp_header_t * csp_rdp_header_remove(csp_packet_t * packet) {
	rdp_header_t * header = (rdp_header_t *) &packet->data[packet->length-sizeof(rdp_header_t)];
	packet->length -= sizeof(rdp_header_t);
	return header;
}

static rdp_header_t * csp_rdp_header_ref(csp_packet_t * packet) {
	rdp_header_t * header = (rdp_header_t *) &packet->data[packet->length-sizeof(rdp_header_t)];
	return header;
}

/**
 * CONTROL MESSAGES
 * The following function is used to send empty messages,
 * with ack, syn or rst flag.
 */
static int csp_rdp_send_cmp(csp_conn_t * conn, csp_packet_t * packet, int ack, int syn, int rst, int eak, int seq_nr, int ack_nr, int copy_yes_no) {

	/* Generate message */
	if (packet == NULL) {
		packet = csp_buffer_get(20);
		packet->length = 0;
	}

	if (packet == NULL || conn == NULL)
		return 0;

	/* Add RDP header */
	rdp_header_t * header = csp_rdp_header_add(packet);
	header->seq_nr = htons(seq_nr);
	header->ack_nr = htons(ack_nr);
	header->ack = ack;
	header->eak = eak;
	header->syn = syn;
	header->rst = rst;

	/* Send copy to tx_queue, before sending packet to IF */
	if (copy_yes_no == 1) {
		rdp_packet_t * rdp_packet = csp_buffer_get(packet->length+10);
		if (rdp_packet == NULL) return 0;
		rdp_packet->timestamp = csp_get_ms();
		memcpy(&rdp_packet->length, &packet->length, packet->length+6);
		if (csp_queue_enqueue(conn->rdp.tx_queue, &rdp_packet, 0) != CSP_QUEUE_OK)
			csp_buffer_free(rdp_packet);
	}

	/* Send packet to IF */
	if (csp_send_direct(conn->idout, packet, 0) == 0) {
		csp_debug(CSP_ERROR, "INTERFACE ERROR: not possible to send\r\n");
		csp_buffer_free(packet);
		return 0;
	}

#if CSP_DELAY_ACKS
	/* Update last ACK time stamp */
	if (ack) {
		conn->rdp.rcv_lsa = ack_nr;
		conn->rdp.ack_timestamp = csp_get_ms();
	}
#endif

	return 1;

}

/**
 * EXTENDED ACKNOWLEDGEMENTS
 * The following function sends an extended ack packet
 */
static int csp_rdp_send_eack(csp_conn_t * conn) {

	/* Allocate message */
	csp_packet_t * packet_eack = csp_buffer_get(100);
	if (packet_eack == NULL) return 0;
	packet_eack->length = 0;

	/* Loop through RX queue */
	int i;
	csp_packet_t * packet;
	unsigned int count = csp_queue_size(conn->rdp.rx_queue);
	for (i = 0; i < count; i++) {

		if (csp_queue_dequeue_isr(conn->rdp.rx_queue, &packet, &pdTrue) != CSP_QUEUE_OK) {
			csp_debug(CSP_ERROR, "Cannot dequeue from rx_queue in queue deliver\r\n");
			break;
		}

		/* Add seq nr to EACK packet */
		rdp_header_t * header = csp_rdp_header_ref(packet);
		packet_eack->data16[packet_eack->length/sizeof(uint16_t)] = htons(header->seq_nr);
		packet_eack->length += sizeof(uint16_t);
		csp_debug(CSP_PROTOCOL, "Added EACK nr %u\r\n", header->seq_nr);

		/* Requeue */
		csp_queue_enqueue_isr(conn->rdp.rx_queue, &packet, &pdTrue);

	}

	return csp_rdp_send_cmp(conn, packet_eack, 1, 0, 0, 1, conn->rdp.snd_nxt, conn->rdp.rcv_cur, 0);

}


/**
 * SYN Packet
 * The following function sends a SYN packet
 */
static int csp_rdp_send_syn(csp_conn_t * conn) {

	/* Allocate message */
	csp_packet_t * packet = csp_buffer_get(100);
	if (packet == NULL) return 0;

	/* Generate contents */
	packet->data32[0] = htonl(csp_rdp_window_size);
	packet->data32[1] = htonl(csp_rdp_conn_timeout);
	packet->data32[2] = htonl(csp_rdp_packet_timeout);
	packet->data32[3] = htonl(csp_rdp_delayed_acks);
	packet->data32[4] = htonl(csp_rdp_ack_timeout);
	packet->data32[5] = htonl(csp_rdp_ack_delay_count);
	packet->length = 24;

	return csp_rdp_send_cmp(conn, packet, 0, 1, 0, 0, conn->rdp.snd_iss, 0, 1);

}

static int inline csp_rdp_receive_data(csp_conn_t * conn, csp_packet_t * packet) {

	/* If a rx_socket is set, this message is the first in a new connection
	 * so the connection must be queued to the socket. */
	if ((conn->rx_socket != NULL) && (conn->rx_socket != (void *) 1)) {

		/* Try queueing */
		if (csp_queue_enqueue(conn->rx_socket, &conn, 0) == CSP_QUEUE_FULL) {
			csp_debug(CSP_ERROR, "ERROR socket cannot accept more connections\r\n");
			return 0;
		}

		/* Ensure that this connection will not be posted to this socket again
		 * and remember that the connection handle has been passed to userspace
		 * by setting the rx_socet = 1. */
		conn->rx_socket = (void *) 1;
	}

	/* Remove RDP header before passing to userspace */
	csp_rdp_header_remove(packet);

	/* Enqueue data */
	if (csp_queue_enqueue(conn->rx_queue, &packet, 0) != CSP_QUEUE_OK) {
		csp_debug(CSP_ERROR, "Conn buffer full\r\n");
		return 0;
	}

	return 1;

}

static void inline csp_rdp_rx_queue_flush(csp_conn_t * conn) {

	/* Loop through RX queue */
	int i;
	csp_packet_t * packet;
	unsigned int count;

	hell:
	count = csp_queue_size(conn->rdp.rx_queue);
	for (i = 0; i < count; i++) {

		if (csp_queue_dequeue_isr(conn->rdp.rx_queue, &packet, &pdTrue) != CSP_QUEUE_OK) {
			csp_debug(CSP_ERROR, "Cannot dequeue from rx_queue in queue deliver\r\n");
			break;
		}

		rdp_header_t * header = csp_rdp_header_ref(packet);
		csp_debug(CSP_PROTOCOL, "RX Queue deliver matching Element, seq %u\r\n", header->seq_nr);

		/* If the matching packet was found: */
		if (header->seq_nr == conn->rdp.rcv_cur + 1) {
			csp_debug(CSP_PROTOCOL, "Deliver seq %u\r\n", header->seq_nr);
			csp_rdp_receive_data(conn, packet);
			conn->rdp.rcv_cur += 1;
			goto hell;
			break;

		/* Otherwise, requeue */
		} else {
			csp_queue_enqueue_isr(conn->rdp.rx_queue, &packet, &pdTrue);
		}

	}

}

static int inline csp_rdp_rx_queue_exists(csp_conn_t * conn, uint16_t seq_nr) {

	/* Loop through RX queue */
	int i;
	rdp_packet_t * packet;
	unsigned int count = csp_queue_size(conn->rdp.rx_queue);
	for (i = 0; i < count; i++) {

		if (csp_queue_dequeue_isr(conn->rdp.rx_queue, &packet, &pdTrue) != CSP_QUEUE_OK) {
			csp_debug(CSP_ERROR, "Cannot dequeue from rx_queue in queue exists\r\n");
			break;
		}

		csp_queue_enqueue_isr(conn->rdp.rx_queue, &packet, &pdTrue);

		rdp_header_t * header = csp_rdp_header_ref((csp_packet_t *) packet);
		csp_debug(CSP_PROTOCOL, "RX Queue exists matching Element, seq %u\r\n", header->seq_nr);

		/* If the matching packet was found, deliver */
		if (header->seq_nr == seq_nr) {
			csp_debug(CSP_PROTOCOL, "We have a match\r\n");
			return 1;
		}

	}

	return 0;

}

static int inline csp_rdp_rx_queue_add(csp_conn_t * conn, csp_packet_t * packet, uint16_t seq_nr) {
	if (csp_rdp_rx_queue_exists(conn, seq_nr))
		return 0;
	return csp_queue_enqueue_isr(conn->rdp.rx_queue, &packet, &pdTrue);
}

static void csp_rdp_flush_eack(csp_conn_t * conn, csp_packet_t * eack_packet) {

	if ((conn == NULL) || eack_packet == NULL) {
		csp_debug(CSP_ERROR, "Null pointer passed to rdp flush eack\r\n");
		return;
	}

	/* Loop through TX queue */
	int i, j;
	rdp_packet_t * packet;
	unsigned int count = csp_queue_size(conn->rdp.tx_queue);
	for (i = 0; i < count; i++) {

		if (csp_queue_dequeue(conn->rdp.tx_queue, &packet, 0) != CSP_QUEUE_OK) {
			csp_debug(CSP_ERROR, "Cannot dequeue from tx_queue in flush\r\n");
			break;
		}

		rdp_header_t * header = csp_rdp_header_ref((csp_packet_t *) packet);
		csp_debug(CSP_PROTOCOL, "EACK Matching Element, time %u, seq %u\r\n", packet->timestamp, ntohs(header->seq_nr));

		/* Look for this element in eacks */
		int match = 0;
		for (j = 0; j < (eack_packet->length - sizeof(rdp_header_t)) / 2; j++) {
			if (ntohs(eack_packet->data16[j]) == ntohs(header->seq_nr))
				match = 1;
			if (ntohs(eack_packet->data16[j]) > ntohs(header->seq_nr))
				packet->timestamp = csp_get_ms() - conn->rdp.packet_timeout;
		}

		if (match == 0) {
			/* If not found, put back on tx queue */
			csp_queue_enqueue(conn->rdp.tx_queue, &packet, 0);
		} else {
			csp_debug(CSP_PROTOCOL, "TX Element %u freed\r\n", ntohs(header->seq_nr));

			/* Found, free */
			csp_buffer_free(packet);
		}

	}

}

void csp_rdp_flush_all(csp_conn_t * conn) {

	if ((conn == NULL) || conn->rdp.tx_queue == NULL) {
		csp_debug(CSP_ERROR, "Null pointer passed to rdp flush all\r\n");
		return;
	}

	rdp_packet_t * packet;

	/* Loop through TX queue */
	int i;
	unsigned int count = csp_queue_size(conn->rdp.tx_queue);
	for (i = 0; i < count; i++) {

		if (csp_queue_dequeue_isr(conn->rdp.tx_queue, &packet, &pdTrue) != CSP_QUEUE_OK) {
			csp_debug(CSP_ERROR, "Cannot dequeue from tx_queue in flush all\r\n");
			break;
		}

		csp_debug(CSP_PROTOCOL, "Clear TX Element, time %u, seq %u\r\n", packet->timestamp, ntohs(csp_rdp_header_ref((csp_packet_t *) packet)->seq_nr));
		csp_buffer_free(packet);
		continue;
	}

	/* Loop through RX queue */
	count = csp_queue_size(conn->rdp.rx_queue);
	for (i = 0; i < count; i++) {

		if (csp_queue_dequeue_isr(conn->rdp.rx_queue, &packet, &pdTrue) != CSP_QUEUE_OK) {
			csp_debug(CSP_ERROR, "Cannot dequeue from rx_queue in flush all\r\n");
			break;
		}

		csp_debug(CSP_PROTOCOL, "Clear RX Element, time %u, seq %u\r\n", packet->timestamp, ntohs(csp_rdp_header_ref((csp_packet_t *) packet)->seq_nr));
		csp_buffer_free(packet);
		continue;
	}

}

/**
 * This function must be called with regular intervals for the
 * RDP protocol to work as expected. This takes care of closing
 * stale connections and retransmitting traffic. A good place to
 * call this function is from the CSP router task.
 * NOTE: the queue calls in this function has been optimized for speed
 * that means using the _isr functions even though it is called only
 * from task context. However the RDP lock ensures that everything
 * is safe.
 */
void csp_rdp_check_timeouts(csp_conn_t * conn) {

	rdp_packet_t * packet;

	/* Check for the dreaded null pointer */
	if (conn == NULL) {
		csp_debug(CSP_ERROR, "Null pointer passed to check timeouts\r\n");
		return;
	}

	/**
	 * CONNECTION TIMEOUT:
	 * Check that connection has not timed out inside the network stack
	 * */
	uint32_t time_now = csp_get_ms();
	if ((conn->rx_socket != NULL) && (conn->rx_socket != (void *) 1)) {
		if (conn->open_timestamp + conn->rdp.conn_timeout < time_now) {
			csp_debug(CSP_WARN, "Found a lost connection, closing now\r\n");
			csp_close(conn);
			return;
		}
	}

	/**
	 * CLOSE-WAIT TIMEOUT:
	 * After waiting a while in CLOSE-WAIT, the connection should be closed.
	 * */
	if (conn->rdp.state == RDP_CLOSE_WAIT) {
		if (conn->open_timestamp + conn->rdp.conn_timeout < time_now) {
			csp_debug(CSP_PROTOCOL, "CLOSE_WAIT timeout\r\n");
			csp_close(conn);
			return;
		}
	}

	/* Wait for RDP to be ready */
	if (!csp_rdp_wait(1000, conn))
		return;

	/**
	 * MESSAGE TIMEOUT:
	 * Check each outgoing message for TX timeout
	 */

	/* Loop through TX queue */
	int i;
	unsigned int count = csp_queue_size(conn->rdp.tx_queue);
	for (i = 0; i < count; i++) {

		if ((csp_queue_dequeue_isr(conn->rdp.tx_queue, &packet, &pdTrue) != CSP_QUEUE_OK) || packet == NULL) {
			csp_debug(CSP_ERROR, "Cannot dequeue from tx_queue in flush\r\n");
			break;
		}

		/* Get header */
		rdp_header_t * header = csp_rdp_header_ref((csp_packet_t *) packet);

		/* If acked, do not retransmit */
		if (ntohs(header->seq_nr) < conn->rdp.snd_una) {
			csp_debug(CSP_PROTOCOL, "TX Element Free, time %u, seq %u\r\n", packet->timestamp, ntohs(header->seq_nr));
			csp_buffer_free(packet);
			continue;
		}

		/* Check timestamp and retransmit if needed */
		if (packet->timestamp + conn->rdp.packet_timeout < time_now) {
			csp_debug(CSP_WARN, "TX Element timed out, retransmitting seq %u\r\n", ntohs(header->seq_nr));

			/* Update to latest outgoing ACK */
			header->ack_nr = htons(conn->rdp.rcv_cur);

			/* Send copy to tx_queue */
			packet->timestamp = csp_get_ms();
			csp_packet_t * new_packet = csp_buffer_get(packet->length+10);
			memcpy(&new_packet->length, &packet->length, packet->length+6);
			if (csp_send_direct(conn->idout, new_packet, 0) == 0) {
				csp_debug(CSP_WARN, "Retransmission failed\r\n");
				csp_buffer_free(new_packet);
			}

		}

		/* Requeue the TX element */
		csp_queue_enqueue_isr(conn->rdp.tx_queue, &packet, &pdTrue);

	}

#if CSP_DELAY_ACKS
	/**
	 * ACK TIMEOUT:
	 * Check ACK timeouts
	 */

	/* Only send timeout ACK if segment was not acknowledged by regular ACK */
	if (conn->rdp.rcv_lsa < conn->rdp.rcv_cur) {
		uint32_t now = csp_get_ms();
		uint32_t diff = now - conn->rdp.ack_timestamp;
		if (diff > conn->rdp.ack_timeout)
			csp_rdp_send_cmp(conn, NULL, 1, 0, 0, 0, conn->rdp.snd_nxt, conn->rdp.rcv_cur, 0);
	}
#endif

	/* Wake user task if TX queue is ready for more data */
	if (conn->rdp.state == RDP_OPEN)
		if (csp_queue_size(conn->rdp.tx_queue) < conn->rdp.window_size - 1)
			if (conn->rdp.snd_nxt < conn->rdp.snd_una + conn->rdp.window_size * 2)
				csp_bin_sem_post(&conn->rdp.tx_wait);

	csp_rdp_release();

}

void csp_rdp_new_packet(csp_conn_t * conn, csp_packet_t * packet) {

	/* Wait for RDP to be ready */
	if (!csp_rdp_wait(1000, conn)) {
		csp_buffer_free(packet);
		return;
	}

	/* Get RX header and convert to host byte-order */
	rdp_header_t * rx_header = csp_rdp_header_ref(packet);
	rx_header->ack_nr = ntohs(rx_header->ack_nr);
	rx_header->seq_nr = ntohs(rx_header->seq_nr);

	csp_debug(CSP_PROTOCOL, "RDP: S %u: HEADER NP: syn %u, ack %u, eack %u, rst %u, seq_nr %u, ack_nr %u, packet_len %u (%u)\r\n", conn->rdp.state, rx_header->syn, rx_header->ack, rx_header->eak, rx_header->rst, rx_header->seq_nr, rx_header->ack_nr, packet->length, packet->length - sizeof(rdp_header_t));

	/* If the connection is closed, this is the first message in a new connection,
	 * Run the connect passive sequence here.
	 */
	if (conn->rdp.state == RDP_CLOSED) {
		conn->rdp.snd_iss = 2000;
		conn->rdp.snd_nxt = conn->rdp.snd_iss + 1;
		conn->rdp.snd_una = conn->rdp.snd_iss;
		conn->rdp.state = RDP_LISTEN;
	}

	/* If a RESET was received. */
	if (rx_header->rst) {

		if (rx_header->ack) {
			/* Store current ack'ed sequence number */
			conn->rdp.snd_una = rx_header->ack_nr + 1;
		}

		if (conn->rdp.state == RDP_CLOSE_WAIT) {
			csp_debug(CSP_PROTOCOL, "RST received in CLOSE_WAIT. Now closing connection\r\n");
			csp_buffer_free(packet);
			csp_rdp_release();
			csp_close(conn);
			return;
		} else {
			csp_debug(CSP_PROTOCOL, "Got RESET in state %u\r\n", conn->rdp.state);

			if (rx_header->seq_nr == conn->rdp.rcv_cur + 1) {
				csp_debug(CSP_PROTOCOL, "RESET in sequence, no more data incoming, reply with RESET\r\n");
				csp_rdp_send_cmp(conn, NULL, 1, 0, 1, 0, conn->rdp.snd_nxt, conn->rdp.rcv_cur, 0);
				conn->rdp.state = RDP_CLOSE_WAIT;
				goto discard_close;
			} else {
				csp_debug(CSP_PROTOCOL, "RESET out of sequence, keep connection open\r\n");
				goto discard_open;
			}
		}
	}

	/* The BIG FAT switch (state-machine) */
	switch(conn->rdp.state) {

	/**
	 * STATE == LISTEN
	 */
	case RDP_LISTEN: {

		/* ACK received while in listen, this is not normal. Inform by sending back RST */
		if (rx_header->ack) {
			csp_debug(CSP_ERROR, "ACK received in LISTEN state\r\n");
			csp_rdp_send_cmp(conn, NULL, 0, 0, 1, 0, conn->rdp.snd_nxt, conn->rdp.rcv_cur, 0);
			goto discard_close;
		}

		/* SYN received, this was expected */
		if (rx_header->syn) {
			csp_debug(CSP_PROTOCOL, "RDP: SYN-Received\r\n");
			conn->rdp.rcv_cur = rx_header->seq_nr;
			conn->rdp.rcv_irs = rx_header->seq_nr;
#if CSP_DELAY_ACKS
			conn->rdp.rcv_lsa = rx_header->seq_nr;
#endif
			conn->rdp.state = RDP_SYN_RCVD;

			conn->rdp.window_size = ntohl(packet->data32[0]);
			conn->rdp.conn_timeout = ntohl(packet->data32[1]);
			conn->rdp.packet_timeout = ntohl(packet->data32[2]);
			conn->rdp.delayed_acks = ntohl(packet->data32[3]);
			conn->rdp.ack_timeout = ntohl(packet->data32[4]);
			conn->rdp.ack_delay_count = ntohl(packet->data32[5]);

			csp_debug(CSP_PROTOCOL, "RDP: Window Size %u, conn timeout %u, packet timeout %u\r\n", conn->rdp.window_size, conn->rdp.conn_timeout, conn->rdp.packet_timeout);
			csp_debug(CSP_PROTOCOL, "RDP: Delayed acks: %u, ack timeout %u, ack each %u packet\r\n", conn->rdp.delayed_acks, conn->rdp.ack_timeout, conn->rdp.ack_delay_count);

			/* Send SYN/ACK */
			csp_rdp_send_cmp(conn, NULL, 1, 1, 0, 0, conn->rdp.snd_iss, conn->rdp.rcv_irs, 1);

			goto discard_open;
		}

		csp_debug(CSP_PROTOCOL, "RDP: ERROR should never reach here state: LISTEN\r\n");
		goto discard_close;

	}
	break;

	/**
	 * STATE == SYN-SENT
	 */
	case RDP_SYN_SENT: {

		/* First check SYN/ACK */
		if (rx_header->syn && rx_header->ack) {

			conn->rdp.rcv_cur = rx_header->seq_nr;
			conn->rdp.rcv_irs = rx_header->seq_nr;
			conn->rdp.snd_una = rx_header->ack_nr + 1;
			conn->rdp.state = RDP_OPEN;

			csp_debug(CSP_PROTOCOL, "RDP: NP: Connection OPEN\r\n");

			/* Send ACK */
#if CSP_DELAY_ACKS
			conn->rdp.rcv_lsa = rx_header->seq_nr - 1;
#else
			csp_rdp_send_cmp(conn, NULL, 1, 0, 0, 0, conn->rdp.snd_nxt, conn->rdp.rcv_cur, 0);
#endif

			/* Wake TX task */
			csp_bin_sem_post(&conn->rdp.tx_wait);

			goto discard_open;
		}

		/* If there was no SYN in the reply, our SYN message hit an already open connection
		 * This is handled by sending a RST.
		 * Normally this would be followed up by a new connection attempt, however
		 * we don't have a method for signalling this to the userspace.
		 */
		if (rx_header->ack) {
			csp_debug(CSP_ERROR, "Half-open connection found, sending RST\r\n");
			csp_rdp_send_cmp(conn, NULL, 0, 0, 1, 0, conn->rdp.snd_nxt, conn->rdp.rcv_cur, 0);
			csp_bin_sem_post(&conn->rdp.tx_wait);

			goto discard_open;
		}

		/* Otherwise we have an invalid command, such as a SYN reply to a SYN command,
		 * indicating simultaneous connections, which is not possible in the way CSP
		 * reserves some ports for server and some for clients.
		 */
		csp_debug(CSP_ERROR, "Invalid reply to SYN request\r\n");
		goto discard_close;

	}
	break;

	/**
	 * STATE == OPEN
	 */
	case RDP_SYN_RCVD:
	case RDP_OPEN:
	{

		/* SYN or !ACK is invalid */
		if ((rx_header->syn == 1) || (rx_header->ack == 0)) {
			csp_debug(CSP_ERROR, "Invalid SYN or no ACK, resetting!\r\n");
			goto discard_close;
		}

		/* Check sequence number */
		if ((rx_header->seq_nr <= conn->rdp.rcv_cur) || (rx_header->seq_nr > conn->rdp.rcv_cur + conn->rdp.window_size * 2)) {
			csp_debug(CSP_WARN, "Sequence number unacceptable\r\n");
			/* If duplicate SYN received, send another SYN/ACK */
			if (conn->rdp.state == RDP_SYN_RCVD)
				csp_rdp_send_cmp(conn, NULL, 1, 1, 0, 0, conn->rdp.snd_iss, conn->rdp.rcv_irs, 1);
			/* If duplicate data packet received, send EACK back */
			if (conn->rdp.state == RDP_OPEN)
				csp_rdp_send_eack(conn);
			goto discard_open;
		}

		/* We have an ACK: Check HIGH boundary: */
		if (rx_header->ack_nr >= conn->rdp.snd_nxt) {
			csp_debug(CSP_ERROR, "ACK number too high! %u >= %u\r\n", rx_header->ack_nr, conn->rdp.snd_nxt);
			goto discard_close;
		}

		/* We have an ACK: Check LOW boundary: */
		if (rx_header->ack_nr < conn->rdp.snd_una - 1 - (conn->rdp.window_size * 2)) {
			csp_debug(CSP_ERROR, "ACK number too low! %u < %u\r\n", rx_header->ack_nr, (conn->rdp.snd_una - 1 - (conn->rdp.window_size * 2)));
			goto discard_close;
		}

		/* We have an ACK: Check SYN_RCVD ACK */
		if (conn->rdp.state == RDP_SYN_RCVD) {
			if (rx_header->ack_nr != conn->rdp.snd_iss) {
				csp_debug(CSP_ERROR, "SYN-RCVD: Wrong ACK number\r\n");
				goto discard_close;
			}
			csp_debug(CSP_PROTOCOL, "RDP: NC: Connection OPEN\r\n");
			conn->rdp.state = RDP_OPEN;
		}

		/* Store current ack'ed sequence number */
		conn->rdp.snd_una = rx_header->ack_nr + 1;

		/* We have an EACK */
		if (rx_header->eak == 1) {
			if (packet->length > sizeof(rdp_header_t))
				csp_rdp_flush_eack(conn, packet);
			goto discard_open;
		}

		/* If no data, return here */
		if (packet->length <= sizeof(rdp_header_t))
			goto discard_open;

		/* If message is not in sequence, send EACK and store packet */
		if (conn->rdp.rcv_cur + 1 != rx_header->seq_nr) {
			if (csp_rdp_rx_queue_add(conn, packet, rx_header->seq_nr) != CSP_QUEUE_OK) {
				csp_debug(CSP_PROTOCOL, "Duplicate sequence number\r\n");
				goto discard_open;
			}
			csp_rdp_send_eack(conn);
			goto accepted_open;
		}

		/* Store sequence number before stripping RDP header */
		uint16_t seq_nr = rx_header->seq_nr;

		/* Receive data */
		if (!csp_rdp_receive_data(conn, packet)) {
			csp_debug(CSP_ERROR, "Cannot receive data, rejecting packet\r\n");
			goto discard_open;
		}

		/* Update last received packet */
		conn->rdp.rcv_cur = seq_nr;

		/* The message is in sequence and contains data */
#if CSP_DELAY_ACKS
		/* We only ACK this if receiver window is half full */
		if (conn->rdp.rcv_cur > conn->rdp.rcv_lsa + conn->rdp.ack_delay_count)
			csp_rdp_send_cmp(conn, NULL, 1, 0, 0, 0, conn->rdp.snd_nxt, conn->rdp.rcv_cur, 0);
#else
		/* ACK the message */
		csp_rdp_send_cmp(conn, NULL, 1, 0, 0, 0, conn->rdp.snd_nxt, conn->rdp.rcv_cur, 0);
#endif

		/* Flush RX queue */
		csp_rdp_rx_queue_flush(conn);

		goto accepted_open;

	}
	break;

	case RDP_CLOSE_WAIT:

		/* We have an ACK: Check HIGH boundary: */
		if (rx_header->ack_nr >= conn->rdp.snd_nxt) {
			csp_debug(CSP_ERROR, "ACK number too high! %u >= %u\r\n", rx_header->ack_nr, conn->rdp.snd_nxt);
			goto discard_open;
		}

		/* We have an ACK: Check LOW boundary: */
		if (rx_header->ack_nr < conn->rdp.snd_una - 1 - (conn->rdp.window_size * 2)) {
			csp_debug(CSP_ERROR, "ACK number too low! %u < %u\r\n", rx_header->ack_nr, (conn->rdp.snd_una - 1 - (conn->rdp.window_size * 2)));
			goto discard_open;
		}

		/* Store current ack'ed sequence number */
		conn->rdp.snd_una = rx_header->ack_nr + 1;

		/* Send back a reset */
		csp_rdp_send_cmp(conn, NULL, 1, 0, 1, 0, conn->rdp.snd_nxt, conn->rdp.rcv_cur, 0);

		goto discard_open;

	default:
		csp_debug(CSP_ERROR, "RDP: ERROR default state!\r\n");
		goto discard_close;
	}

discard_close:

	/* If user-space has received the conn handle, wake it up,
	 * by sending a NULL pointer, userspace should close connection */
	if (conn->rx_socket == (void *) 1) {
		csp_debug(CSP_PROTOCOL, "Waiting for userspace to close\r\n");
	    void * null_pointer = NULL;
	    csp_queue_enqueue(conn->rx_queue, &null_pointer, 0);
	}

discard_open:
	csp_buffer_free(packet);
accepted_open:
	csp_rdp_release();
	return;

}

int csp_rdp_connect_active(csp_conn_t * conn, unsigned int timeout) {

	int retry = 1;

	/* Wait for RDP to be ready */
	if (!csp_rdp_wait(1000, conn))
		return 0;

	conn->rdp.window_size = csp_rdp_window_size;
	conn->rdp.conn_timeout = csp_rdp_conn_timeout;
	conn->rdp.packet_timeout = csp_rdp_packet_timeout;
	conn->rdp.delayed_acks = csp_rdp_delayed_acks;
	conn->rdp.ack_timeout = csp_rdp_ack_timeout;
	conn->rdp.ack_delay_count = csp_rdp_ack_delay_count;
	conn->rdp.ack_timestamp = csp_get_ms();

	retry:

	csp_debug(CSP_PROTOCOL, "RDP: Active connect, conn state %u\r\n", conn->rdp.state);

	if (conn->rdp.state == RDP_OPEN) {
		csp_debug(CSP_ERROR, "RDP: Connection already open\r\n");
		csp_rdp_release();
		return 0;
	}

	conn->rdp.snd_iss = 1000;
	conn->rdp.snd_nxt = conn->rdp.snd_iss + 1;
	conn->rdp.snd_una = conn->rdp.snd_iss;

	csp_debug(CSP_PROTOCOL, "RDP: AC: Sending SYN\r\n");

	/* Send SYN message */
	conn->rdp.state = RDP_SYN_SENT;
	if (csp_rdp_send_syn(conn) == 0)
		goto error;

	/* Wait for reply */
	csp_debug(CSP_PROTOCOL, "RDP: AC: Waiting for SYN/ACK reply...\r\n");
	csp_rdp_release();
	csp_bin_sem_wait(&conn->rdp.tx_wait, 0);
	int result = csp_bin_sem_wait(&conn->rdp.tx_wait, conn->rdp.conn_timeout);

	if (!csp_rdp_wait(1000, conn))
		return 0;

	if (result == CSP_SEMAPHORE_OK) {
		if (conn->rdp.state == RDP_OPEN) {
			csp_debug(CSP_PROTOCOL, "RDP: AC: Connection OPEN\r\n");
			csp_rdp_release();
			return 1;
		} else if(conn->rdp.state == RDP_SYN_SENT) {
			if (retry) {
				csp_debug(CSP_WARN, "RDP: Half-open connection detected, RST sent, now retrying\r\n");
				csp_rdp_flush_all(conn);
				retry -= 1;
				goto retry;
			} else {
				csp_debug(CSP_ERROR, "RDP: Connection stayed half-open, even after RST and retry!\r\n");
				goto error;
			}
		}
	} else {
		csp_debug(CSP_PROTOCOL, "RDP: AC: Connection Failed\r\n");
		goto error;
	}

error:
	conn->rdp.state = RDP_CLOSE_WAIT;
	csp_rdp_release();
	return 0;

}

int csp_rdp_send(csp_conn_t * conn, csp_packet_t * packet, unsigned int timeout) {

	/* Wait for RDP to be ready */
	if (!csp_rdp_wait(1000, conn))
		return 0;

	if (conn->rdp.state != RDP_OPEN) {
		csp_debug(CSP_ERROR, "RDP: ERROR cannot send, connection reset by peer!\r\n");
		csp_rdp_release();
		return 0;
	}

	csp_debug(CSP_PROTOCOL, "RDP: SEND SEQ %u\r\n", conn->rdp.snd_nxt);

	/* If TX window is full, wait here */
	if (conn->rdp.snd_nxt - conn->rdp.snd_una + 1 >= conn->rdp.window_size) {
		/* Release, and wait for stack to complete TX */
		csp_rdp_release();
		csp_bin_sem_wait(&conn->rdp.tx_wait, 0);
		if ((csp_bin_sem_wait(&conn->rdp.tx_wait, timeout)) != CSP_SEMAPHORE_OK) {
			csp_debug(CSP_ERROR, "Timeout during send\r\n");
			return 0;
		}
		/* Lock stack again */
		if (!csp_rdp_wait(1000, conn))
			return 0;
	}

	/* Add RDP header */
	rdp_header_t * tx_header = csp_rdp_header_add(packet);
	tx_header->ack_nr = htons(conn->rdp.rcv_cur);
	tx_header->seq_nr = htons(conn->rdp.snd_nxt);
	tx_header->ack = 1;
	conn->rdp.snd_nxt += 1;

	/* Send copy to tx_queue */
	rdp_packet_t * rdp_packet = csp_buffer_get(packet->length+10);
	if (rdp_packet == NULL) {
		csp_debug(CSP_ERROR, "Failed to allocate packet buffer\r\n");
		return 0;
	}

	rdp_packet->timestamp = csp_get_ms();
	memcpy(&rdp_packet->length, &packet->length, packet->length+6);
	if (csp_queue_enqueue(conn->rdp.tx_queue, &rdp_packet, 0) != CSP_QUEUE_OK) {
		csp_debug(CSP_ERROR, "No more space in RDP retransmit queue\r\n");
		csp_buffer_free(rdp_packet);
		csp_rdp_release();
		return 0;
	}

	csp_rdp_release();
	return 1;

}

int csp_rdp_allocate(csp_conn_t * conn) {

	csp_debug(CSP_BUFFER, "RDP: Creating RDP queues for conn %p\r\n", conn);

	/* Set initial state */
	conn->rdp.state = RDP_CLOSED;

	/* Create a binary semaphore to wait on for tasks */
	if (csp_bin_sem_create(&conn->rdp.tx_wait) != CSP_SEMAPHORE_OK) {
		csp_debug(CSP_ERROR, "Failed to initialize semaphore\r\n");
		return 0;
	}

	/* Create TX queue */
	conn->rdp.tx_queue = csp_queue_create(CSP_RDP_MAX_WINDOW, sizeof(csp_packet_t *));
	if (conn->rdp.tx_queue == NULL) {
		csp_debug(CSP_ERROR, "Failed to create TX queue for conn\r\n");
		csp_bin_sem_remove(&conn->rdp.tx_wait);
		return 0;
	}

	/* Create RX queue */
	conn->rdp.rx_queue = csp_queue_create(CSP_RDP_MAX_WINDOW * 2, sizeof(csp_packet_t *));
	if (conn->rdp.rx_queue == NULL) {
		csp_debug(CSP_ERROR, "Failed to create RX queue for conn\r\n");
		csp_bin_sem_remove(&conn->rdp.tx_wait);
		csp_queue_remove(conn->rdp.tx_queue);
		return 0;
	}

	return 1;

}

int csp_rdp_close(csp_conn_t * conn) {

	/* Wait for RDP to be ready */
	if (!csp_rdp_wait(1000, conn))
		return 0;

	/* If message is open, send reset */
	if (conn->rdp.state != RDP_CLOSE_WAIT) {
		csp_debug(CSP_PROTOCOL, "RDP Close, sending RST on conn %p\r\n", conn);
		csp_rdp_send_cmp(conn, NULL, 1, 0, 1, 0, conn->rdp.snd_nxt, conn->rdp.rcv_cur, 0);
		conn->rdp.state = RDP_CLOSE_WAIT;
		csp_rdp_release();
		return 1;
	}

	csp_debug(CSP_PROTOCOL, "RDP Close in CLOSE_WAIT, now closing\r\n");
	conn->rdp.state = RDP_CLOSED;
	csp_rdp_release();
	return 0;

}

/**
 * RDP Set socket options
 * Controls important parameters of the RDP protocol.
 * These settings will be applied to all new outgoing connections.
 * The settings are global, so be sure no other task are conflicting with your settings.
 */
void csp_rdp_set_opt(unsigned int window_size, unsigned int conn_timeout_ms, unsigned int packet_timeout_ms, unsigned int delayed_acks, unsigned int ack_timeout, unsigned int ack_delay_count) {
	csp_rdp_window_size = window_size;
	csp_rdp_conn_timeout = conn_timeout_ms;
	csp_rdp_packet_timeout = packet_timeout_ms;
	csp_rdp_delayed_acks = delayed_acks;
	csp_rdp_ack_timeout = ack_timeout;
	csp_rdp_ack_delay_count = ack_delay_count;
}

#ifdef CSP_DEBUG
void csp_rdp_conn_print(csp_conn_t * conn) {

	if (conn == NULL)
		return;

	printf("\tRDP: State %u, rcv %u, snd %u, win %u\r\n", conn->rdp.state, conn->rdp.rcv_cur, conn->rdp.snd_una, conn->rdp.window_size);

}
#endif

#endif
