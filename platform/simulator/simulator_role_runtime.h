#ifndef SIMULATOR_ROLE_RUNTIME_H
#define SIMULATOR_ROLE_RUNTIME_H

#include "simulator_role.h"

#ifdef __cplusplus
extern "C" {
#endif

void simulator_role_runtime_set(simulator_role_t role);
simulator_role_t simulator_role_runtime_get(void);
int simulator_role_runtime_is_master(void);
int simulator_role_runtime_is_slave(void);

#ifdef __cplusplus
}
#endif

#endif
