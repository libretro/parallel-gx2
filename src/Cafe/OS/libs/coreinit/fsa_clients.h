#ifndef COREINIT_FSA_CLIENTS_H
#define COREINIT_FSA_CLIENTS_H

/* The FSA clients a title has added (FSAAddClient), with no lock: each is
 * an IOS device handle, kept in the slot of the IOSU's own handle index
 * (its low 12 bits, under 96), which no other open handle shares. C89. */

#include <stdint.h>

#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

#define FSA_CLIENTS_MAX 128

/* none; not while others use them */
void fsa_clients_reset(void);
/* false for 0 or an index past the table */
bool fsa_clients_add(uint32_t handle);
/* true for the one call that takes the handle out */
bool fsa_clients_remove(uint32_t handle);
bool fsa_clients_has(uint32_t handle);

RETRO_END_DECLS

#endif
