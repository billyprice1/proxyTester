/*
 * Copyright (c) 2014 Andrius Aucinas <andrius.aucinas@cl.cam.ac.uk>
 * 
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <android/log.h>
#include <chrono>
#include "testsuite.hpp"
#include "util.hpp"

#ifndef BUFLEN
#define BUFLEN 65535
#endif

#define TCPWINDOW (BUFLEN - IPHDRLEN - TCPHDRLEN)

const std::chrono::seconds sock_receive_timeout_sec(10);

void buildIPHeader(struct iphdr *ip, 
            uint32_t source, uint32_t destination,
            uint32_t data_length)
{
    ip->frag_off    = 0;
    ip->version     = 4;
    ip->ihl         = 5;
    ip->tot_len     = htons(IPHDRLEN + TCPHDRLEN + data_length);
    ip->id          = 0;
    ip->ttl         = 40;
    ip->protocol    = IPPROTO_TCP;
    ip->saddr       = source;
    ip->daddr       = destination;
    ip->check       = 0;
}

// Computes checksum of a TCP packet by creating a pseudoheader
// from provided IP packet and appending it after the payload.
// Since the checksum is a one's complement, 16-bit sum, appending
// or prepending has no difference other than having to be careful
// to add one zero-padding byte after data if length is not even
uint16_t tcpChecksum(struct iphdr *ip, struct tcphdr *tcp, int datalen) {
    // Add pseudoheader at the end of the packet for simplicity,
    struct pseudohdr * pseudoheader;
    // Need to add padding between data and pseudoheader
    // if data payload length is not a multiple of 2,
    // checksum is a 2 byte value.
    int padding = datalen % 2 ? 1 : 0;
    pseudoheader = (struct pseudohdr *) ( (u_int8_t *) tcp + TCPHDRLEN + datalen + padding );
    pseudoheader->src_addr = ip->saddr;
    pseudoheader->dst_addr = ip->daddr;
    pseudoheader->padding = 0;
    pseudoheader->proto = ip->protocol;
    pseudoheader->length = htons(TCPHDRLEN + datalen);
    // compute chekcsum from the bound of the tcp header to the appended pseudoheader
    uint16_t checksum = comp_chksum((uint16_t*) tcp,
            TCPHDRLEN + datalen + padding + PHDRLEN);
    return checksum;
}

// Function very specific to our tests and relies on packet checksum
// being computed in a specific way:
//      - The sending side decides the target checksum value
//      - Subtracts the destination IP address and port from that value
//      - If there is an intermediate NAT, it hopefully only modifies
//        the destination port and/or address and recomputes checksum
//        according to RFC3022
//      - It is sufficient to add back the (potentially new) destination address
//        to the received checksum to obtain the sender's target one
uint16_t undo_natting(struct iphdr *ip, struct tcphdr *tcp) {
    uint32_t checksum = ntohs(tcp->check);
    // Add back destination (own!) IP address and port number to undo what NAT modifies
    checksum = csum_add(checksum, ntohs(ip->daddr & 0xFFFF));
    checksum = csum_add(checksum, ntohs((ip->daddr >> 16) & 0xFFFF));
    checksum = csum_add(checksum, ntohs(tcp->dest));
    LOGD("Checksum NATing recalculated: %d, %04X", checksum, checksum);
    return (uint16_t) checksum;
}

// Check if the received packet is a valid one:
// the IP addresses and port numbers match the expected ones.
// We are using RAW sockets, which get a copy all incoming TCP traffic,
// so we need to filter out the packets that are not for us.
//
// param ip         IP header
// param tcp        TCP header
// param exp_src    expected packet source address (IP and port)
// param exp_dst    expected packet destination address (IP and port)
// return           true or false
bool validPacket(struct iphdr *ip, struct tcphdr *tcp,
    struct sockaddr_in *exp_src, struct sockaddr_in *exp_dst)
{
    if ( ip->saddr != exp_src->sin_addr.s_addr || ip->daddr != exp_dst->sin_addr.s_addr ) {
        // LOGE("Corrupted packet received: unexpected IP address");
        return false;
    } else if ( tcp->source != exp_src->sin_port || tcp->dest != exp_dst->sin_port ) {
        // LOGE("Corrupted packet received: unexpected port number");
        return false;
    } else {
        return true;
    }
}

// Receive one packet from the given socket.
// Blocks until there is a valid packet matching the expected connection
// or until recv fails (e.g. times out or the socket is closed).
//
// TODO: better logic for packet receive failure - currently could wait for a long
// time until recv fails (times out) if there is other TCP traffic
//
// param sock       The socket
// param ip         IP header
// param tcp        TCP header
// param exp_src    expected packet source address (IP and port)
// param exp_dst    expected packet destination address (IP and port)
// return           length of the packet read or -1 if recv returned -1
int receivePacket(int sock, struct iphdr *ip, struct tcphdr *tcp,
    struct sockaddr_in *exp_src, struct sockaddr_in *exp_dst)
{
    std::chrono::time_point<std::chrono::system_clock> start, now;
    start = std::chrono::system_clock::now();
    while (true) {
        int length = recv(sock, (char*)ip, BUFLEN, 0);
        // LOGD("Received %d bytes \n", length);
        // Error reading from socket or reading timed out - failure either way
        if (length == -1) {
            return length;
        }

        if (validPacket(ip, tcp, exp_src, exp_dst)) {
            printPacketInfo(ip, tcp);
            printBufferHex((char*)ip, length);
            return length;
        }
        else {
            // Read a packet that belongs to some other connection
            // try again unless we have exceeded receive timeout
            now = std::chrono::system_clock::now();
            if (now - start > sock_receive_timeout_sec) {
                LOGD("Packet reading timed out");
                return -1;
            }
        }
    }
}

bool sendPacket(int sock, char buffer[], struct sockaddr_in *dst, uint16_t len) {
    int bytes = sendto(sock, buffer, len, 0, (struct sockaddr*) dst, sizeof(*dst));
    if (bytes == -1) {
        LOGE("sendto() failed for data packet: %s", strerror(errno));
        return false;
    }
    return true;
}

// Function to receive SYNACK packet of TCP's three-way handshake.
// Wraps the normal receivePacket function call with SYNACK specific logic,
// checking for the right flags, sequence numbers and our testsuite-specific
// checks for the right values in the different parts of the header.
//
// param seq_local      local sequence number (check against ack)
// param sock           socket to receive synack from
// param ip             IP header
// param tcp            TCP header
// param exp_src        expected source address (remote!)
// param exp_dst        expected destination address (local!)
// param synack_urg     expected UGR pointer value
// param synack_check   expected checksum value (after running undo_natting)
// param synack_res     expected reserved field value
// return               execution status - success or a number of possible errors
test_error receiveTcpSynAck(uint32_t seq_local, int sock, 
            struct iphdr *ip, struct tcphdr *tcp,
            struct sockaddr_in *exp_src, struct sockaddr_in *exp_dst,
            uint16_t synack_urg, uint16_t synack_check, uint8_t synack_res)
{
    int packet_length = receivePacket(sock, ip, tcp, exp_src, exp_dst);
    if (packet_length < 0) 
        return receive_error;
    if (!tcp->syn || !tcp->ack) {
        LOGE("Not a SYNACK packet");
        return protocol_error;
    }
    if (seq_local != ntohl(tcp->ack_seq)) {
        LOGE("SYNACK packet unexpected ACK number: %d, %d", seq_local, ntohl(tcp->ack_seq));
        return sequence_error;
    }
    if (synack_urg != 0 && ntohs(tcp->urg_ptr) != synack_urg) {
        LOGE("SYNACK packet expected urg %04X, got: %04X", synack_urg, ntohs(tcp->urg_ptr));
        return synack_error_urg;
    }
    if (synack_check != 0) {
        uint16_t check = undo_natting(ip, tcp);
        if (synack_check != check) {
            LOGE("SYNACK packet expected check %04X, got: %04X", synack_check, check);
            return synack_error_urg;
        }
    }
    if (synack_res != 0 && synack_res != (tcp->res1 & 0xF) ) {
        LOGE("SYNACK packet expected res %02X, got: %02X", synack_res, (tcp->res1 & 0xF));
        return synack_error_urg;
    }
    
    return success;
}

// Build a TCP/IP SYN packet with the given
// ACK number, URG pointer and reserved field values
// Packet is pass-by-reference, new values stored there
void buildTcpSyn(struct sockaddr_in *src, struct sockaddr_in *dst,
            struct iphdr *ip, struct tcphdr *tcp,
            uint32_t syn_ack, uint32_t syn_urg, uint8_t syn_res) 
{
    // IP packet with TCP and no payload
    int datalen = 0;
    buildIPHeader(ip, src->sin_addr.s_addr, dst->sin_addr.s_addr, datalen);

    tcp->source     = src->sin_port;
    tcp->dest       = dst->sin_port;
    tcp->seq        = htonl(random() % 65535);
    tcp->ack_seq    = htonl(syn_ack);
    tcp->res1       = syn_res & 0xF;           // 4 bits reserved field
    tcp->doff       = 5;                        // Data offset 5 octets (no options)
    tcp->ack        = 0;
    tcp->psh        = 0;
    tcp->rst        = 0;
    tcp->urg        = 0;
    tcp->syn        = 1;
    tcp->fin        = 0;
    tcp->window     = htons(TCPWINDOW);
    tcp->urg_ptr    = htons(syn_urg);
    tcp->check      = 0;
    tcp->check      = tcpChecksum(ip, tcp, datalen);
    printPacketInfo(ip, tcp);
}

// Build a TCP/IP ACK packet with the given
// sequence numbers, everything else as standard, valid TCP
void buildTcpAck(struct sockaddr_in *src, struct sockaddr_in *dst,
            struct iphdr *ip, struct tcphdr *tcp,
            uint32_t seq_local, uint32_t seq_remote) 
{
    int datalen = 0;
    buildIPHeader(ip, src->sin_addr.s_addr, dst->sin_addr.s_addr, datalen);

    tcp->source     = src->sin_port;
    tcp->dest       = dst->sin_port;
    tcp->seq        = htonl(seq_local);
    tcp->ack_seq    = htonl(seq_remote);
    tcp->doff       = 5;
    tcp->ack        = 1;
    tcp->psh        = 0;
    tcp->rst        = 0;
    tcp->urg        = 0;
    tcp->syn        = 0;
    tcp->fin        = 0;
    tcp->window     = htons(TCPWINDOW);
    tcp->urg_ptr    = 0;
    tcp->check      = 0;
    tcp->check      = tcpChecksum(ip, tcp, datalen);
    printPacketInfo(ip, tcp);
}

// Build a TCP/IP FIN packet with the given
// sequence numbers, everything else as standard, valid TCP
void buildTcpFin(struct sockaddr_in *src, struct sockaddr_in *dst,
            struct iphdr *ip, struct tcphdr *tcp,
            uint32_t seq_local, uint32_t seq_remote) 
{
    // IP packet with TCP and no payload
    int datalen = 0;
    buildIPHeader(ip, src->sin_addr.s_addr, dst->sin_addr.s_addr, datalen);

    tcp->source     = src->sin_port;
    tcp->dest       = dst->sin_port;
    tcp->seq        = htonl(seq_local);
    tcp->ack_seq    = htonl(seq_remote);
    tcp->doff       = 5;    // Data offset 5 octets (no options)
    tcp->ack        = 1;
    tcp->psh        = 0;
    tcp->rst        = 0;
    tcp->urg        = 0;
    tcp->syn        = 0;
    tcp->fin        = 1;
    tcp->window     = htons(TCPWINDOW);
    tcp->check      = 0;
    tcp->check      = tcpChecksum(ip, tcp, datalen);
    printPacketInfo(ip, tcp);
}

// Build a TCP/IP data packet with the given
// sequence numbers, reserved field value and data
//
// param src        Source address
// param dst        Destination address
// param ip         IP header
// param tcp        TCP header
// param seq_local  local sequence number
// param seq_remote remote sequence number
// param reserved   reserved field value
// param data       byte array of data
// param datalen    length of data to be sent
void buildTcpData(struct sockaddr_in *src, struct sockaddr_in *dst,
            struct iphdr *ip, struct tcphdr *tcp,
            uint32_t seq_local, uint32_t seq_remote,
            uint8_t reserved,
            char data[], int datalen)
{
    char *dataStart = (char*) ip + IPHDRLEN + TCPHDRLEN;
    memcpy(dataStart, data, datalen);
    buildIPHeader(ip, src->sin_addr.s_addr, dst->sin_addr.s_addr, datalen);

    tcp->source     = src->sin_port;
    tcp->dest       = dst->sin_port;
    tcp->seq        = htonl(seq_local);
    tcp->ack_seq    = htonl(seq_remote);
    tcp->res1       = reserved & 0xF;
    tcp->doff       = 5;
    tcp->ack        = 1;
    tcp->psh        = (datalen > 0 ? 1 : 0);
    tcp->rst        = 0;
    tcp->urg        = 0;
    tcp->syn        = 0;
    tcp->fin        = 0;
    tcp->window     = htons(TCPWINDOW);
    tcp->check      = 0;
    tcp->check      = tcpChecksum(ip, tcp, datalen);

    printPacketInfo(ip, tcp);
    printBufferHex((char*)ip, IPHDRLEN + TCPHDRLEN + datalen);   
}

// TCP handshake function, parametrised with a bunch of values for our testsuite
// 
// param src        source address
// param dst        destination address
// param socket     RAW socket
// param ip         IP header (for reading and writing)
// param tcp        TCP header (for reading and writing)
// param buffer     the whole of the read/write buffer for headers and data
// param seq_local  local sequence number (reference, used for returning the negotiated number)
// param seq_remote remoe sequence number (reference, used for returning the negotiated number)
// param syn_ack    SYN packet ACK value to be sent
// param syn_urg    SYN packet URG pointer to be sent
// param syn_res    SYN packet reserved field value to be sent
// param synack_urg expected SYNACK packet URG pointer value
// param synack_check expected SYNACK packet checksum value (after undoing NATting recalculation)
// param synack_res expected SYNACK packet reserved field value
// return           success if handshake has been successful with all received values matching expected ones,
//                  error code otherwise
test_error handshake(struct sockaddr_in *src, struct sockaddr_in *dst,
                int socket, struct iphdr *ip, struct tcphdr *tcp, char buffer[],
                uint32_t &seq_local, uint32_t &seq_remote,
                uint32_t syn_ack, uint16_t syn_urg, uint8_t syn_res,
                uint16_t synack_urg, uint16_t synack_check, uint8_t synack_res)
{
    test_error ret;
    seq_local = 0;
    seq_remote = 0;
    buildTcpSyn(src, dst, ip, tcp, syn_ack, syn_urg, syn_res);
    if (!sendPacket(socket, buffer, dst, ntohs(ip->tot_len))) {
        LOGE("TCP SYN packet failure: %s", strerror(errno));
        return send_error;
    }
    seq_local = ntohl(tcp->seq) + 1;

    // Receive and verify that incoming packet source is our destination and vice-versa
    ret = receiveTcpSynAck(seq_local, socket, ip, tcp, dst, src, synack_urg, synack_check, synack_res);
    seq_remote = ntohl(tcp->seq) + 1;
    if (ret != success) {
        LOGE("TCP SYNACK packet failure: %d, %s", ret, strerror(errno));
        return ret;
    }
    LOGD("SYNACK \tSeq: %zu \tAck: %zu\n", ntohl(tcp->seq), ntohl(tcp->ack_seq));
    
    buildTcpAck(src, dst, ip, tcp, seq_local, seq_remote);
    if (!sendPacket(socket, buffer, dst, ntohs(ip->tot_len))) {
        LOGE("TCP handshake ACK failure: %s", strerror(errno));
        return send_error;
    }
    return success;
}

// Cleanly shutdown the connection with the
// FIN  -> 
//      <- FINACK / FIN
// ACK  ->
//      <- ACK if only FIN previously
test_error shutdownConnection(struct sockaddr_in *src, struct sockaddr_in *dst,
                int socket, struct iphdr *ip, struct tcphdr *tcp, char buffer[],
                uint32_t &seq_local, uint32_t &seq_remote)
{
    test_error ret;
    buildTcpFin(src, dst, ip, tcp, seq_local, seq_remote);
    if (!sendPacket(socket, buffer, dst, ntohs(ip->tot_len)))
        return send_error;

    int len = receivePacket(socket, ip, tcp, dst, src);
    bool finack_received = false;
    if (len > 0) {
        if (tcp->fin && tcp->ack) {
            finack_received = true;
            seq_remote = ntohl(tcp->ack_seq) + 1;
        } else if (!tcp->fin) {
            // Must be a packet with FIN flag set
            return protocol_error;
        }
    } else {
        return receive_error;
    }

    buildTcpAck(src, dst, ip, tcp, seq_local, seq_remote);
    if (!sendPacket(socket, buffer, dst, ntohs(ip->tot_len)))
        return send_error;

    if (!finack_received) {
        int len = receivePacket(socket, ip, tcp, dst, src);
        if (len < 0) {
            LOGE("TCP FINACK ACK not received, %d", ret);
        }
        else {
            LOGE("TCP connection closed");
            return success;
        }
    } else {
        return success;
    }
    return success;
}

// Setup RAW socket
// setsockopt calls for:
//      - allowing to manipulate full packet down to IP layer (IPPROTO_IP, IP_HDRINCL)
//      - timeout on recv'ing packets (SOL_SOCKET, SO_RCVTIMEO)
// param sock       socket as reference
test_error setupSocket(int &sock) {
    sock = socket(AF_INET, SOCK_RAW, IPPROTO_TCP);
    if (sock == -1) {
        LOGD("socket() failed");
        return test_failed;
    } else {
        LOGD("socket() ok");
    }

    int on = 1;
    if (setsockopt(sock, IPPROTO_IP, IP_HDRINCL, &on, sizeof(on)) == -1) {
        LOGE("setsockopt() failed: %s", strerror(errno));
        return test_failed;
    } else {
        LOGD("setsockopt() ok");
    }

    struct timeval tv;
    tv.tv_sec = sock_receive_timeout_sec.count();
    tv.tv_usec = 0;  // Not init'ing this can cause strange errors
    if ( setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char *)&tv, sizeof(struct timeval)) == -1 ) {
        LOGE("setsockopt receive timeout failed: %s", strerror(errno));
    }

    return success;
}

// Generic function for running any test. Takes all parameters and runs the rest of the functions:
//      1. Sets up a new socket
//      2. Performs the parametrised three-way handshake
//      3. Sends a piece of data if the connection has been opened successfully
//      4. Expects a specific data response back (returns error code if the result doesn't match)
//      5. ACKs the received data
//      6. Cleanly shuts down the connection
//
// return   test_failed or test_complete codes depending on the outcome
test_error runTest(uint32_t source, uint16_t src_port, uint32_t destination, uint16_t dst_port,
            uint32_t syn_ack, uint16_t syn_urg, uint8_t syn_res,
            uint16_t synack_urg, uint16_t synack_check, uint8_t synack_res,
            uint8_t data_out_res, uint8_t data_in_res,
            char *send_payload, int send_length, char *expect_payload, int expect_length)
{
    int sock;
    char buffer[BUFLEN] = {0};
    struct iphdr *ip;
    struct tcphdr *tcp;
    struct sockaddr_in src, dst;
    uint32_t seq_local, seq_remote;
    ip = (struct iphdr*) buffer;
    tcp = (struct tcphdr*) (buffer + IPHDRLEN);
    char *data = buffer + IPHDRLEN + TCPHDRLEN;

    if (setupSocket(sock) != success) {
        LOGE("Socket setup failed: %s", strerror(errno));
        return test_failed;
    }

    src.sin_family = AF_INET;
    src.sin_port = htons(src_port);
    src.sin_addr.s_addr = htonl(source);
    dst.sin_family = AF_INET;
    dst.sin_port = htons(dst_port);
    dst.sin_addr.s_addr = htonl(destination);

    if (handshake(&src, &dst, sock, ip, tcp, buffer, seq_local, seq_remote, 
        syn_ack, syn_urg, syn_res, 
        synack_urg, synack_check, synack_res) != success)
    {
        LOGE("TCP handshake failed: %s", strerror(errno));
        return test_failed;
    }

    memset(buffer, 0, BUFLEN);
    buildTcpData(&src, &dst, ip, tcp, seq_local, seq_remote, data_out_res, send_payload, send_length);
    test_error ret = success;
    if (sendPacket(sock, buffer, &dst, ntohs(ip->tot_len))) {
        int receiveLength = receivePacket(sock, ip, tcp, &dst, &src);

        if (memcmp(data, expect_payload, expect_length) != 0) {
            LOGE("Payload wrong value, expected for iplen %d, tcplen %d:", IPHDRLEN, TCPHDRLEN);
            printBufferHex(data, expect_length);
            printBufferHex(expect_payload, expect_length);
            ret = test_failed;
        }

        if (tcp->res1 != (data_in_res & 0xF)) {
            LOGE("Data packet reserved field wrong value: %02X, expected %02X", tcp->res1, data_in_res & 0xF);
            ret = test_failed;
        }

        int dataLength = receiveLength - IPHDRLEN - TCPHDRLEN;
        // TODO: handle the new sequence numbers
        seq_local = ntohl(tcp->ack_seq);
        if (dataLength > 0) {
            seq_remote = seq_remote + dataLength;
            buildTcpAck(&src, &dst, ip, tcp, seq_local, seq_remote);
            sendPacket(sock, buffer, &dst, ntohs(ip->tot_len));
            // if (ret != success) {
            //     LOGE("TCP Data ACK failure: %s", strerror(errno));
            // }
        }
    }

    shutdownConnection(&src, &dst, sock, ip, tcp, buffer, seq_local, seq_remote);

    if (ret != success)
        return test_failed;
    else
        return test_complete;
}

test_error runTest_ack_only(uint32_t source, uint16_t src_port, uint32_t destination, uint16_t dst_port) {
    uint32_t syn_ack = 0xbeef0001;
    uint16_t syn_urg = 0;
    uint8_t syn_res = 0;
    uint16_t synack_urg = 0;
    uint16_t synack_check = 0;
    uint8_t synack_res = 0;
    uint8_t data_out_res = 0;
    uint8_t data_in_res = 0;
    
    char send_payload[] = "HELLO";
    int send_length = strlen(send_payload);
    int expect_length = 4;
    char expect_payload[expect_length];
    expect_payload[0] = (syn_ack >> 8*3) & 0xFF;
    expect_payload[1] = (syn_ack >> 8*2) & 0xFF;
    expect_payload[2] = (syn_ack >> 8*1) & 0xFF;
    expect_payload[3] = syn_ack & 0xFF;
    
    return runTest(source, src_port, destination, dst_port,
        syn_ack, syn_urg, syn_res, synack_urg, synack_check, synack_res,
        data_out_res, data_in_res,
        send_payload, send_length, expect_payload, expect_length);
}

test_error runTest_urg_only(u_int32_t source, u_int16_t src_port, u_int32_t destination, u_int16_t dst_port)
{
    uint32_t syn_ack = 0;
    uint16_t syn_urg = 0xbe02;
    uint8_t syn_res = 0;
    uint16_t synack_urg = 0;
    uint16_t synack_check = 0;
    uint8_t synack_res = 0;
    uint8_t data_out_res = 0;
    uint8_t data_in_res = 0;
    
    char send_payload[] = "HELLO";
    int send_length = strlen(send_payload);
    int expect_length = 2;
    char expect_payload[expect_length];
    expect_payload[0] = (syn_urg >> 8) & 0xFF;
    expect_payload[1] = syn_urg & 0xFF;
    
    return runTest(source, src_port, destination, dst_port,
        syn_ack, syn_urg, syn_res, synack_urg, synack_check, synack_res,
        data_out_res, data_in_res,
        send_payload, send_length, expect_payload, expect_length);
}

test_error runTest_ack_urg(u_int32_t source, u_int16_t src_port, u_int32_t destination, u_int16_t dst_port)
{
    uint32_t syn_ack = 0xbeef0003;
    uint16_t syn_urg = 0;
    uint8_t syn_res = 0;
    uint16_t synack_urg = 0xbe03;
    uint16_t synack_check = 0;
    uint8_t synack_res = 0;
    uint8_t data_out_res = 0;
    uint8_t data_in_res = 0;
    
    char send_payload[] = "HELLO";
    int send_length = strlen(send_payload);
    char expect_payload[] = "OLLEH";
    int expect_length = strlen(expect_payload);
    
    return runTest(source, src_port, destination, dst_port,
        syn_ack, syn_urg, syn_res, synack_urg, synack_check, synack_res,
        data_out_res, data_in_res,
        send_payload, send_length, expect_payload, expect_length);
}

test_error runTest_plain_urg(u_int32_t source, u_int16_t src_port, u_int32_t destination, u_int16_t dst_port)
{
    uint32_t syn_ack = 0;
    uint16_t syn_urg = 0;
    uint8_t syn_res = 0;
    uint16_t synack_urg = 0xbe04;
    uint16_t synack_check = 0;
    uint8_t synack_res = 0;
    uint8_t data_out_res = 0;
    uint8_t data_in_res = 0;
    
    char send_payload[] = "HELLO";
    int send_length = strlen(send_payload);
    char expect_payload[] = "OLLEH";
    int expect_length = strlen(expect_payload);
    
    return runTest(source, src_port, destination, dst_port,
        syn_ack, syn_urg, syn_res, synack_urg, synack_check, synack_res,
        data_out_res, data_in_res,
        send_payload, send_length, expect_payload, expect_length);
}

test_error runTest_ack_checksum_incorrect(u_int32_t source, u_int16_t src_port, u_int32_t destination, u_int16_t dst_port)
{
    uint32_t syn_ack = 0xbeef0005;
    uint16_t syn_urg = 0;
    uint8_t syn_res = 0;
    uint16_t synack_urg = 0;
    uint16_t synack_check = 0xbeef;
    uint8_t synack_res = 0;
    uint8_t data_out_res = 0;
    uint8_t data_in_res = 0;
    
    char send_payload[] = "HELLO";
    int send_length = strlen(send_payload);
    char expect_payload[] = "OLLEH";
    int expect_length = strlen(expect_payload);
    
    return runTest(source, src_port, destination, dst_port,
        syn_ack, syn_urg, syn_res, synack_urg, synack_check, synack_res,
        data_out_res, data_in_res,
        send_payload, send_length, expect_payload, expect_length);
}

test_error runTest_ack_checksum(u_int32_t source, u_int16_t src_port, u_int32_t destination, u_int16_t dst_port)
{
    uint32_t syn_ack = 0xbeef0006;
    uint16_t syn_urg = 0;
    uint8_t syn_res = 0;
    uint16_t synack_urg = 0;
    uint16_t synack_check = 0xbeef;
    uint8_t synack_res = 0;
    uint8_t data_out_res = 0;
    uint8_t data_in_res = 0;
    
    char send_payload[] = "HELLO";
    int send_length = strlen(send_payload);
    char expect_payload[] = "OLLEH";
    int expect_length = strlen(expect_payload);
    
    return runTest(source, src_port, destination, dst_port,
        syn_ack, syn_urg, syn_res, synack_urg, synack_check, synack_res,
        data_out_res, data_in_res,
        send_payload, send_length, expect_payload, expect_length);
}

test_error runTest_urg_urg(u_int32_t source, u_int16_t src_port, u_int32_t destination, u_int16_t dst_port)
{
    uint32_t syn_ack = 0;
    uint16_t syn_urg = 0xbe07;
    uint8_t syn_res = 0;
    uint16_t synack_urg = 0xbe07;
    uint16_t synack_check = 0;
    uint8_t synack_res = 0;
    uint8_t data_out_res = 0;
    uint8_t data_in_res = 0;
    
    char send_payload[] = "HELLO";
    int send_length = strlen(send_payload);
    char expect_payload[] = "OLLEH";
    int expect_length = strlen(expect_payload);
    
    return runTest(source, src_port, destination, dst_port,
        syn_ack, syn_urg, syn_res, synack_urg, synack_check, synack_res,
        data_out_res, data_in_res,
        send_payload, send_length, expect_payload, expect_length);
}

test_error runTest_urg_checksum(u_int32_t source, u_int16_t src_port, u_int32_t destination, u_int16_t dst_port)
{
    uint32_t syn_ack = 0;
    uint16_t syn_urg = 0xbe08;
    uint8_t syn_res = 0;
    uint16_t synack_urg = 0;
    uint16_t synack_check = 0xbeef;
    uint8_t synack_res = 0;
    uint8_t data_out_res = 0;
    uint8_t data_in_res = 0;
    
    char send_payload[] = "HELLO";
    int send_length = strlen(send_payload);
    char expect_payload[] = "OLLEH";
    int expect_length = strlen(expect_payload);
    
    return runTest(source, src_port, destination, dst_port,
        syn_ack, syn_urg, syn_res, synack_urg, synack_check, synack_res,
        data_out_res, data_in_res,
        send_payload, send_length, expect_payload, expect_length);
}

test_error runTest_urg_checksum_incorrect(u_int32_t source, u_int16_t src_port, u_int32_t destination, u_int16_t dst_port)
{
    uint32_t syn_ack = 0;
    uint16_t syn_urg = 0xbe09;
    uint8_t syn_res = 0;
    uint16_t synack_urg = 0;
    uint16_t synack_check = 0xbeef;
    uint8_t synack_res = 0;
    uint8_t data_out_res = 0;
    uint8_t data_in_res = 0;
    
    char send_payload[] = "HELLO";
    int send_length = strlen(send_payload);
    char expect_payload[] = "OLLEH";
    int expect_length = strlen(expect_payload);
    
    return runTest(source, src_port, destination, dst_port,
        syn_ack, syn_urg, syn_res, synack_urg, synack_check, synack_res,
        data_out_res, data_in_res,
        send_payload, send_length, expect_payload, expect_length);
}

test_error runTest_reserved_syn(u_int32_t source, u_int16_t src_port, u_int32_t destination, u_int16_t dst_port)
{
    uint32_t syn_ack = 0;
    uint16_t syn_urg = 0;
    uint16_t synack_urg = 0;
    uint16_t synack_check = 0;
    uint8_t data_out_res = 0;
    uint8_t data_in_res = 0;

    char send_payload[] = "HELLO";
    int send_length = strlen(send_payload);
    char expect_payload[] = "OLLEH";
    int expect_length = strlen(expect_payload);
    
    uint8_t syn_res = 0b0001;
    uint8_t synack_res = 0b0001;
    test_error res1 = runTest(source, src_port, destination, dst_port,
        syn_ack, syn_urg, syn_res, synack_urg, synack_check, synack_res,
        data_out_res, data_in_res,
        send_payload, send_length, expect_payload, expect_length);
    if (res1 == test_complete)
        LOGD("Reserved byte 0b0001 passed");

    syn_res = 0b0010;
    synack_res = 0b0010;
    test_error res2 = runTest(source, src_port, destination, dst_port,
        syn_ack, syn_urg, syn_res, synack_urg, synack_check, synack_res,
        data_out_res, data_in_res,
        send_payload, send_length, expect_payload, expect_length);
    if (res2 == test_complete)
        LOGD("Reserved byte 0b0010 passed");

    syn_res = 0b0100;
    synack_res = 0b0100;
    test_error res3 = runTest(source, src_port, destination, dst_port,
        syn_ack, syn_urg, syn_res, synack_urg, synack_check, synack_res,
        data_out_res, data_in_res,
        send_payload, send_length, expect_payload, expect_length);
    if (res3 == test_complete)
        LOGD("Reserved byte 0b0100 passed");

    syn_res = 0b1000;
    synack_res = 0b1000;
    test_error res4 = runTest(source, src_port, destination, dst_port,
        syn_ack, syn_urg, syn_res, synack_urg, synack_check, synack_res,
        data_out_res, data_in_res,
        send_payload, send_length, expect_payload, expect_length);
    if (res1 == test_complete)
        LOGD("Reserved byte 0b1000 passed");

    if (res1 == test_complete && res2 == test_complete && res3 == test_complete && res4 == test_complete) {
        return test_complete;
    } else {
        return test_failed;
    }
}

test_error runTest_reserved_est(u_int32_t source, u_int16_t src_port, u_int32_t destination, u_int16_t dst_port)
{
    uint32_t syn_ack = 0;
    uint16_t syn_urg = 0;
    uint16_t synack_urg = 0;
    uint16_t synack_check = 0;
    uint8_t syn_res = 0;
    uint8_t synack_res = 0;
    
    char send_payload[] = "HELLO";
    int send_length = strlen(send_payload);
    char expect_payload[] = "OLLEH";
    int expect_length = strlen(expect_payload);

    uint8_t data_out_res = 0b0001;
    uint8_t data_in_res = 0b0001;
    test_error res1 = runTest(source, src_port, destination, dst_port,
        syn_ack, syn_urg, syn_res, synack_urg, synack_check, synack_res,
        data_out_res, data_in_res,
        send_payload, send_length, expect_payload, expect_length);
    if (res1 == test_complete)
        LOGD("Reserved byte 0b0001 passed");

    data_out_res = 0b0010;
    data_in_res = 0b0010;
    test_error res2 = runTest(source, src_port, destination, dst_port,
        syn_ack, syn_urg, syn_res, synack_urg, synack_check, synack_res,
        data_out_res, data_in_res,
        send_payload, send_length, expect_payload, expect_length);
    if (res2 == test_complete)
        LOGD("Reserved byte 0b0010 passed");

    data_out_res = 0b0100;
    data_in_res = 0b0100;
    test_error res3 = runTest(source, src_port, destination, dst_port,
        syn_ack, syn_urg, syn_res, synack_urg, synack_check, synack_res,
        data_out_res, data_in_res,
        send_payload, send_length, expect_payload, expect_length);
    if (res3 == test_complete)
        LOGD("Reserved byte 0b0100 passed");

    data_out_res = 0b1000;
    data_in_res = 0b1000;
    test_error res4 = runTest(source, src_port, destination, dst_port,
        syn_ack, syn_urg, syn_res, synack_urg, synack_check, synack_res,
        data_out_res, data_in_res,
        send_payload, send_length, expect_payload, expect_length);
    if (res1 == test_complete)
        LOGD("Reserved byte 0b1000 passed");

    if (res1 == test_complete && res2 == test_complete && res3 == test_complete && res4 == test_complete) {
        return test_complete;
    } else {
        return test_failed;
    }
}
