/*
 * libFuzzer target for the state parser, which root reads at boot.
 * Anything it accepts must survive a format/parse round trip unchanged.
 * Build and run: make fuzz   (needs clang)
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "state.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    struct state s, again;
    char out[STATE_MAX_SIZE];
    if (state_parse((const char *)data, size, &s, NULL) != STATE_LOADED)
        return 0;
    if (state_format(&s, out, sizeof(out)) != 0)
        abort();
    if (state_parse(out, strlen(out), &again, NULL) != STATE_LOADED)
        abort();
    if (memcmp(&s.all, &again.all, sizeof(s.all)) != 0 || s.ngpu != again.ngpu)
        abort();
    for (unsigned i = 0; i < s.ngpu; i++)
        if (strcmp(s.gpu[i].uuid, again.gpu[i].uuid) != 0 ||
            memcmp(&s.gpu[i].req, &again.gpu[i].req, sizeof(s.gpu[i].req)) != 0)
            abort();
    return 0;
}
