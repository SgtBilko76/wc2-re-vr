/* The SDL port includes <lzo1x.h> (full LZO); the Quest build uses miniLZO,
 * which provides the same lzo1x_decompress_safe API. */
#include "minilzo.h"
