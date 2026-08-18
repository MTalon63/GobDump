#define SATDUMP_DLL_EXPORT 1
#include "buffer.h"
#include "logger.h"

namespace dsp
{
    // 1MB buffers
    SATDUMP_DLL int STREAM_BUFFER_SIZE = 1000000;
    SATDUMP_DLL int RING_BUF_SZ = 1000000;

    static bool buffer_size_locked = false;

    SATDUMP_DLL int setDefaultBufferSize(int size)
    {
        // Every DSP buffer is sized from this, so a change after any exist silently under-allocates
        // them all. Only honour it before the pipeline starts.
        if (buffer_size_locked)
        {
            logger->error("DSP buffer size can only be set at startup! Keeping %d", STREAM_BUFFER_SIZE);
            return STREAM_BUFFER_SIZE;
        }

        if (size < MIN_BUFFER_SIZE || size > MAX_BUFFER_SIZE)
        {
            logger->error("Requested DSP buffer size %d is out of range [%d, %d]! Keeping %d", size, MIN_BUFFER_SIZE, MAX_BUFFER_SIZE, STREAM_BUFFER_SIZE);
            return STREAM_BUFFER_SIZE;
        }

        STREAM_BUFFER_SIZE = size;
        RING_BUF_SZ = size;
        return size;
    }

    SATDUMP_DLL void lockDefaultBufferSize() { buffer_size_locked = true; }
}; // namespace dsp
