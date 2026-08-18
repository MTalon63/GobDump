#pragma once

#include <cstddef>
#include <cstdint>

namespace satdump
{
    /// Bytes available at `offset` in a buffer of `size`. 0 if offset is at or past the end, so
    /// `buf_remaining(p.size(), 41)` is safe even when the packet is shorter than the header.
    inline size_t buf_remaining(size_t size, size_t offset) { return offset >= size ? 0 : size - offset; }

    /// True if [offset, offset+len) is entirely inside `size`. Written to be immune to overflow.
    inline bool buf_fits(size_t size, size_t offset, size_t len) { return offset <= size && len <= size - offset; }

    /// Largest length that fits at `offset`, i.e. `len` truncated to the space actually available.
    inline size_t buf_clip_len(size_t size, size_t offset, size_t len)
    {
        const size_t avail = buf_remaining(size, offset);
        return len < avail ? len : avail;
    }

    /// Signed lengths from received data may be negative before they are ever compared; treat any
    /// negative as zero rather than letting it convert to a huge size_t at the memcpy call.
    inline size_t buf_len_nonneg(long long len) { return len <= 0 ? 0 : (size_t)len; }
} // namespace satdump
