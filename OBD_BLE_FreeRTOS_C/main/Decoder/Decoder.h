#ifndef DECODER_H
#define DECODER_H

#include <stdint.h>

#include "../Data/Mode01Data.h"

typedef struct {
    mode01_data_t *data;
} decoder_t;

void decoder_init(decoder_t *decoder, mode01_data_t *storage);
void decoder_decode(
    decoder_t *decoder,
    uint8_t mode,
    uint8_t pid,
    const uint8_t *payload,
    uint8_t length
);

#endif
