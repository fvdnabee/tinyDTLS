#ifndef UDP_CONNECTION_ADDR
#define UDP_CONNECTION_ADDR       fe80:0:0:0:0:0:0:2
#endif /* !UDP_CONNECTION_ADDR */
/*
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the Institute nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * This file is part of the Contiki operating system.
 *
 */

#include "contiki.h"
#include "contiki-lib.h"
#include "contiki-net.h"

#include "dev/serial-line.h"

#include <string.h>

#include "config.h"

#ifndef DEBUG
#define DEBUG DEBUG_PRINT
#endif
#include "net/ip/uip-debug.h"

#include "debug.h"
#include "dtls.h"

#define UIP_IP_BUF   ((struct uip_ip_hdr *)&uip_buf[UIP_LLH_LEN])
#define UIP_UDP_BUF  ((struct uip_udp_hdr *)&uip_buf[UIP_LLIPH_LEN])

#define MAX_PAYLOAD_LEN 120

static struct uip_udp_conn *client_conn;
static dtls_context_t *dtls_context;
static char buf[200];
static size_t buflen = 0;

static const unsigned char ecdsa_priv_key[] = {
			0x41, 0xC1, 0xCB, 0x6B, 0x51, 0x24, 0x7A, 0x14,
			0x43, 0x21, 0x43, 0x5B, 0x7A, 0x80, 0xE7, 0x14,
			0x89, 0x6A, 0x33, 0xBB, 0xAD, 0x72, 0x94, 0xCA,
			0x40, 0x14, 0x55, 0xA1, 0x94, 0xA9, 0x49, 0xFA};

static const unsigned char ecdsa_pub_key_x[] = {
			0x36, 0xDF, 0xE2, 0xC6, 0xF9, 0xF2, 0xED, 0x29,
			0xDA, 0x0A, 0x9A, 0x8F, 0x62, 0x68, 0x4E, 0x91,
			0x63, 0x75, 0xBA, 0x10, 0x30, 0x0C, 0x28, 0xC5,
			0xE4, 0x7C, 0xFB, 0xF2, 0x5F, 0xA5, 0x8F, 0x52};

static const unsigned char ecdsa_pub_key_y[] = {
			0x71, 0xA0, 0xD4, 0xFC, 0xDE, 0x1A, 0xB8, 0x78,
			0x5A, 0x3C, 0x78, 0x69, 0x35, 0xA7, 0xCF, 0xAB,
			0xE9, 0x3F, 0x98, 0x72, 0x09, 0xDA, 0xED, 0x0B,
			0x4F, 0xAB, 0xC3, 0x6F, 0xC7, 0x72, 0xF8, 0x29};

static void
try_send(struct dtls_context_t *ctx, session_t *dst) {
  int res;
  res = dtls_write(ctx, dst, (uint8 *)buf, buflen);
  if (res >= 0) {
    memmove(buf, buf + res, buflen - res);
    buflen -= res;
  }
}

static int
read_from_peer(struct dtls_context_t *ctx, 
	       session_t *session, uint8 *data, size_t len) {
  size_t i;
  for (i = 0; i < len; i++)
    PRINTF("%c", data[i]);
  return 0;
}

static int
send_to_peer(struct dtls_context_t *ctx, 
	     session_t *session, uint8 *data, size_t len) {

  struct uip_udp_conn *conn = (struct uip_udp_conn *)dtls_get_app_data(ctx);

  uip_ipaddr_copy(&conn->ripaddr, &session->addr);
  conn->rport = session->port;

  PRINTF("send to ");
  PRINT6ADDR(&conn->ripaddr);
  PRINTF(":%u\n", uip_ntohs(conn->rport));

  uip_udp_packet_send(conn, data, len);

  /* Restore server connection to allow data from any node */
  /* FIXME: do we want this at all? */
  memset(&conn->ripaddr, 0, sizeof(conn->ripaddr));
  memset(&conn->rport, 0, sizeof(conn->rport));

  return len;
}

#ifdef DTLS_PSK
static int
get_psk_key(struct dtls_context_t *ctx, 
	    const session_t *session, 
	    const unsigned char *id, size_t id_len, 
	    const dtls_psk_key_t **result) {

  static const dtls_psk_key_t psk = {
    .id = (unsigned char *)"Client_identity", 
    .id_length = 15,
    .key = (unsigned char *)"secretPSK", 
    .key_length = 9
  };
   
  *result = &psk;
  return 0;
}
#endif /* DTLS_PSK */

#ifdef DTLS_ECC
static int
get_ecdsa_key(struct dtls_context_t *ctx,
	      const session_t *session,
	      const dtls_ecdsa_key_t **result) {
  static const dtls_ecdsa_key_t ecdsa_key = {
    .curve = DTLS_ECDH_CURVE_SECP256R1,
    .priv_key = ecdsa_priv_key,
    .pub_key_x = ecdsa_pub_key_x,
    .pub_key_y = ecdsa_pub_key_y
  };

  *result = &ecdsa_key;
  return 0;
}

static int
verify_ecdsa_key(struct dtls_context_t *ctx,
		 const session_t *session,
		 const unsigned char *other_pub_x,
		 const unsigned char *other_pub_y,
		 size_t key_size) {
  return 0;
}
#endif /* DTLS_ECC */

PROCESS(udp_server_process, "UDP server process");
AUTOSTART_PROCESSES(&udp_server_process);
/*---------------------------------------------------------------------------*/
static void
dtls_handle_read(dtls_context_t *ctx) {
  static session_t session;

  if(uip_newdata()) {
    uip_ipaddr_copy(&session.addr, &UIP_IP_BUF->srcipaddr);
    session.port = UIP_UDP_BUF->srcport;
    session.size = sizeof(session.addr) + sizeof(session.port);

    ((char *)uip_appdata)[uip_datalen()] = 0;
    PRINTF("Client received message from ");
    PRINT6ADDR(&session.addr);
    PRINTF(":%d\n", uip_ntohs(session.port));

    dtls_handle_message(ctx, &session, uip_appdata, uip_datalen());
  }
}
/*---------------------------------------------------------------------------*/
static void
print_local_addresses(void)
{
  int i;
  uint8_t state;

  PRINTF("Client IPv6 addresses: ");
  for(i = 0; i < UIP_DS6_ADDR_NB; i++) {
    state = uip_ds6_if.addr_list[i].state;
    if(uip_ds6_if.addr_list[i].isused &&
       (state == ADDR_TENTATIVE || state == ADDR_PREFERRED)) {
      PRINT6ADDR(&uip_ds6_if.addr_list[i].ipaddr);
      PRINTF("\n");
    }
  }
}

static void
set_connection_address(uip_ipaddr_t *ipaddr)
{
#define _QUOTEME(x) #x
#define QUOTEME(x) _QUOTEME(x)
#ifdef UDP_CONNECTION_ADDR
  if(uiplib_ipaddrconv(QUOTEME(UDP_CONNECTION_ADDR), ipaddr) == 0) {
    PRINTF("UDP client failed to parse address '%s'\n", QUOTEME(UDP_CONNECTION_ADDR));
  }
#elif UIP_CONF_ROUTER
  uip_ip6addr(ipaddr,0xaaaa,0,0,0,0x0200,0x0000,0x0000,0x0001);
#else
  //uip_ip6addr(ipaddr,0xfe80,0,0,0,0x6466,0x6666,0x6666,0x6666);
  uip_ip6addr(ipaddr,0xfe80,0,0,0,0,0,0,0x0002);
#endif /* UDP_CONNECTION_ADDR */
}

void
init_dtls(session_t *dst) {
  static dtls_handler_t cb = {
    .write = send_to_peer,
    .read  = read_from_peer,
    .event = NULL,
#ifdef DTLS_PSK
    .get_psk_key = get_psk_key,
#endif /* DTLS_PSK */
#ifdef DTLS_ECC
    .get_ecdsa_key = get_ecdsa_key,
    .verify_ecdsa_key = verify_ecdsa_key
#endif /* DTLS_ECC */
  };
  PRINTF("DTLS client started\n");

  print_local_addresses();

  dst->size = sizeof(dst->addr) + sizeof(dst->port);
  dst->port = UIP_HTONS(20220);

  set_connection_address(&dst->addr);
  client_conn = udp_new(&dst->addr, 0, NULL);
  udp_bind(client_conn, dst->port);

  PRINTF("set connection address to ");
  PRINT6ADDR(&dst->addr);
  PRINTF(":%d\n", uip_ntohs(dst->port));

  dtls_set_log_level(DTLS_LOG_DEBUG);

  dtls_context = dtls_new_context(client_conn);
  if (dtls_context)
    dtls_set_handler(dtls_context, &cb);
}

/*---------------------------------------------------------------------------*/
PROCESS_THREAD(udp_server_process, ev, data)
{
  static int connected = 0;
  static session_t dst;

  PROCESS_BEGIN();

  dtls_init();

  init_dtls(&dst);
  serial_line_init();
#ifdef CONTIKI_TARGET_RM090
  uart1_set_input(serial_line_input_byte);
#endif

  if (!dtls_context) {
    dtls_emerg("cannot create context\n");
    PROCESS_EXIT();
  }

  while(1) {
    PRINTF("PROCESS_YIELD()");
    PROCESS_YIELD();
    PRINTF("PROCESS_YIELD() returned");
    if(ev == tcpip_event) {
      PRINTF("tcpip_event");
      dtls_handle_read(dtls_context);
    } else if (ev == serial_line_event_message) {
      PRINTF("serial_line_event_message");
      register size_t len = min(strlen(data), sizeof(buf) - buflen);
      memcpy(buf + buflen, data, len);
      buflen += len;
      if (buflen < sizeof(buf) - 1)
	buf[buflen++] = '\n';	/* serial event does not contain LF */
    }

    if (buflen) {
      if (!connected)
	connected = dtls_connect(dtls_context, &dst) >= 0;
      
      try_send(dtls_context, &dst);
    }
  }
  
  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
