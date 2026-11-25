#pragma once

#include <cstddef>

namespace tracks {

#if defined(__APPLE__)
void setMacDockIconFromPngData(const void* data, size_t size);
#else
inline void setMacDockIconFromPngData(const void*, size_t) {}
#endif

} // namespace tracks
