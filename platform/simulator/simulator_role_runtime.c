#include "simulator_role_runtime.h"

static simulator_role_t s_current_role =
    SIMULATOR_ROLE_MASTER;

void simulator_role_runtime_set(simulator_role_t role)
{
    s_current_role = role;
}

simulator_role_t simulator_role_runtime_get(void)
{
    return s_current_role;
}

int simulator_role_runtime_is_master(void)
{
    return s_current_role == SIMULATOR_ROLE_MASTER;
}

int simulator_role_runtime_is_slave(void)
{
    return
        s_current_role == SIMULATOR_ROLE_SLAVE_LEFT ||
        s_current_role == SIMULATOR_ROLE_SLAVE_RIGHT;
}
