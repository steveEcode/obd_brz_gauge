#include "simulator_role.h"

#include <stddef.h>
#include <string.h>

static int parse_role_name(
    const char *name,
    simulator_role_t *out_role
)
{
    if (name == NULL || out_role == NULL) {
        return -1;
    }

    if (strcmp(name, "master") == 0) {
        *out_role = SIMULATOR_ROLE_MASTER;
        return 0;
    }

    if (
        strcmp(name, "slave-left") == 0 ||
        strcmp(name, "left") == 0
    ) {
        *out_role = SIMULATOR_ROLE_SLAVE_LEFT;
        return 0;
    }

    if (
        strcmp(name, "slave-right") == 0 ||
        strcmp(name, "right") == 0
    ) {
        *out_role = SIMULATOR_ROLE_SLAVE_RIGHT;
        return 0;
    }

    return -1;
}

int simulator_role_parse(
    int argc,
    char **argv,
    simulator_role_t *out_role
)
{
    if (out_role == NULL) {
        return -1;
    }

    /* 不写参数时默认作为主表启动。 */
    *out_role = SIMULATOR_ROLE_MASTER;

    for (int index = 1; index < argc; index++) {
        const char *argument = argv[index];

        if (strcmp(argument, "--role") == 0) {
            if (index + 1 >= argc) {
                return -1;
            }

            index++;

            if (parse_role_name(argv[index], out_role) != 0) {
                return -1;
            }

            continue;
        }

        static const char prefix[] = "--role=";

        if (
            strncmp(argument, prefix, sizeof(prefix) - 1) == 0
        ) {
            const char *role_name =
                argument + sizeof(prefix) - 1;

            if (parse_role_name(role_name, out_role) != 0) {
                return -1;
            }

            continue;
        }

        return -1;
    }

    return 0;
}

const char *simulator_role_name(simulator_role_t role)
{
    switch (role) {
        case SIMULATOR_ROLE_MASTER:
            return "master";

        case SIMULATOR_ROLE_SLAVE_LEFT:
            return "slave-left";

        case SIMULATOR_ROLE_SLAVE_RIGHT:
            return "slave-right";

        default:
            return "unknown";
    }
}
