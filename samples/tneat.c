#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/time.h>
#include "../neat.h"
#include "../neat_internal.h"

/*
    tneat
    testing tool for neat
*/

/*
    default values
*/
static uint32_t config_rcv_buffer_size = 512;
static uint32_t config_snd_buffer_size = 1024;
static uint32_t config_message_count = 32;
static uint32_t config_runtime = 0;
static uint16_t config_active = 0;
static uint16_t config_chargen_offset = 0;
static uint16_t config_port = 8080;
static uint16_t config_log_level = 1;
static char config_property[] = "NEAT_PROPERTY_TCP_REQUIRED,NEAT_PROPERTY_IPV4_REQUIRED";

/*
    macro - error report
*/
#define debug_error(M, ...) fprintf(stderr, "[ERROR][%s:%d] " M "\n", __FUNCTION__, __LINE__, ##__VA_ARGS__)

/*
    macro - tvp-uvp=vvp
*/

#ifndef timersub
#define timersub(tvp, uvp, vvp)                                         \
        do {                                                            \
                (vvp)->tv_sec = (tvp)->tv_sec - (uvp)->tv_sec;          \
                (vvp)->tv_usec = (tvp)->tv_usec - (uvp)->tv_usec;       \
                if ((vvp)->tv_usec < 0) {                               \
                        (vvp)->tv_sec--;                                \
                        (vvp)->tv_usec += 1000000;                      \
                }                                                       \
        } while (0)
#endif

struct tneat_flow_direction {
    unsigned char *buffer;
    uint64_t calls;
    uint64_t bytes;
    struct timeval tv_first;
    struct timeval tv_last;
};

struct tneat_flow {
    struct tneat_flow_direction rcv;
    struct tneat_flow_direction snd;
};

static struct neat_flow_operations ops;

/*
    print usage and exit
*/
static void print_usage()
{
    printf("tneat [OPTIONS] [HOST]\n");
    printf("\t- l \tsize for each message in byte (%d)\n", config_snd_buffer_size);
    printf("\t- n \tmax number of messages to send (%d)\n", config_message_count);
    printf("\t- p \tport [receive on|send to] (%d)\n", config_port);
    printf("\t- P \tneat properties (%s)\n", config_property);
    printf("\t- R \treceive buffer in byte (%d)\n", config_rcv_buffer_size);
    printf("\t- T \tmax runtime in seconds (%d)\n", config_runtime);
    printf("\t- v \tlog level 0..3 (%d)\n", config_log_level);
}

/*
    print human readable file sizes - keep attention to provide enough buffer space
*/
static char *filesize_human(double bytes, char *buffer)
{
    uint8_t i = 0;
    const char* units[] = {"B", "kB", "MB", "GB", "TB", "PB", "EB", "ZB", "YB"};
    while (bytes > 1000) {
        bytes /= 1000;
        i++;
    }
    sprintf(buffer, "%.*f %s", i, bytes, units[i]);
    return buffer;
}

/*
    error handler
*/
static neat_error_code on_error(struct neat_flow_operations *opCB)
{
    exit(EXIT_FAILURE);
}

static neat_error_code
on_all_written(struct neat_flow_operations *opCB)
{
    struct tneat_flow *tnf = opCB->userData;
    struct timeval now, diff_time;
    double time_elapsed;
    char buffer_filesize_human[16];

    gettimeofday(&now, NULL);
    timersub(&now, &(tnf->snd.tv_first), &diff_time);
    time_elapsed = diff_time.tv_sec + (double)diff_time.tv_usec/1000000.0;

    printf("neat_write finished - statistics\n");
    printf("\tbytes\t\t: %" PRIu64 "\n", tnf->snd.bytes);
    printf("\tsnd-calls\t: %" PRIu64 "\n", tnf->snd.calls);
    printf("\tduration\t: %.2fs\n", time_elapsed);
    printf("\tbandwidth\t: %s/s\n", filesize_human(tnf->snd.bytes/time_elapsed, buffer_filesize_human));

    opCB->on_readable = NULL;
    free(tnf->snd.buffer);
    free(tnf->rcv.buffer);
    free(tnf);
    neat_stop_event_loop(opCB->ctx);
    return NEAT_OK;
}

/*
    send *config_message_size* chars to peer
*/
static neat_error_code on_writable(struct neat_flow_operations *opCB)
{
    struct tneat_flow *tnf = opCB->userData;
    neat_error_code code;
    struct timeval diff_time;
    double time_elapsed;
    int last_message;


    // record start time
    if (tnf->snd.calls == 0) {
        gettimeofday(&(tnf->snd.tv_first), NULL);
    }

    tnf->snd.calls++;
    tnf->snd.bytes += config_snd_buffer_size;
    gettimeofday(&(tnf->snd.tv_last), NULL);

    timersub(&(tnf->snd.tv_last), &(tnf->snd.tv_first), &diff_time);
    time_elapsed = diff_time.tv_sec + (double)diff_time.tv_usec/1000000.0;

    // stop writing if config_runtime reached or config_message_count msgs sent
    if ((config_runtime > 0 && time_elapsed >= config_runtime) ||
        (config_runtime == 0 && config_message_count > 0 && tnf->snd.calls >= config_message_count)) {
        last_message = 1;
        opCB->on_all_written = on_all_written;
    } else {
        last_message = 0;
    }

    if (last_message == 1) {
        opCB->on_all_written = on_all_written;
    }
    // every buffer is filled with chars - offset increased by every run
    if (config_log_level >= 2) {
        printf("neat_write - # %" PRIu64 " - %d byte\n", tnf->snd.calls, config_snd_buffer_size);
        if (config_log_level >= 3) {
            printf("neat_write - content\n");
            fwrite(tnf->snd.buffer, sizeof(char), config_snd_buffer_size, stdout);
            printf("\n");
        }
    }

    memset(tnf->snd.buffer, 33 + config_chargen_offset++, config_snd_buffer_size);
    config_chargen_offset = config_chargen_offset % 72;
    code = neat_write(opCB->ctx, opCB->flow, tnf->snd.buffer, config_snd_buffer_size);
    if (code != NEAT_OK) {
        debug_error("neat_write - code: %d", (int)code);
        return on_error(opCB);
    }

    if (last_message == 1) {
        opCB->on_writable = NULL;
    }

    return NEAT_OK;
}

static neat_error_code on_readable(struct neat_flow_operations *opCB)
{
    struct tneat_flow *tnf = opCB->userData;
    uint32_t buffer_filled;
    struct timeval diff_time;
    neat_error_code code;
    char buffer_filesize_human[16];
    double time_elapsed;

    code = neat_read(opCB->ctx, opCB->flow, tnf->rcv.buffer, config_rcv_buffer_size, &buffer_filled);
    if (code) {
        if (code == NEAT_ERROR_WOULD_BLOCK) {
            debug_error("NEAT_ERROR_WOULD_BLOCK");
            return NEAT_OK;
        } else {
            debug_error("neat_read - code: %d", (int)code);
            return on_error(opCB);
        }
    }

    if (buffer_filled > 0) {
        // we got data!
        if (tnf->rcv.calls == 0) {
            gettimeofday(&(tnf->rcv.tv_first), NULL);
        }
        tnf->rcv.calls++;
        tnf->rcv.bytes += buffer_filled;
        gettimeofday(&(tnf->rcv.tv_last), NULL);

        if (config_log_level >= 2) {
            printf("neat_read - # %" PRIu64 " - %d byte\n", tnf->rcv.calls, buffer_filled);
            if (config_log_level >= 3) {
                fwrite(tnf->rcv.buffer, sizeof(char), buffer_filled, stdout);
                printf("\n");
            }
        }
    } else {
        // client disconnected - print statistics
        timersub(&(tnf->rcv.tv_last), &(tnf->rcv.tv_first), &diff_time);
        time_elapsed = diff_time.tv_sec + (double)diff_time.tv_usec/1000000.0;

        printf("%" PRIu64 ", %" PRIu64 ", %.2f, %.2f, %s\n", tnf->rcv.bytes, tnf->rcv.calls, time_elapsed, tnf->rcv.bytes/time_elapsed, filesize_human(tnf->rcv.bytes/time_elapsed, buffer_filesize_human));

        if (config_log_level >= 1) {
            printf("client disconnected - statistics\n");
            printf("\tbytes\t\t: %" PRIu64 "\n", tnf->rcv.bytes);
            printf("\trcv-calls\t: %" PRIu64 "\n", tnf->rcv.calls);
            printf("\tduration\t: %.2fs\n", time_elapsed);
            printf("\tbandwidth\t: %s/s\n", filesize_human(tnf->rcv.bytes/time_elapsed, buffer_filesize_human));
        }

        opCB->on_readable = NULL;
        free(tnf->snd.buffer);
        free(tnf->rcv.buffer);
        free(tnf);
        neat_free_flow(opCB->flow);
    }

    return NEAT_OK;
}

/*
    Connection established - set callbacks and reset statistics
*/
static neat_error_code on_connected(struct neat_flow_operations *opCB)
{
    struct tneat_flow *tnf = NULL;

    // print ip-family and protocol
    if (config_log_level >= 1) {
        printf("[%d] connected - ", opCB->flow->fd);

        if (opCB->flow->family == AF_INET) {
            printf("IPv4 - ");
        } else if (opCB->flow->family == AF_INET6) {
            printf("IPv6 - ");
        }

        switch (opCB->flow->sockProtocol) {
        case IPPROTO_TCP:
            printf("TCP ");
            break;
        case IPPROTO_UDP:
            printf("UDP ");
            break;
    #ifdef IPPROTO_SCTP
        case IPPROTO_SCTP:
            printf("SCTP ");
            break;
    #endif
    #ifdef IPPROTO_UDPLITE
        case IPPROTO_UDPLITE:
            printf("UDPLite ");
            break;
    #endif
        default:
            printf("protocol #%d", opCB->flow->sockProtocol);
            break;
        }
        printf("\n");
    }

    if ((opCB->userData = calloc(1, sizeof(struct tneat_flow))) == NULL) {
        debug_error("could not allocate tneat_flow");
        exit(EXIT_FAILURE);
    }

    tnf = opCB->userData;

    if ((tnf->snd.buffer = malloc(config_snd_buffer_size)) == NULL) {
        debug_error("could not allocate send buffer");
        exit(EXIT_FAILURE);
    }

    if ((tnf->rcv.buffer = malloc(config_rcv_buffer_size)) == NULL) {
        debug_error("could not allocate receive buffer");
        exit(EXIT_FAILURE);
    }

    // reset stats
    tnf->snd.calls = 0;
    tnf->snd.bytes = 0;
    tnf->rcv.calls = 0;
    tnf->rcv.bytes = 0;

    // set callbacks
    opCB->on_readable = on_readable;
    if (config_active) {
        opCB->on_writable = on_writable;
    }

    return NEAT_OK;
}

int main(int argc, char *argv[])
{
    struct neat_ctx *ctx = NULL;
    struct neat_flow *flow = NULL;
    uint64_t prop;
    int arg, result;
    char *arg_property = config_property;
    char *arg_property_ptr;
    char arg_property_delimiter[] = ",;";

    result = EXIT_SUCCESS;

    while ((arg = getopt(argc, argv, "l:n:p:P:R:T:v:")) != -1) {
        switch(arg) {
        case 'l':
            config_snd_buffer_size = atoi(optarg);
            if (config_log_level >= 1) {
                printf("option - send buffer size: %d\n", config_snd_buffer_size);
            }
            break;
        case 'n':
            config_message_count = atoi(optarg);
            if (config_log_level >= 1) {
                printf("option - message limit: %d\n", config_message_count);
            }
            break;
        case 'p':
            config_port = atoi(optarg);
            if (config_log_level >= 1) {
                printf("option - port: %d\n", config_port);
            }
            break;
        case 'P':
            arg_property = optarg;
            if (config_log_level >= 1) {
                printf("option - properties: %s\n", arg_property);
            }
            break;
        case 'R':
            config_rcv_buffer_size = atoi(optarg);
            if (config_log_level >= 1) {
                printf("option - receive buffer size: %d\n", config_rcv_buffer_size);
            }
            break;
        case 'T':
            config_runtime = atoi(optarg);
            if (config_log_level >= 1) {
                printf("option - runtime limit: %d\n", config_runtime);
            }
            break;
        case 'v':
            config_log_level = atoi(optarg);
            if (config_log_level >= 1) {
                printf("option - log level: %d\n", config_log_level);
            }
            break;
        default:
            print_usage();
            goto cleanup;
            break;
        }
    }

    if (optind == argc) {
        config_active = 0;
        if (config_log_level >= 1) {
            printf("role: passive\n");
        }
    } else if (optind + 1 == argc) {
        config_active = 1;
        if (config_log_level >= 1) {
            printf("role: active\n");
        }
    } else {
        debug_error("argument error");
        print_usage();
        goto cleanup;
    }

    if ((ctx = neat_init_ctx()) == NULL) {
        debug_error("could not initialize context");
        result = EXIT_FAILURE;
        goto cleanup;
    }

    // new neat flow
    if ((flow = neat_new_flow(ctx)) == NULL) {
        debug_error("neat_new_flow");
        result = EXIT_FAILURE;
        goto cleanup;
    }

    ops.on_connected = on_connected;
    ops.on_error = on_error;

    //ops.on_all_written = on_all_written;
    if (neat_set_operations(ctx, flow, &ops)) {
        debug_error("neat_set_operations");
        result = EXIT_FAILURE;
        goto cleanup;
    }

    // get properties
    if (neat_get_property(ctx, flow, &prop)) {
        debug_error("neat_get_property");
        result = EXIT_FAILURE;
        goto cleanup;
    }


    // read property arguments
    arg_property_ptr = strtok(arg_property, arg_property_delimiter);

    while (arg_property_ptr != NULL) {
        if (config_log_level >= 1) {
            printf("setting property: %s\n", arg_property_ptr);
        }

        if (strcmp(arg_property_ptr,"NEAT_PROPERTY_OPTIONAL_SECURITY") == 0) {
            prop |= NEAT_PROPERTY_TCP_REQUIRED;
        } else if (strcmp(arg_property_ptr,"NEAT_PROPERTY_REQUIRED_SECURITY") == 0) {
            prop |= NEAT_PROPERTY_REQUIRED_SECURITY;
        } else if (strcmp(arg_property_ptr,"NEAT_PROPERTY_MESSAGE") == 0) {
            prop |= NEAT_PROPERTY_MESSAGE;
        } else if (strcmp(arg_property_ptr,"NEAT_PROPERTY_IPV4_REQUIRED") == 0) {
            prop |= NEAT_PROPERTY_IPV4_REQUIRED;
        } else if (strcmp(arg_property_ptr,"NEAT_PROPERTY_IPV4_BANNED") == 0) {
            prop |= NEAT_PROPERTY_IPV4_BANNED;
        } else if (strcmp(arg_property_ptr,"NEAT_PROPERTY_IPV6_REQUIRED") == 0) {
            prop |= NEAT_PROPERTY_IPV6_REQUIRED;
        } else if (strcmp(arg_property_ptr,"NEAT_PROPERTY_IPV6_BANNED") == 0) {
            prop |= NEAT_PROPERTY_IPV6_BANNED;
        } else if (strcmp(arg_property_ptr,"NEAT_PROPERTY_SCTP_REQUIRED") == 0) {
            prop |= NEAT_PROPERTY_SCTP_REQUIRED;
        } else if (strcmp(arg_property_ptr,"NEAT_PROPERTY_SCTP_BANNED") == 0) {
            prop |= NEAT_PROPERTY_SCTP_BANNED;
        } else if (strcmp(arg_property_ptr,"NEAT_PROPERTY_TCP_REQUIRED") == 0) {
            prop |= NEAT_PROPERTY_TCP_REQUIRED;
        } else if (strcmp(arg_property_ptr,"NEAT_PROPERTY_TCP_BANNED") == 0) {
            prop |= NEAT_PROPERTY_TCP_BANNED;
        } else if (strcmp(arg_property_ptr,"NEAT_PROPERTY_UDP_REQUIRED") == 0) {
            prop |= NEAT_PROPERTY_UDP_REQUIRED;
        } else if (strcmp(arg_property_ptr,"NEAT_PROPERTY_UDP_BANNED") == 0) {
            prop |= NEAT_PROPERTY_UDP_BANNED;
        } else if (strcmp(arg_property_ptr,"NEAT_PROPERTY_UDPLITE_REQUIRED") == 0) {
            prop |= NEAT_PROPERTY_UDPLITE_REQUIRED;
        } else if (strcmp(arg_property_ptr,"NEAT_PROPERTY_UDPLITE_BANNED") == 0) {
            prop |= NEAT_PROPERTY_UDPLITE_BANNED;
        } else if (strcmp(arg_property_ptr,"NEAT_PROPERTY_CONGESTION_CONTROL_REQUIRED") == 0) {
            prop |= NEAT_PROPERTY_CONGESTION_CONTROL_REQUIRED;
        } else if (strcmp(arg_property_ptr,"NEAT_PROPERTY_CONGESTION_CONTROL_BANNED") == 0) {
            prop |= NEAT_PROPERTY_CONGESTION_CONTROL_BANNED;
        } else if (strcmp(arg_property_ptr,"NEAT_PROPERTY_RETRANSMISSIONS_REQUIRED") == 0) {
            prop |= NEAT_PROPERTY_RETRANSMISSIONS_REQUIRED;
        } else if (strcmp(arg_property_ptr,"NEAT_PROPERTY_RETRANSMISSIONS_BANNED") == 0) {
            prop |= NEAT_PROPERTY_RETRANSMISSIONS_BANNED;
        } else {
            printf("error - unknown property: %s\n", arg_property_ptr);
            print_usage();
            goto cleanup;
        }

       // get next property
       arg_property_ptr = strtok(NULL, arg_property_delimiter);
    }

    // set properties
    if (neat_set_property(ctx, flow, prop)) {
        debug_error("neat_set_property");
        result = EXIT_FAILURE;
        goto cleanup;
    }

    // workaround until port is notated in int..
    // XXX
    char port[10];
    sprintf(port, "%d", config_port);

    if (config_active) {
        // connect to peer
        if (neat_open(ctx, flow, argv[optind], port) == NEAT_OK) {
            if (config_log_level >= 1) {
                printf("neat_open - connecting to %s:%d\n", argv[optind], config_port);
            }
            neat_start_event_loop(ctx, NEAT_RUN_DEFAULT);
        } else {
            debug_error("neat_open");
            result = EXIT_FAILURE;
            goto cleanup;
        }
    } else {
        // wait for on_connected or on_error to be invoked
        if (neat_accept(ctx, flow, "*", port)) {
            debug_error("neat_accept - *:%d\n", config_port);
            result = EXIT_FAILURE;
            goto cleanup;
        }

        neat_start_event_loop(ctx, NEAT_RUN_DEFAULT);
    }


    if (config_log_level >= 1) {
        printf("freeing (flow + ctx) and bye bye!\n");
    }

    // cleanup
cleanup:
    if (flow != NULL) {
        neat_free_flow(flow);
    }
    if (ctx != NULL) {
        neat_free_ctx(ctx);
    }
    exit(result);
}
