#ifndef ELM327_PROTOCOL_H
#define ELM327_PROTOCOL_H

#include <stdbool.h>

#include "../Core/OBD2.h"

typedef struct {
    obd2_t *obd;
} elm327_protocol_t;

void elm327_protocol_init(elm327_protocol_t *protocol, obd2_t *obd);
bool elm327_protocol_process(elm327_protocol_t *protocol, const char *command);

#endif
