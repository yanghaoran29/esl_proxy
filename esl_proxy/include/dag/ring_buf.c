/*
 * ring_buf.c - Global ring buffer definitions
 *
 * Defines the 4 global ring buffers used for task data storage.
 * Included once in the build to define globals.
 */

#include "ring_buf.h"

_Atomic uint32_t g_state_buf[RING_SIZE];
_Atomic struct task_desc g_basic_buf[RING_SIZE];
_Atomic uint32_t g_dep_buf[RING_SIZE];
void *g_runtime_buf[RING_SIZE];