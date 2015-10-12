#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <libmnl/libmnl.h>
#include <linux/rtnetlink.h>
#include <linux/if_addr.h>

#include "neat.h"
#include "neat_core.h"
#include "neat_addr.h"
#include "neat_linux.h"
#include "neat_linux_internal.h"

static ssize_t neat_linux_request_addrs(struct mnl_socket *mnl_sock)
{
    uint8_t snd_buf[MNL_SOCKET_BUFFER_SIZE];
    struct nlmsghdr *nl_hdr = mnl_nlmsg_put_header(snd_buf);

    nl_hdr->nlmsg_type = RTM_GETADDR;
    nl_hdr->nlmsg_pid = getpid();
    nl_hdr->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;

    struct rtgenmsg* rt_msg = (struct rtgenmsg*)
        mnl_nlmsg_put_extra_header(nl_hdr, sizeof(struct rtgenmsg));
    rt_msg->rtgen_family = AF_UNSPEC;

    return mnl_socket_sendto(mnl_sock, snd_buf, nl_hdr->nlmsg_len);
}

static int neat_linux_parse_nlattr(const struct nlattr *attr, void *data)
{
    struct nlattr_storage *storage = (struct nlattr_storage*) data;
    int32_t type = mnl_attr_get_type(attr);

    if (mnl_attr_type_valid(attr, storage->limit) < 0)
        return MNL_CB_OK;

    storage->tb[type] = attr;
    return MNL_CB_OK;
}
static void neat_linux_handle_addr(struct neat_ctx *nc,
                                   struct nlmsghdr *nl_hdr)
{
    struct ifaddrmsg *ifm = (struct ifaddrmsg*) mnl_nlmsg_get_payload(nl_hdr);
    const struct nlattr *attr_table[IFA_MAX+1];
    struct nlattr_storage tb_storage = {attr_table, IFA_MAX+1};
    struct sockaddr_storage src_addr;
    struct sockaddr_in *src_addr4;
    struct sockaddr_in6 *src_addr6;
    struct ifa_cacheinfo *ci;
    uint32_t *addr6_ptr, ifa_pref = 0, ifa_valid = 0;
    uint8_t i;

    if (ifm->ifa_index == LO_DEV_IDX)
        return;

    memset(attr_table, 0, sizeof(attr_table));
    memset(&src_addr, 0, sizeof(src_addr));

    if (mnl_attr_parse(nl_hdr, sizeof(struct ifaddrmsg),
                neat_linux_parse_nlattr, &tb_storage) != MNL_CB_OK) {
        fprintf(stderr, "Failed to parse nlattr for msg of type %d\n",
                nl_hdr->nlmsg_type);
        return;
    }

    if (ifm->ifa_family == AF_INET) {
        src_addr4 = (struct sockaddr_in*) &src_addr;
        src_addr4->sin_family = AF_INET;
        src_addr4->sin_addr.s_addr = mnl_attr_get_u32(attr_table[IFA_LOCAL]);
    } else {
        src_addr6 = (struct sockaddr_in6*) &src_addr;
        src_addr6->sin6_family = AF_INET6;
        addr6_ptr = (uint32_t*) mnl_attr_get_payload(attr_table[IFA_ADDRESS]);

        for (i=0; i<4; i++)
            src_addr6->sin6_addr.s6_addr32[i] = *(addr6_ptr + i);

        //Ignore addresses with ULA prefix
        if (ifm->ifa_scope != RT_SCOPE_UNIVERSE &&
            (src_addr6->sin6_addr.s6_addr[0] & 0xfe) != 0xfc)
            return;

        ci = (struct ifa_cacheinfo*) mnl_attr_get_payload(attr_table[IFA_CACHEINFO]);
        ifa_pref = ci->ifa_prefered;
        ifa_valid = ci->ifa_valid;
    }

    //TODO: Should this function be a callback instead? Will we have multiple
    //addresses handlers/types of context?
    neat_addr_update_src_list(nc, &src_addr, ifm->ifa_index,
            nl_hdr->nlmsg_type == RTM_NEWADDR, ifa_pref, ifa_valid);
}

static void neat_linux_nl_alloc(uv_handle_t *handle, size_t suggested_size,
        uv_buf_t *buf)
{
    struct neat_ctx *nc = handle->data;
    memset(nc->mnl_rcv_buf, 0, sizeof(nc->mnl_rcv_buf)); 
    buf->base = nc->mnl_rcv_buf;
    buf->len = sizeof(nc->mnl_rcv_buf);
}

static void neat_linux_nl_recv(uv_udp_t *handle, ssize_t nread,
        const uv_buf_t *buf, const struct sockaddr *addr, unsigned int flags)
{
    struct neat_ctx *nc = (struct neat_ctx*) handle->data;
    struct nlmsghdr *nl_hdr = (struct nlmsghdr*) buf->base;
    //We don't need any check here, we don't read more than 8192 bytes in one go
    int numbytes = (int) nread;

    while (mnl_nlmsg_ok(nl_hdr, numbytes)) {
        if (nl_hdr->nlmsg_type == RTM_NEWADDR ||
            nl_hdr->nlmsg_type == RTM_DELADDR)
            neat_linux_handle_addr(nc, nl_hdr);  
       
        nl_hdr = mnl_nlmsg_next(nl_hdr, &numbytes);
    }
}

static void neat_linux_cleanup(struct neat_ctx *nc)
{
    if (nc->mnl_sock)
        mnl_socket_close(nc->mnl_sock);
}

uint8_t neat_linux_init_ctx(struct neat_ctx *nc)
{
    //Configure netlink and start requesting addresses
    if ((nc->mnl_sock = mnl_socket_open(NETLINK_ROUTE)) == NULL) {
        fprintf(stderr, "Failed to allocate netlink socket\n");
        return RETVAL_FAILURE;
    }

    if (mnl_socket_bind(nc->mnl_sock, (1 << (RTNLGRP_IPV4_IFADDR - 1)) |
                (1 << (RTNLGRP_IPV6_IFADDR - 1)), 0)) {
        fprintf(stderr, "Failed to bind netlink socket\n");
        return RETVAL_FAILURE;
    }
    
    //Send address request to get things started
    if (neat_linux_request_addrs(nc->mnl_sock) <= 0) {
        fprintf(stderr, "Failed to request addresses\n");
        return RETVAL_FAILURE;
    }

    //Add socket to event loop
    if (uv_udp_init(nc->loop, &(nc->uv_nl_handle))) {
        fprintf(stderr, "Failed to initialize uv UDP handle\n");
        return RETVAL_FAILURE;
    }

    //TODO: We could use offsetof, but libuv has a pointer so ...
    nc->uv_nl_handle.data = nc;

    if (uv_udp_open(&(nc->uv_nl_handle), mnl_socket_get_fd(nc->mnl_sock))) {
        fprintf(stderr, "Could not add netlink socket to uv\n");
        return RETVAL_FAILURE;
    }

    if (uv_udp_recv_start(&(nc->uv_nl_handle), neat_linux_nl_alloc,
                neat_linux_nl_recv)) {
        fprintf(stderr, "Could not start receiving netlink packets\n");
        return RETVAL_FAILURE;
    }
     
    nc->cleanup = neat_linux_cleanup;

    //Configure netlink socket, add to event loop and start dumping
    return RETVAL_SUCCESS;
}
