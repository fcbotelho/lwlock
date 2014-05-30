/***
 *
 * Independently contributed to the lwlock library. This file is also released under the
 * terms of MPL 2.0 license but you do not need to sign the committer agreement for
 * this file.
 */

#ifndef __LW_MONITOR_H__
#define __LW_MONITOR_H__

#include "lw_types.h"
#include "lw_mutex.h"
#include "lw_cond_var.h"

/*
 * Monitor or implicit locks and condition variables: Instread of explicitly declaring
 * and maintaining locks in various structures, monitors or implicit locks provide for
 * dynamically/under the covers association of pointers with locks. The pointer could be
 * to any stucture or function or basically any 64-bit number that is used consistenly as a
 * locking device.
 *
 */

void lw_monitor_module_init(lw_uint32_t num_monitors);

void lw_monitor_module_deinit(void);

typedef lw_uint64_t lw_monitor_id_t;

#define INVALID_MONITOR         (0)

lw_monitor_id_t lw_monitor_lock(void *ptr);
void lw_monitor_unlock(void *ptr, lw_monitor_id_t monitor);
void lw_monitor_wait(void *ptr, lw_monitor_id_t *monitor);
void lw_monitor_signal(void *ptr, lw_monitor_id_t monitor);
void lw_monitor_broadcast(void *ptr, lw_monitor_id_t monitor);

/* Convenience wrappers for the common way of using monitors. */

#define MONITOR_LOCK_START(_ptr)                                                \
    do {                                                                        \
        void *_monitored_ptr = (_ptr);                                          \
        lw_monitor_id_t _monitor = lw_monitor_lock(_monitored_ptr)              \


#define MONITOR_WAIT(_expr)                                                     \
        while (!(_expr)) {                                                      \
            lw_monitor_wait(_monitored_ptr, &_monitor);                         \
        }

#define MONITOR_SIGNAL                                                          \
        lw_monitor_signal(_monitored_ptr, _monitor)                             \

#define MONITOR_BROADCAST                                                       \
        lw_monitor_broadcast(_monitored_ptr, _monitor)                          \

#define MONITOR_LOCK_END                                                        \
        lw_monitor_unlock(_monitored_ptr, _monitor);                            \
    } while (0)


#define MONITOR_ENTER(_label)                                                   \
    do {                                                                        \
        static lw_uint32_t monitor_var_##_label = 0;                            \
        void *_monitored_ptr = &(monitor_var_##_label);                         \
        lw_monitor_id_t _monitor = lw_monitor_lock(&_monitored_ptr);            \
        ++(monitor_var_##_label)

#define MONITOR_LEAVE                                                           \
        lw_monitor_unlock(_monitored_ptr, _monitor);                            \
    } while (0)

#endif /* __LW_MONITOR_H__ */
