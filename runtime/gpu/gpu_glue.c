/* Globals and small services the GPU module expects from the reference
 * runtime, provided by Wonderpark. */
#include "ppc.h"

void (*g_gx_flush)(void);
void (*g_gx_copy_sync)(uint32_t addr);
uint64_t (*g_host_frame_due)(void);
void (*g_host_frame_render)(void);
uint64_t g_wait_ticks; /* time spent idle, for the busy share in the fps line */
