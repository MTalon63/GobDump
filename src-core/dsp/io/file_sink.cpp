#include "file_sink.h"
#include "dsp/block.h"
#include "dsp/block_helpers.h"
#include "logger.h"
#include <cstdint>

namespace satdump
{
    namespace ndsp
    {
        template <typename T>
        FileSinkBlock<T>::FileSinkBlock()
            : Block("file_sink_" + getShortTypeName<T>(), //
                    {{"out", getTypeSampleType<T>()}}, {})
        {
        }

        template <typename T>
        FileSinkBlock<T>::~FileSinkBlock()
        {
        }

        template <typename T>
        bool FileSinkBlock<T>::work()
        {
            DSPBuffer iblk = inputs[0].fifo->wait_dequeue();

            if (iblk.isTerminator())
            {
                inputs[0].fifo->free(iblk);
                return true;
            }

            file_writer.write((char *)iblk.getSamples<T>(), iblk.size * sizeof(T));
            file_writer.flush();

            // ofstream swallows write errors unless asked. A full disk silently produced a truncated
            // recording that still looked like it was being written.
            if (!file_writer.good() && !d_write_error_logged)
            {
                logger->error("Error writing to file sink! Recording may be truncated (disk full?)");
                d_write_error_logged = true;
            }

            inputs[0].fifo->free(iblk);

            return false;
        }

        template class FileSinkBlock<complex_t>;
        template class FileSinkBlock<float>;
        template class FileSinkBlock<int16_t>;
        template class FileSinkBlock<int8_t>;
        template class FileSinkBlock<uint8_t>;
    } // namespace ndsp
} // namespace satdump