/* Cache maintenance. Memory here is coherent (there are no CPU caches to
 * flush and the GPU/DSP read guest memory directly), so the SDK's range
 * functions return at once. Without these, every dcbf/dcbi/icbi in their
 * loops is a trip through the host (one per 32 bytes). DCZeroRange is left
 * alone: its dcbz really clears memory. */
#include "hle.h"

#define NOP_HOOK(addr, name) WP_HOOK(addr, name) { wp_return(c); return 1; }

NOP_HOOK(0x8028E000, DCInvalidateRange)
NOP_HOOK(0x8028E030, DCFlushRange)
NOP_HOOK(0x8028E060, DCStoreRange)
NOP_HOOK(0x8028E090, DCFlushRangeNoSync)
NOP_HOOK(0x8028E0C0, DCStoreRangeNoSync)
NOP_HOOK(0x8028E120, ICInvalidateRange)
NOP_HOOK(0x8028E160, ICFlashInvalidate)
