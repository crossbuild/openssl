/* ssl/d1_lib.c */
/*
 * DTLS implementation written by Nagendra Modadugu
 * (nagendra@cs.stanford.edu) for the OpenSSL project 2005.
 */
/* ====================================================================
 * Copyright (c) 1999-2005 The OpenSSL Project.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. All advertising materials mentioning features or use of this
 *    software must display the following acknowledgment:
 *    "This product includes software developed by the OpenSSL Project
 *    for use in the OpenSSL Toolkit. (http://www.OpenSSL.org/)"
 *
 * 4. The names "OpenSSL Toolkit" and "OpenSSL Project" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission. For written permission, please contact
 *    openssl-core@OpenSSL.org.
 *
 * 5. Products derived from this software may not be called "OpenSSL"
 *    nor may "OpenSSL" appear in their names without prior written
 *    permission of the OpenSSL Project.
 *
 * 6. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by the OpenSSL Project
 *    for use in the OpenSSL Toolkit (http://www.OpenSSL.org/)"
 *
 * THIS SOFTWARE IS PROVIDED BY THE OpenSSL PROJECT ``AS IS'' AND ANY
 * EXPRESSED OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE OpenSSL PROJECT OR
 * ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 * ====================================================================
 *
 * This product includes cryptographic software written by Eric Young
 * (eay@cryptsoft.com).  This product includes software written by Tim
 * Hudson (tjh@cryptsoft.com).
 *
 */

#include <stdio.h>
#define USE_SOCKETS
#include <openssl/objects.h>
#include <openssl/rand.h>
#include "ssl_locl.h"

#if defined(OPENSSL_SYS_VMS)
# include <sys/timeb.h>
#elif defined(OPENSSL_SYS_NETWARE) && !defined(_WINSOCK2API_)
# include <sys/timeval.h>
#elif defined(OPENSSL_SYS_VXWORKS)
# include <sys/times.h>
#elif !defined(OPENSSL_SYS_WIN32)
# include <sys/time.h>
#endif

static void get_current_time(struct timeval *t);
static int dtls1_set_handshake_header(SSL *s, int type, unsigned long len);
static int dtls1_handshake_write(SSL *s);
int dtls1_listen(SSL *s, struct sockaddr *client);
static unsigned int dtls1_link_min_mtu(void);

/* XDTLS:  figure out the right values */
static const unsigned int g_probable_mtu[] = { 1500, 512, 256 };

const SSL3_ENC_METHOD DTLSv1_enc_data = {
    tls1_enc,
    tls1_mac,
    tls1_setup_key_block,
    tls1_generate_master_secret,
    tls1_change_cipher_state,
    tls1_final_finish_mac,
    TLS1_FINISH_MAC_LENGTH,
    tls1_cert_verify_mac,
    TLS_MD_CLIENT_FINISH_CONST, TLS_MD_CLIENT_FINISH_CONST_SIZE,
    TLS_MD_SERVER_FINISH_CONST, TLS_MD_SERVER_FINISH_CONST_SIZE,
    tls1_alert_code,
    tls1_export_keying_material,
    SSL_ENC_FLAG_DTLS | SSL_ENC_FLAG_EXPLICIT_IV,
    DTLS1_HM_HEADER_LENGTH,
    dtls1_set_handshake_header,
    dtls1_handshake_write
};

const SSL3_ENC_METHOD DTLSv1_2_enc_data = {
    tls1_enc,
    tls1_mac,
    tls1_setup_key_block,
    tls1_generate_master_secret,
    tls1_change_cipher_state,
    tls1_final_finish_mac,
    TLS1_FINISH_MAC_LENGTH,
    tls1_cert_verify_mac,
    TLS_MD_CLIENT_FINISH_CONST, TLS_MD_CLIENT_FINISH_CONST_SIZE,
    TLS_MD_SERVER_FINISH_CONST, TLS_MD_SERVER_FINISH_CONST_SIZE,
    tls1_alert_code,
    tls1_export_keying_material,
    SSL_ENC_FLAG_DTLS | SSL_ENC_FLAG_EXPLICIT_IV | SSL_ENC_FLAG_SIGALGS
        | SSL_ENC_FLAG_SHA256_PRF | SSL_ENC_FLAG_TLS1_2_CIPHERS,
    DTLS1_HM_HEADER_LENGTH,
    dtls1_set_handshake_header,
    dtls1_handshake_write
};

long dtls1_default_timeout(void)
{
    /*
     * 2 hours, the 24 hours mentioned in the DTLSv1 spec is way too long for
     * http, the cache would over fill
     */
    return (60 * 60 * 2);
}

int dtls1_new(SSL *s)
{
    DTLS1_STATE *d1;

    if (!DTLS_RECORD_LAYER_new(&s->rlayer)) {
        return 0;
    }
    
    if (!ssl3_new(s))
        return (0);
    if ((d1 = OPENSSL_zalloc(sizeof(*d1))) == NULL) {
        ssl3_free(s);
        return (0);
    }

    d1->buffered_messages = pqueue_new();
    d1->sent_messages = pqueue_new();

    if (s->server) {
        d1->cookie_len = sizeof(s->d1->cookie);
    }

    d1->link_mtu = 0;
    d1->mtu = 0;

    if (!d1->buffered_messages || !d1->sent_messages) {
        pqueue_free(d1->buffered_messages);
        pqueue_free(d1->sent_messages);
        OPENSSL_free(d1);
        ssl3_free(s);
        return (0);
    }

    s->d1 = d1;
    s->method->ssl_clear(s);
    return (1);
}

static void dtls1_clear_queues(SSL *s)
{
    pitem *item = NULL;
    hm_fragment *frag = NULL;

    while ((item = pqueue_pop(s->d1->buffered_messages)) != NULL) {
        frag = (hm_fragment *)item->data;
        dtls1_hm_fragment_free(frag);
        pitem_free(item);
    }

    while ((item = pqueue_pop(s->d1->sent_messages)) != NULL) {
        frag = (hm_fragment *)item->data;
        dtls1_hm_fragment_free(frag);
        pitem_free(item);
    }
}

void dtls1_free(SSL *s)
{
    DTLS_RECORD_LAYER_free(&s->rlayer);

    ssl3_free(s);

    dtls1_clear_queues(s);

    pqueue_free(s->d1->buffered_messages);
    pqueue_free(s->d1->sent_messages);

    OPENSSL_free(s->d1);
    s->d1 = NULL;
}

void dtls1_clear(SSL *s)
{
    pqueue buffered_messages;
    pqueue sent_messages;
    unsigned int mtu;
    unsigned int link_mtu;

    DTLS_RECORD_LAYER_clear(&s->rlayer);

    if (s->d1) {
        buffered_messages = s->d1->buffered_messages;
        sent_messages = s->d1->sent_messages;
        mtu = s->d1->mtu;
        link_mtu = s->d1->link_mtu;

        dtls1_clear_queues(s);

        memset(s->d1, 0, sizeof(*s->d1));

        if (s->server) {
            s->d1->cookie_len = sizeof(s->d1->cookie);
        }

        if (SSL_get_options(s) & SSL_OP_NO_QUERY_MTU) {
            s->d1->mtu = mtu;
            s->d1->link_mtu = link_mtu;
        }

        s->d1->buffered_messages = buffered_messages;
        s->d1->sent_messages = sent_messages;
    }

    ssl3_clear(s);
    if (s->options & SSL_OP_CISCO_ANYCONNECT)
        s->client_version = s->version = DTLS1_BAD_VER;
    else if (s->method->version == DTLS_ANY_VERSION)
        s->version = DTLS1_2_VERSION;
    else
        s->version = s->method->version;
}

long dtls1_ctrl(SSL *s, int cmd, long larg, void *parg)
{
    int ret = 0;

    switch (cmd) {
    case DTLS_CTRL_GET_TIMEOUT:
        if (dtls1_get_timeout(s, (struct timeval *)parg) != NULL) {
            ret = 1;
        }
        break;
    case DTLS_CTRL_HANDLE_TIMEOUT:
        ret = dtls1_handle_timeout(s);
        break;
    case DTLS_CTRL_LISTEN:
        ret = dtls1_listen(s, parg);
        break;
    case SSL_CTRL_CHECK_PROTO_VERSION:
        /*
         * For library-internal use; checks that the current protocol is the
         * highest enabled version (according to s->ctx->method, as version
         * negotiation may have changed s->method).
         */
        if (s->version == s->ctx->method->version)
            return 1;
        /*
         * Apparently we're using a version-flexible SSL_METHOD (not at its
         * highest protocol version).
         */
        if (s->ctx->method->version == DTLS_method()->version) {
#if DTLS_MAX_VERSION != DTLS1_2_VERSION
# error Code needs update for DTLS_method() support beyond DTLS1_2_VERSION.
#endif
            if (!(s->options & SSL_OP_NO_DTLSv1_2))
                return s->version == DTLS1_2_VERSION;
            if (!(s->options & SSL_OP_NO_DTLSv1))
                return s->version == DTLS1_VERSION;
        }
        return 0;               /* Unexpected state; fail closed. */
    case DTLS_CTRL_SET_LINK_MTU:
        if (larg < (long)dtls1_link_min_mtu())
            return 0;
        s->d1->link_mtu = larg;
        return 1;
    case DTLS_CTRL_GET_LINK_MIN_MTU:
        return (long)dtls1_link_min_mtu();
    case SSL_CTRL_SET_MTU:
        /*
         *  We may not have a BIO set yet so can't call dtls1_min_mtu()
         *  We'll have to make do with dtls1_link_min_mtu() and max overhead
         */
        if (larg < (long)dtls1_link_min_mtu() - DTLS1_MAX_MTU_OVERHEAD)
            return 0;
        s->d1->mtu = larg;
        return larg;
    default:
        ret = ssl3_ctrl(s, cmd, larg, parg);
        break;
    }
    return (ret);
}

/*
 * As it's impossible to use stream ciphers in "datagram" mode, this
 * simple filter is designed to disengage them in DTLS. Unfortunately
 * there is no universal way to identify stream SSL_CIPHER, so we have
 * to explicitly list their SSL_* codes. Currently RC4 is the only one
 * available, but if new ones emerge, they will have to be added...
 */
const SSL_CIPHER *dtls1_get_cipher(unsigned int u)
{
    const SSL_CIPHER *ciph = ssl3_get_cipher(u);

    if (ciph != NULL) {
        if (ciph->algorithm_enc == SSL_RC4)
            return NULL;
    }

    return ciph;
}

void dtls1_start_timer(SSL *s)
{
#ifndef OPENSSL_NO_SCTP
    /* Disable timer for SCTP */
    if (BIO_dgram_is_sctp(SSL_get_wbio(s))) {
        memset(&s->d1->next_timeout, 0, sizeof(s->d1->next_timeout));
        return;
    }
#endif

    /* If timer is not set, initialize duration with 1 second */
    if (s->d1->next_timeout.tv_sec == 0 && s->d1->next_timeout.tv_usec == 0) {
        s->d1->timeout_duration = 1;
    }

    /* Set timeout to current time */
    get_current_time(&(s->d1->next_timeout));

    /* Add duration to current time */
    s->d1->next_timeout.tv_sec += s->d1->timeout_duration;
    BIO_ctrl(SSL_get_rbio(s), BIO_CTRL_DGRAM_SET_NEXT_TIMEOUT, 0,
             &(s->d1->next_timeout));
}

struct timeval *dtls1_get_timeout(SSL *s, struct timeval *timeleft)
{
    struct timeval timenow;

    /* If no timeout is set, just return NULL */
    if (s->d1->next_timeout.tv_sec == 0 && s->d1->next_timeout.tv_usec == 0) {
        return NULL;
    }

    /* Get current time */
    get_current_time(&timenow);

    /* If timer already expired, set remaining time to 0 */
    if (s->d1->next_timeout.tv_sec < timenow.tv_sec ||
        (s->d1->next_timeout.tv_sec == timenow.tv_sec &&
         s->d1->next_timeout.tv_usec <= timenow.tv_usec)) {
        memset(timeleft, 0, sizeof(*timeleft));
        return timeleft;
    }

    /* Calculate time left until timer expires */
    memcpy(timeleft, &(s->d1->next_timeout), sizeof(struct timeval));
    timeleft->tv_sec -= timenow.tv_sec;
    timeleft->tv_usec -= timenow.tv_usec;
    if (timeleft->tv_usec < 0) {
        timeleft->tv_sec--;
        timeleft->tv_usec += 1000000;
    }

    /*
     * If remaining time is less than 15 ms, set it to 0 to prevent issues
     * because of small devergences with socket timeouts.
     */
    if (timeleft->tv_sec == 0 && timeleft->tv_usec < 15000) {
        memset(timeleft, 0, sizeof(*timeleft));
    }

    return timeleft;
}

int dtls1_is_timer_expired(SSL *s)
{
    struct timeval timeleft;

    /* Get time left until timeout, return false if no timer running */
    if (dtls1_get_timeout(s, &timeleft) == NULL) {
        return 0;
    }

    /* Return false if timer is not expired yet */
    if (timeleft.tv_sec > 0 || timeleft.tv_usec > 0) {
        return 0;
    }

    /* Timer expired, so return true */
    return 1;
}

void dtls1_double_timeout(SSL *s)
{
    s->d1->timeout_duration *= 2;
    if (s->d1->timeout_duration > 60)
        s->d1->timeout_duration = 60;
    dtls1_start_timer(s);
}

void dtls1_stop_timer(SSL *s)
{
    /* Reset everything */
    memset(&s->d1->timeout, 0, sizeof(s->d1->timeout));
    memset(&s->d1->next_timeout, 0, sizeof(s->d1->next_timeout));
    s->d1->timeout_duration = 1;
    BIO_ctrl(SSL_get_rbio(s), BIO_CTRL_DGRAM_SET_NEXT_TIMEOUT, 0,
             &(s->d1->next_timeout));
    /* Clear retransmission buffer */
    dtls1_clear_record_buffer(s);
}

int dtls1_check_timeout_num(SSL *s)
{
    unsigned int mtu;

    s->d1->timeout.num_alerts++;

    /* Reduce MTU after 2 unsuccessful retransmissions */
    if (s->d1->timeout.num_alerts > 2
        && !(SSL_get_options(s) & SSL_OP_NO_QUERY_MTU)) {
        mtu =
            BIO_ctrl(SSL_get_wbio(s), BIO_CTRL_DGRAM_GET_FALLBACK_MTU, 0,
                     NULL);
        if (mtu < s->d1->mtu)
            s->d1->mtu = mtu;
    }

    if (s->d1->timeout.num_alerts > DTLS1_TMO_ALERT_COUNT) {
        /* fail the connection, enough alerts have been sent */
        SSLerr(SSL_F_DTLS1_CHECK_TIMEOUT_NUM, SSL_R_READ_TIMEOUT_EXPIRED);
        return -1;
    }

    return 0;
}

int dtls1_handle_timeout(SSL *s)
{
    /* if no timer is expired, don't do anything */
    if (!dtls1_is_timer_expired(s)) {
        return 0;
    }

    dtls1_double_timeout(s);

    if (dtls1_check_timeout_num(s) < 0)
        return -1;

    s->d1->timeout.read_timeouts++;
    if (s->d1->timeout.read_timeouts > DTLS1_TMO_READ_COUNT) {
        s->d1->timeout.read_timeouts = 1;
    }
#ifndef OPENSSL_NO_HEARTBEATS
    if (s->tlsext_hb_pending) {
        s->tlsext_hb_pending = 0;
        return dtls1_heartbeat(s);
    }
#endif

    dtls1_start_timer(s);
    return dtls1_retransmit_buffered_messages(s);
}

static void get_current_time(struct timeval *t)
{
#if defined(_WIN32)
    SYSTEMTIME st;
    union {
        unsigned __int64 ul;
        FILETIME ft;
    } now;

    GetSystemTime(&st);
    SystemTimeToFileTime(&st, &now.ft);
# ifdef  __MINGW32__
    now.ul -= 116444736000000000ULL;
# else
    now.ul -= 116444736000000000UI64; /* re-bias to 1/1/1970 */
# endif
    t->tv_sec = (long)(now.ul / 10000000);
    t->tv_usec = ((int)(now.ul % 10000000)) / 10;
#elif defined(OPENSSL_SYS_VMS)
    struct timeb tb;
    ftime(&tb);
    t->tv_sec = (long)tb.time;
    t->tv_usec = (long)tb.millitm * 1000;
#else
    gettimeofday(t, NULL);
#endif
}


#define LISTEN_SUCCESS              2
#define LISTEN_SEND_VERIFY_REQUEST  1


int dtls1_listen(SSL *s, struct sockaddr *client)
{
    int next, n, ret = 0, clearpkt = 0;
    unsigned char cookie[DTLS1_COOKIE_LENGTH];
    unsigned char seq[SEQ_NUM_SIZE];
    unsigned char *data, *p, *buf;
    unsigned long reclen, fragoff, fraglen, msglen;
    unsigned int rectype, versmajor, msgseq, msgtype, clientvers, cookielen;
    BIO *rbio, *wbio;
    BUF_MEM *bufm;
    struct sockaddr_storage tmpclient;
    PACKET pkt, msgpkt, msgpayload, session, cookiepkt;

    /* Ensure there is no state left over from a previous invocation */
    if (!SSL_clear(s))
        return -1;

    ERR_clear_error();

    rbio = SSL_get_rbio(s);
    wbio = SSL_get_wbio(s);

    if(!rbio || !wbio) {
        SSLerr(SSL_F_DTLS1_LISTEN, SSL_R_BIO_NOT_SET);
        return -1;
    }

    /*
     * We only peek at incoming ClientHello's until we're sure we are going to
     * to respond with a HelloVerifyRequest. If its a ClientHello with a valid
     * cookie then we leave it in the BIO for dtls1_accept to handle.
     */
    BIO_ctrl(SSL_get_rbio(s), BIO_CTRL_DGRAM_SET_PEEK_MODE, 1, NULL);

    /*
     * Note: This check deliberately excludes DTLS1_BAD_VER because that version
     * requires the MAC to be calculated *including* the first ClientHello
     * (without the cookie). Since DTLSv1_listen is stateless that cannot be
     * supported. DTLS1_BAD_VER must use cookies in a stateful manner (e.g. via
     * SSL_accept)
     */
    if ((s->version & 0xff00) != (DTLS1_VERSION & 0xff00)) {
        SSLerr(SSL_F_DTLS1_LISTEN, SSL_R_UNSUPPORTED_SSL_VERSION);
        return -1;
    }

    if (s->init_buf == NULL) {
        if ((bufm = BUF_MEM_new()) == NULL) {
            SSLerr(SSL_F_DTLS1_LISTEN, ERR_R_MALLOC_FAILURE);
            return -1;
        }

        if (!BUF_MEM_grow(bufm, SSL3_RT_MAX_PLAIN_LENGTH)) {
            BUF_MEM_free(bufm);
            SSLerr(SSL_F_DTLS1_LISTEN, ERR_R_MALLOC_FAILURE);
            return -1;
        }
        s->init_buf = bufm;
    }
    buf = (unsigned char *)s->init_buf->data;

    do {
        /* Get a packet */

        clear_sys_error();
        /*
         * Technically a ClientHello could be SSL3_RT_MAX_PLAIN_LENGTH
         * + DTLS1_RT_HEADER_LENGTH bytes long. Normally init_buf does not store
         * the record header as well, but we do here. We've set up init_buf to
         * be the standard size for simplicity. In practice we shouldn't ever
         * receive a ClientHello as long as this. If we do it will get dropped
         * in the record length check below.
         */
        n = BIO_read(rbio, buf, SSL3_RT_MAX_PLAIN_LENGTH);

        if (n <= 0) {
            if(BIO_should_retry(rbio)) {
                /* Non-blocking IO */
                goto end;
            }
            return -1;
        }

        /* If we hit any problems we need to clear this packet from the BIO */
        clearpkt = 1;

        if (!PACKET_buf_init(&pkt, buf, n)) {
            SSLerr(SSL_F_DTLS1_LISTEN, ERR_R_INTERNAL_ERROR);
            return -1;
        }

        /*
         * Parse the received record. If there are any problems with it we just
         * dump it - with no alert. RFC6347 says this "Unlike TLS, DTLS is
         * resilient in the face of invalid records (e.g., invalid formatting,
         * length, MAC, etc.).  In general, invalid records SHOULD be silently
         * discarded, thus preserving the association; however, an error MAY be
         * logged for diagnostic purposes."
         */

        /* this packet contained a partial record, dump it */
        if (n < DTLS1_RT_HEADER_LENGTH) {
            SSLerr(SSL_F_DTLS1_LISTEN, SSL_R_RECORD_TOO_SMALL);
            goto end;
        }

        if (s->msg_callback)
            s->msg_callback(0, 0, SSL3_RT_HEADER, buf,
                            DTLS1_RT_HEADER_LENGTH, s, s->msg_callback_arg);

        /* Get the record header */
        if (!PACKET_get_1(&pkt, &rectype)
            || !PACKET_get_1(&pkt, &versmajor)) {
            SSLerr(SSL_F_DTLS1_LISTEN, SSL_R_LENGTH_MISMATCH);
            goto end;
        }

        if (rectype != SSL3_RT_HANDSHAKE)  {
            SSLerr(SSL_F_DTLS1_LISTEN, SSL_R_UNEXPECTED_MESSAGE);
            goto end;
        }

        /*
         * Check record version number. We only check that the major version is
         * the same.
         */
        if (versmajor != DTLS1_VERSION_MAJOR) {
            SSLerr(SSL_F_DTLS1_LISTEN, SSL_R_BAD_PROTOCOL_VERSION_NUMBER);
            goto end;
        }

        if (!PACKET_forward(&pkt, 1)
            /* Save the sequence number: 64 bits, with top 2 bytes = epoch */
            || !PACKET_copy_bytes(&pkt, seq, SEQ_NUM_SIZE)
            || !PACKET_get_length_prefixed_2(&pkt, &msgpkt)
            || PACKET_remaining(&pkt) != 0) {
            SSLerr(SSL_F_DTLS1_LISTEN, SSL_R_LENGTH_MISMATCH);
            goto end;
        }

        /* This is an initial ClientHello so the epoch has to be 0 */
        if (seq[0] != 0 || seq[1] != 0) {
            SSLerr(SSL_F_DTLS1_LISTEN, SSL_R_UNEXPECTED_MESSAGE);
            goto end;
        }

        /* Get a pointer to the raw message for the later callback */
        data = PACKET_data(&msgpkt);

        /* Finished processing the record header, now process the message */
        if (!PACKET_get_1(&msgpkt, &msgtype)
            || !PACKET_get_net_3(&msgpkt, &msglen)
            || !PACKET_get_net_2(&msgpkt, &msgseq)
            || !PACKET_get_net_3(&msgpkt, &fragoff)
            || !PACKET_get_net_3(&msgpkt, &fraglen)
            || !PACKET_get_sub_packet(&msgpkt, &msgpayload, msglen)
            || PACKET_remaining(&msgpkt) != 0) {
            SSLerr(SSL_F_DTLS1_LISTEN, SSL_R_LENGTH_MISMATCH);
            goto end;
        }

        if (msgtype != SSL3_MT_CLIENT_HELLO) {
            SSLerr(SSL_F_DTLS1_LISTEN, SSL_R_UNEXPECTED_MESSAGE);
            goto end;
        }

        /* Message sequence number can only be 0 or 1 */
        if(msgseq > 2) {
            SSLerr(SSL_F_DTLS1_LISTEN, SSL_R_INVALID_SEQUENCE_NUMBER);
            goto end;
        }

        /* We don't support a fragmented ClientHello whilst listening */
        if (fragoff != 0 || fraglen != msglen) {
            SSLerr(SSL_F_DTLS1_LISTEN, SSL_R_FRAGMENTED_CLIENT_HELLO);
            goto end;
        }

        if (s->msg_callback)
            s->msg_callback(0, s->version, SSL3_RT_HANDSHAKE, data,
                            msglen + DTLS1_HM_HEADER_LENGTH, s,
                            s->msg_callback_arg);

        if (!PACKET_get_net_2(&msgpayload, &clientvers)) {
            SSLerr(SSL_F_DTLS1_LISTEN, SSL_R_LENGTH_MISMATCH);
            goto end;
        }

        /*
         * Verify client version is supported
         */
        if ((clientvers > (unsigned int)s->method->version &&
                              s->method->version != DTLS_ANY_VERSION)) {
            SSLerr(SSL_F_DTLS1_LISTEN, SSL_R_WRONG_VERSION_NUMBER);
            goto end;
        }

        if (!PACKET_forward(&msgpayload, SSL3_RANDOM_SIZE)
            || !PACKET_get_length_prefixed_1(&msgpayload, &session)
            || !PACKET_get_length_prefixed_1(&msgpayload, &cookiepkt)) {
            SSLerr(SSL_F_DTLS1_LISTEN, SSL_R_LENGTH_MISMATCH);
            goto end;
        }

        /*
         * Check if we have a cookie or not. If not we need to send a
         * HelloVerifyRequest.
         */
        if (PACKET_remaining(&cookiepkt) == 0) {
            next = LISTEN_SEND_VERIFY_REQUEST;
        } else {
            /*
             * We have a cookie, so lets check it.
             */
            if (s->ctx->app_verify_cookie_cb == NULL) {
                SSLerr(SSL_F_DTLS1_LISTEN, SSL_R_NO_VERIFY_COOKIE_CALLBACK);
                /* This is fatal */
                return -1;
            }
            if (s->ctx->app_verify_cookie_cb(s, PACKET_data(&cookiepkt),
                                             PACKET_remaining(&cookiepkt)) ==
                0) {
                /*
                 * We treat invalid cookies in the same was as no cookie as
                 * per RFC6347
                 */
                next = LISTEN_SEND_VERIFY_REQUEST;
            } else {
                /* Cookie verification succeeded */
                next = LISTEN_SUCCESS;
            }
        }

        if (next == LISTEN_SEND_VERIFY_REQUEST) {
            /*
             * There was no cookie in the ClientHello so we need to send a
             * HelloVerifyRequest. If this fails we do not worry about trying
             * to resend, we just drop it.
             */

            /*
             * Dump the read packet, we don't need it any more. Ignore return
             * value
             */
            BIO_ctrl(SSL_get_rbio(s), BIO_CTRL_DGRAM_SET_PEEK_MODE, 0, NULL);
            BIO_read(rbio, buf, SSL3_RT_MAX_PLAIN_LENGTH);
            BIO_ctrl(SSL_get_rbio(s), BIO_CTRL_DGRAM_SET_PEEK_MODE, 1, NULL);

            /* Generate the cookie */
            if (s->ctx->app_gen_cookie_cb == NULL ||
                s->ctx->app_gen_cookie_cb(s, cookie, &cookielen) == 0 ||
                cookielen > 255) {
                SSLerr(SSL_F_DTLS1_LISTEN, SSL_R_COOKIE_GEN_CALLBACK_FAILURE);
                /* This is fatal */
                return -1;
            }

            p = &buf[DTLS1_RT_HEADER_LENGTH];
            msglen = dtls_raw_hello_verify_request(p + DTLS1_HM_HEADER_LENGTH,
                                                   cookie, cookielen);

            *p++ = DTLS1_MT_HELLO_VERIFY_REQUEST;

            /* Message length */
            l2n3(msglen, p);

            /* Message sequence number is always 0 for a HelloVerifyRequest */
            s2n(0, p);

            /*
             * We never fragment a HelloVerifyRequest, so fragment offset is 0
             * and fragment length is message length
             */
            l2n3(0, p);
            l2n3(msglen, p);

            /* Set reclen equal to length of whole handshake message */
            reclen = msglen + DTLS1_HM_HEADER_LENGTH;

            /* Add the record header */
            p = buf;

            *(p++) = SSL3_RT_HANDSHAKE;
            /*
             * Special case: for hello verify request, client version 1.0 and we
             * haven't decided which version to use yet send back using version
             * 1.0 header: otherwise some clients will ignore it.
             */
            if (s->method->version == DTLS_ANY_VERSION) {
                *(p++) = DTLS1_VERSION >> 8;
                *(p++) = DTLS1_VERSION & 0xff;
            } else {
                *(p++) = s->version >> 8;
                *(p++) = s->version & 0xff;
            }

            /*
             * Record sequence number is always the same as in the received
             * ClientHello
             */
            memcpy(p, seq, SEQ_NUM_SIZE);
            p += SEQ_NUM_SIZE;

            /* Length */
            s2n(reclen, p);

            /*
             * Set reclen equal to length of whole record including record
             * header
             */
            reclen += DTLS1_RT_HEADER_LENGTH;

            if (s->msg_callback)
                s->msg_callback(1, 0, SSL3_RT_HEADER, buf,
                                DTLS1_RT_HEADER_LENGTH, s, s->msg_callback_arg);

            /*
             * This is unneccessary if rbio and wbio are one and the same - but
             * maybe they're not.
             */
            if(BIO_dgram_get_peer(rbio, &tmpclient) <= 0
               || BIO_dgram_set_peer(wbio, &tmpclient) <= 0) {
                SSLerr(SSL_F_DTLS1_LISTEN, ERR_R_INTERNAL_ERROR);
                goto end;
            }

            if (BIO_write(wbio, buf, reclen) < (int)reclen) {
                if(BIO_should_retry(wbio)) {
                    /*
                     * Non-blocking IO...but we're stateless, so we're just
                     * going to drop this packet.
                     */
                    goto end;
                }
                return -1;
            }

            if (BIO_flush(wbio) <= 0) {
                if(BIO_should_retry(wbio)) {
                    /*
                     * Non-blocking IO...but we're stateless, so we're just
                     * going to drop this packet.
                     */
                    goto end;
                }
                return -1;
            }
        }
    } while (next != LISTEN_SUCCESS);

    /*
     * Set expected sequence numbers to continue the handshake.
     */
    s->d1->handshake_read_seq = 1;
    s->d1->handshake_write_seq = 1;
    s->d1->next_handshake_write_seq = 1;
    DTLS_RECORD_LAYER_set_write_sequence(&s->rlayer, seq);

    /*
     * We are doing cookie exchange, so make sure we set that option in the
     * SSL object
     */
    SSL_set_options(s, SSL_OP_COOKIE_EXCHANGE);

    /*
     * Tell the state machine that we've done the initial hello verify
     * exchange
     */
    ossl_statem_set_hello_verify_done(s);

    if(BIO_dgram_get_peer(rbio, client) <= 0) {
        SSLerr(SSL_F_DTLS1_LISTEN, ERR_R_INTERNAL_ERROR);
        return -1;
    }

    ret = 1;
    clearpkt = 0;
end:
    BIO_ctrl(SSL_get_rbio(s), BIO_CTRL_DGRAM_SET_PEEK_MODE, 0, NULL);
    if (clearpkt) {
        /* Dump this packet. Ignore return value */
        BIO_read(rbio, buf, SSL3_RT_MAX_PLAIN_LENGTH);
    }
    return ret;
}

static int dtls1_set_handshake_header(SSL *s, int htype, unsigned long len)
{
    unsigned char *p = (unsigned char *)s->init_buf->data;
    dtls1_set_message_header(s, p, htype, len, 0, len);
    s->init_num = (int)len + DTLS1_HM_HEADER_LENGTH;
    s->init_off = 0;
    /* Buffer the message to handle re-xmits */

    if (!dtls1_buffer_message(s, 0))
        return 0;

    return 1;
}

static int dtls1_handshake_write(SSL *s)
{
    return dtls1_do_write(s, SSL3_RT_HANDSHAKE);
}

#ifndef OPENSSL_NO_HEARTBEATS
int dtls1_process_heartbeat(SSL *s, unsigned char *p, unsigned int length)
{
    unsigned char *pl;
    unsigned short hbtype;
    unsigned int payload;
    unsigned int padding = 16;  /* Use minimum padding */

    if (s->msg_callback)
        s->msg_callback(0, s->version, TLS1_RT_HEARTBEAT,
                        p, length, s, s->msg_callback_arg);

    /* Read type and payload length first */
    if (1 + 2 + 16 > length)
        return 0;               /* silently discard */
    if (length > SSL3_RT_MAX_PLAIN_LENGTH)
        return 0;               /* silently discard per RFC 6520 sec. 4 */

    hbtype = *p++;
    n2s(p, payload);
    if (1 + 2 + payload + 16 > length)
        return 0;               /* silently discard per RFC 6520 sec. 4 */
    pl = p;

    if (hbtype == TLS1_HB_REQUEST) {
        unsigned char *buffer, *bp;
        unsigned int write_length = 1 /* heartbeat type */  +
            2 /* heartbeat length */  +
            payload + padding;
        int r;

        if (write_length > SSL3_RT_MAX_PLAIN_LENGTH)
            return 0;

        /*
         * Allocate memory for the response, size is 1 byte message type,
         * plus 2 bytes payload length, plus payload, plus padding
         */
        buffer = OPENSSL_malloc(write_length);
        if (buffer == NULL)
            return -1;
        bp = buffer;

        /* Enter response type, length and copy payload */
        *bp++ = TLS1_HB_RESPONSE;
        s2n(payload, bp);
        memcpy(bp, pl, payload);
        bp += payload;
        /* Random padding */
        if (RAND_bytes(bp, padding) <= 0) {
            OPENSSL_free(buffer);
            return -1;
        }

        r = dtls1_write_bytes(s, TLS1_RT_HEARTBEAT, buffer, write_length);

        if (r >= 0 && s->msg_callback)
            s->msg_callback(1, s->version, TLS1_RT_HEARTBEAT,
                            buffer, write_length, s, s->msg_callback_arg);

        OPENSSL_free(buffer);

        if (r < 0)
            return r;
    } else if (hbtype == TLS1_HB_RESPONSE) {
        unsigned int seq;

        /*
         * We only send sequence numbers (2 bytes unsigned int), and 16
         * random bytes, so we just try to read the sequence number
         */
        n2s(pl, seq);

        if (payload == 18 && seq == s->tlsext_hb_seq) {
            dtls1_stop_timer(s);
            s->tlsext_hb_seq++;
            s->tlsext_hb_pending = 0;
        }
    }

    return 0;
}

int dtls1_heartbeat(SSL *s)
{
    unsigned char *buf, *p;
    int ret = -1;
    unsigned int payload = 18;  /* Sequence number + random bytes */
    unsigned int padding = 16;  /* Use minimum padding */

    /* Only send if peer supports and accepts HB requests... */
    if (!(s->tlsext_heartbeat & SSL_TLSEXT_HB_ENABLED) ||
        s->tlsext_heartbeat & SSL_TLSEXT_HB_DONT_SEND_REQUESTS) {
        SSLerr(SSL_F_DTLS1_HEARTBEAT, SSL_R_TLS_HEARTBEAT_PEER_DOESNT_ACCEPT);
        return -1;
    }

    /* ...and there is none in flight yet... */
    if (s->tlsext_hb_pending) {
        SSLerr(SSL_F_DTLS1_HEARTBEAT, SSL_R_TLS_HEARTBEAT_PENDING);
        return -1;
    }

    /* ...and no handshake in progress. */
    if (SSL_in_init(s) || ossl_statem_get_in_handshake(s)) {
        SSLerr(SSL_F_DTLS1_HEARTBEAT, SSL_R_UNEXPECTED_MESSAGE);
        return -1;
    }

    /*
     * Check if padding is too long, payload and padding must not exceed 2^14
     * - 3 = 16381 bytes in total.
     */
    OPENSSL_assert(payload + padding <= 16381);

    /*-
     * Create HeartBeat message, we just use a sequence number
     * as payload to distuingish different messages and add
     * some random stuff.
     *  - Message Type, 1 byte
     *  - Payload Length, 2 bytes (unsigned int)
     *  - Payload, the sequence number (2 bytes uint)
     *  - Payload, random bytes (16 bytes uint)
     *  - Padding
     */
    buf = OPENSSL_malloc(1 + 2 + payload + padding);
    if (buf == NULL) {
        SSLerr(SSL_F_DTLS1_HEARTBEAT, ERR_R_MALLOC_FAILURE);
        return -1;
    }
    p = buf;
    /* Message Type */
    *p++ = TLS1_HB_REQUEST;
    /* Payload length (18 bytes here) */
    s2n(payload, p);
    /* Sequence number */
    s2n(s->tlsext_hb_seq, p);
    /* 16 random bytes */
    if (RAND_bytes(p, 16) <= 0) {
        SSLerr(SSL_F_DTLS1_HEARTBEAT, ERR_R_INTERNAL_ERROR);
        goto err;
    }
    p += 16;
    /* Random padding */
    if (RAND_bytes(p, padding) <= 0) {
        SSLerr(SSL_F_DTLS1_HEARTBEAT, ERR_R_INTERNAL_ERROR);
        goto err;
    }

    ret = dtls1_write_bytes(s, TLS1_RT_HEARTBEAT, buf, 3 + payload + padding);
    if (ret >= 0) {
        if (s->msg_callback)
            s->msg_callback(1, s->version, TLS1_RT_HEARTBEAT,
                            buf, 3 + payload + padding,
                            s, s->msg_callback_arg);

        dtls1_start_timer(s);
        s->tlsext_hb_pending = 1;
    }

 err:
    OPENSSL_free(buf);

    return ret;
}
#endif

int dtls1_shutdown(SSL *s)
{
    int ret;
#ifndef OPENSSL_NO_SCTP
    BIO *wbio;

    wbio = SSL_get_wbio(s);
    if (wbio != NULL && BIO_dgram_is_sctp(wbio) &&
        !(s->shutdown & SSL_SENT_SHUTDOWN)) {
        ret = BIO_dgram_sctp_wait_for_dry(wbio);
        if (ret < 0)
            return -1;

        if (ret == 0)
            BIO_ctrl(SSL_get_wbio(s), BIO_CTRL_DGRAM_SCTP_SAVE_SHUTDOWN, 1,
                     NULL);
    }
#endif
    ret = ssl3_shutdown(s);
#ifndef OPENSSL_NO_SCTP
    BIO_ctrl(SSL_get_wbio(s), BIO_CTRL_DGRAM_SCTP_SAVE_SHUTDOWN, 0, NULL);
#endif
    return ret;
}

int dtls1_query_mtu(SSL *s)
{
    if (s->d1->link_mtu) {
        s->d1->mtu =
            s->d1->link_mtu - BIO_dgram_get_mtu_overhead(SSL_get_wbio(s));
        s->d1->link_mtu = 0;
    }

    /* AHA!  Figure out the MTU, and stick to the right size */
    if (s->d1->mtu < dtls1_min_mtu(s)) {
        if (!(SSL_get_options(s) & SSL_OP_NO_QUERY_MTU)) {
            s->d1->mtu =
                BIO_ctrl(SSL_get_wbio(s), BIO_CTRL_DGRAM_QUERY_MTU, 0, NULL);

            /*
             * I've seen the kernel return bogus numbers when it doesn't know
             * (initial write), so just make sure we have a reasonable number
             */
            if (s->d1->mtu < dtls1_min_mtu(s)) {
                /* Set to min mtu */
                s->d1->mtu = dtls1_min_mtu(s);
                BIO_ctrl(SSL_get_wbio(s), BIO_CTRL_DGRAM_SET_MTU,
                         s->d1->mtu, NULL);
            }
        } else
            return 0;
    }
    return 1;
}

static unsigned int dtls1_link_min_mtu(void)
{
    return (g_probable_mtu[(sizeof(g_probable_mtu) /
                            sizeof(g_probable_mtu[0])) - 1]);
}

unsigned int dtls1_min_mtu(SSL *s)
{
    return dtls1_link_min_mtu() - BIO_dgram_get_mtu_overhead(SSL_get_wbio(s));
}
