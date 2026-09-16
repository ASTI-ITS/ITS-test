#include "ELM327Protocol.h"

void elm327_protocol_init(elm327_protocol_t *protocol, obd2_t *obd) {
    if (!protocol) return;
    protocol->obd = obd;
}

bool elm327_protocol_process(elm327_protocol_t *protocol, const char *command) {
    if (!protocol || !protocol->obd || !command) return false;
    return obd2_command(protocol->obd, command);
}
