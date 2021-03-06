#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#include "neat.h"
#include "neat_internal.h"
#include "neat_property_helpers.h"

static void he_print_results(struct neat_resolver_results *results)
{
    struct neat_resolver_res *result;
    char addr_name[INET6_ADDRSTRLEN];
    char serv_name[6];

    fprintf(stderr, "Results:\n");
    LIST_FOREACH(result, results, next_res) {
        switch (result->ai_protocol) {
        case IPPROTO_UDP:
            fprintf(stderr, "UDP/");
            break;
        case IPPROTO_TCP:
            fprintf(stderr, "TCP/");
            break;
#ifdef IPPROTO_SCTP
        case IPPROTO_SCTP:
            fprintf(stderr, "SCTP/");
            break;
#endif
#ifdef IPPROTO_UDPLITE
        case IPPROTO_UDPLITE:
            fprintf(stderr, "UDPLITE/");
            break;
#endif
        default:
            fprintf(stderr, "proto%d/", result->ai_protocol);
            break;
        }
        switch (result->ai_family) {
        case AF_INET:
            fprintf(stderr, "IPv4");
            break;
        case AF_INET6:
            fprintf(stderr, "IPv6");
            break;
        default:
            fprintf(stderr, "family%d", result->ai_family);
            break;
        }
        getnameinfo((struct sockaddr *)&result->src_addr, result->src_addr_len,
                    addr_name, sizeof(addr_name),
                    serv_name, sizeof(serv_name),
                    NI_NUMERICHOST | NI_NUMERICSERV);
        fprintf(stderr, ": %s:%s->", addr_name, serv_name);
        getnameinfo((struct sockaddr *)&result->dst_addr, result->dst_addr_len,
                    addr_name, sizeof(addr_name),
                    serv_name, sizeof(serv_name),
                    NI_NUMERICHOST | NI_NUMERICSERV);
        fprintf(stderr, "%s:%s\n", addr_name, serv_name);
    }
}

/* TODO: Used by Karl-Johan Grinnemo during test. Remove in final version. */
#if 0
static void
pm_filter(struct neat_resolver_results *results)
{
    struct neat_resolver_res *res_itr1 = results->lh_first;

    while (res_itr1 != NULL) {

        struct neat_resolver_res *tmp_itr1 = res_itr1;
        res_itr1 = res_itr1->next_res.le_next;
        if (((tmp_itr1->ai_protocol != IPPROTO_TCP) &&
            (tmp_itr1->ai_protocol != IPPROTO_SCTP)) ||
            (tmp_itr1->ai_family != AF_INET)) {

            LIST_REMOVE(tmp_itr1, next_res);
            free(tmp_itr1);

        } else {

            struct neat_resolver_res *res_itr2 = results->lh_first;
            while (res_itr2 != tmp_itr1) {
                struct neat_resolver_res *tmp_itr2 = res_itr2;
                res_itr2 = res_itr2->next_res.le_next;
                if ((tmp_itr1->ai_protocol == tmp_itr2->ai_protocol) &&
                    (memcmp(&tmp_itr1->dst_addr,
                            &tmp_itr2->dst_addr,
                            sizeof(struct sockaddr_storage)) == 0)) {

                    LIST_REMOVE(tmp_itr1, next_res);
                    free(tmp_itr1);
                    break;

                }

            }

        }

    }
}
#endif

static void
he_resolve_cb(struct neat_resolver *resolver, struct neat_resolver_results *results, uint8_t code)
{
    neat_flow *flow = (neat_flow *)resolver->userData1;
    uv_poll_cb callback_fx;
    callback_fx = (uv_poll_cb) (neat_flow *)resolver->userData2;

    assert (results->lh_first);
    assert (!flow->resolver_results);

    /* TODO: Used by Karl-Johan Grinnemo during test. Remove in final version. */
#if 0
    pm_filter(results);
    he_print_results(results);
#endif

    flow->resolver_results = results;
    flow->hefirstConnect = 1;
    struct neat_resolver_res *candidate;
    LIST_FOREACH(candidate, results, next_res) {
        //TODO: Potential place to filter based on policy
        struct he_cb_ctx *he_ctx = (struct he_cb_ctx *) malloc(sizeof(struct he_cb_ctx));
        assert(he_ctx !=NULL);
        he_ctx->handle = (uv_poll_t *) malloc(sizeof(uv_poll_t));
        assert(he_ctx->handle != NULL);
        he_ctx->handle->data = (void *)he_ctx;
        he_ctx->nc = resolver->nc;
        he_ctx->candidate = candidate;
        he_ctx->flow = flow;
        he_ctx->fd = -1;

        if (flow->connectfx(he_ctx, callback_fx) == -1) {
            /* TODO: Some error handling? */
            continue;
        }

    }

}

neat_error_code neat_he_lookup(neat_ctx *ctx, neat_flow *flow, uv_poll_cb callback_fx)
{
    int protocols[NEAT_MAX_NUM_PROTO]; /* We only support SCTP, TCP, UDP, and UDPLite */
    uint8_t nr_of_protocols;
    uint8_t family;

    if ((flow->propertyMask & NEAT_PROPERTY_IPV4_REQUIRED) &&
        (flow->propertyMask & NEAT_PROPERTY_IPV4_BANNED))
        return NEAT_ERROR_UNABLE;
    if ((flow->propertyMask & NEAT_PROPERTY_IPV6_REQUIRED) &&
        (flow->propertyMask & NEAT_PROPERTY_IPV6_BANNED))
        return NEAT_ERROR_UNABLE;
    if ((flow->propertyMask & NEAT_PROPERTY_IPV4_BANNED) &&
        (flow->propertyMask & NEAT_PROPERTY_IPV6_BANNED))
        return NEAT_ERROR_UNABLE;
    if ((flow->propertyMask & NEAT_PROPERTY_IPV4_REQUIRED) &&
        (flow->propertyMask & NEAT_PROPERTY_IPV6_BANNED))
        family = AF_INET;
    else if ((flow->propertyMask & NEAT_PROPERTY_IPV6_REQUIRED) &&
             (flow->propertyMask & NEAT_PROPERTY_IPV4_BANNED))
        family = AF_INET6;
    else
        family = AF_UNSPEC; /* AF_INET and AF_INET6 */

    nr_of_protocols = neat_property_translate_protocols(flow->propertyMask,
            protocols);
    if (nr_of_protocols == 0)
        return NEAT_ERROR_UNABLE;

    if (!ctx->resolver) {
        ctx->resolver = neat_resolver_init(ctx, he_resolve_cb, NULL);
    }
    ctx->resolver->userData1 = (void *)flow; // TODO: This doesn't allow multiple sockets
    ctx->resolver->userData2 = callback_fx;

    /* FIXME: derivation of the socket type is wrong.
     * FIXME: Make use of the array of protocols
     */
    neat_getaddrinfo(ctx->resolver, family, flow->name, flow->port,
            protocols, nr_of_protocols);

    return NEAT_OK;
}
