#ifndef TRANSPORT_H
#define TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TRANSPORT_FRAME_SIZE 8

typedef struct {
    uint32_t id;
    uint8_t dlc;
    uint8_t data[TRANSPORT_FRAME_SIZE];
} transport_frame_t;

typedef struct {
    void *ctx;
    bool (*begin)(void *ctx);
    bool (*send)(void *ctx, const transport_frame_t *frame);
    bool (*available)(void *ctx);
    bool (*receive)(void *ctx, transport_frame_t *frame);
    void (*poll)(void *ctx);
} transport_iface_t;

#endif
