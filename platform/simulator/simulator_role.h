#ifndef SIMULATOR_ROLE_H
#define SIMULATOR_ROLE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SIMULATOR_ROLE_MASTER = 0,
    SIMULATOR_ROLE_SLAVE_LEFT,
    SIMULATOR_ROLE_SLAVE_RIGHT
} simulator_role_t;

/*
 * 默认角色是 master。
 *
 * 支持：
 *   --role master
 *   --role slave-left
 *   --role slave-right
 *   --role=master
 *   --role=slave-left
 *   --role=slave-right
 *
 * 成功返回 0，参数错误返回 -1。
 */
int simulator_role_parse(
    int argc,
    char **argv,
    simulator_role_t *out_role
);

const char *simulator_role_name(simulator_role_t role);

#ifdef __cplusplus
}
#endif

#endif
