#pragma once
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <stdexcept>
#include <volk/volk.h>
#include <string.h>
#include "common/dsp/complex.h"
#include "dll_export.h"

namespace dsp
{
    // Sized once from config in initSatDump(), constant after: raising them later would leave every
    // already-allocated buffer smaller than the size the rest of the code then assumes.
    SATDUMP_DLL extern int STREAM_BUFFER_SIZE;
    SATDUMP_DLL extern int RING_BUF_SZ;

    static constexpr int MIN_BUFFER_SIZE = 8192 * 2; // module_demod_base already assumes >= 8192+1
    static constexpr int MAX_BUFFER_SIZE = 1000000000 / (int)sizeof(complex_t);

    SATDUMP_DLL int setDefaultBufferSize(int size); // Validates; returns the value applied
    SATDUMP_DLL void lockDefaultBufferSize();       // Reject later changes, once init is done

    /*
    Util function to create a volk aligned buffer
    */
    template <typename T>
    T *create_volk_buffer(int size, bool zero = true)
    {
        // A negative size became a huge size_t in the multiply, so volk_malloc returned NULL and every
        // later write went through it. Genuine allocation failure was unchecked too.
        if (size <= 0)
            throw std::runtime_error("Invalid DSP buffer size requested!");

        T *buffer = (T *)volk_malloc((size_t)size * sizeof(T), volk_get_alignment());
        if (buffer == nullptr)
            throw std::runtime_error("Could not allocate DSP buffer!");

        if (zero)
            for (int i = 0; i < size; i++)
                buffer[i] = 0;
        return buffer;
    }

    template <class T>
    class stream
    {
    public:
        stream(int stream_size = STREAM_BUFFER_SIZE)
        {
            writeBuf = create_volk_buffer<T>(stream_size);
            readBuf = create_volk_buffer<T>(stream_size);

            for (int i = 0; i < stream_size; i++)
            {
                writeBuf[i] = 0;
                readBuf[i] = 0;
            }
        }

        ~stream()
        {
            volk_free(writeBuf);
            volk_free(readBuf);
        }

        // writeBuf/readBuf are raw owning volk allocations freed above, so a shallow copy would alias
        // them and both destructors would free the same blocks. Streams are shared by pointer anyway.
        stream(const stream &) = delete;
        stream &operator=(const stream &) = delete;

        bool swap(int size)
        {
            {
                // Wait to either swap or stop
                std::unique_lock<std::mutex> lck(swapMtx);
                swapCV.wait(lck, [this]
                            { return (canSwap || writerStop); });

                // If writer was stopped, abandon operation
                if (writerStop)
                {
                    return false;
                }

                // Swap buffers
                dataSize = size;
                T *temp = writeBuf;
                writeBuf = readBuf;
                readBuf = temp;
                canSwap = false;
            }

            // Notify reader that some data is ready
            {
                std::lock_guard<std::mutex> lck(rdyMtx);
                dataReady = true;
            }
            rdyCV.notify_all();

            return true;
        }

        int read()
        {
            // Wait for data to be ready or to be stopped
            std::unique_lock<std::mutex> lck(rdyMtx);
            rdyCV.wait(lck, [this]
                       { return (dataReady || readerStop); });

            return (readerStop ? -1 : dataSize);
        }

        void flush()
        {
            // Clear data ready
            {
                std::lock_guard<std::mutex> lck(rdyMtx);
                dataReady = false;
            }

            // Notify writer that buffers can be swapped
            {
                std::lock_guard<std::mutex> lck(swapMtx);
                canSwap = true;
            }

            swapCV.notify_all();
        }

        void stopWriter()
        {
            {
                std::lock_guard<std::mutex> lck(swapMtx);
                writerStop = true;
            }
            swapCV.notify_all();
        }

        // Cleared under the lock, so a waiter can't observe a stale true for a restarted stream
        void clearWriteStop()
        {
            std::lock_guard<std::mutex> lck(swapMtx);
            writerStop = false;
        }

        void stopReader()
        {
            {
                std::lock_guard<std::mutex> lck(rdyMtx);
                readerStop = true;
            }
            rdyCV.notify_all();
        }

        void clearReadStop()
        {
            std::lock_guard<std::mutex> lck(rdyMtx);
            readerStop = false;
        }
        int getDataSize() { return dataSize; }
        bool getReady() { return dataReady; }

        T *writeBuf;
        T *readBuf;

    private:
        std::mutex swapMtx;
        std::condition_variable swapCV;
        bool canSwap = true;

        std::mutex rdyMtx;
        std::condition_variable rdyCV;
        bool dataReady = false;

        bool readerStop = false;
        bool writerStop = false;

        int dataSize = 0;
    };

    template <class T>
    class RingBuffer
    {
    public:
        RingBuffer()
        {
        }

        RingBuffer(int maxLatency) { init(maxLatency); }

        ~RingBuffer()
        {
            if (size != 0)
                delete[] _buffer;
            size = 0;
        }

        // _buffer is a raw owning allocation freed above; a shallow copy would alias it and both
        // destructors would delete[] the same block.
        RingBuffer(const RingBuffer &) = delete;
        RingBuffer &operator=(const RingBuffer &) = delete;

        void init(int maxLatency)
        {
            if (size != 0) // Re-init used to overwrite the pointer and leak the old block
                delete[] _buffer;

            size = RING_BUF_SZ;
            _buffer = new T[size];
            _stopReader = false;
            _stopWriter = false;
            this->maxLatency = maxLatency;
            writec = 0;
            readc = 0;
            readable = 0;
            writable = size;
            memset((void *)_buffer, 0, size * sizeof(T));
        }

        int read(T *data, int len)
        {
            int dataRead = 0;
            int toRead = 0;
            while (dataRead < len)
            {
                toRead = std::min<int>(waitUntilReadable(), len - dataRead);
                if (toRead < 0)
                {
                    return -1;
                };

                if ((toRead + readc) > size)
                {
                    memcpy(&data[dataRead], &_buffer[readc], (size - readc) * sizeof(T));
                    memcpy(&data[dataRead + (size - readc)], &_buffer[0], (toRead - (size - readc)) * sizeof(T));
                }
                else
                {
                    memcpy(&data[dataRead], &_buffer[readc], toRead * sizeof(T));
                }

                dataRead += toRead;

                _readable_mtx.lock();
                readable -= toRead;
                _readable_mtx.unlock();
                _writable_mtx.lock();
                writable += toRead;
                _writable_mtx.unlock();
                readc = (readc + toRead) % size;
                canWriteVar.notify_one();
            }
            return len;
        }

        int readAndSkip(T *data, int len, int skip)
        {
            int dataRead = 0;
            int toRead = 0;
            while (dataRead < len)
            {
                toRead = std::min<int>(waitUntilReadable(), len - dataRead);
                if (toRead < 0)
                {
                    return -1;
                };

                if ((toRead + readc) > size)
                {
                    memcpy(&data[dataRead], &_buffer[readc], (size - readc) * sizeof(T));
                    memcpy(&data[dataRead + (size - readc)], &_buffer[0], (toRead - (size - readc)) * sizeof(T));
                }
                else
                {
                    memcpy(&data[dataRead], &_buffer[readc], toRead * sizeof(T));
                }

                dataRead += toRead;

                _readable_mtx.lock();
                readable -= toRead;
                _readable_mtx.unlock();
                _writable_mtx.lock();
                writable += toRead;
                _writable_mtx.unlock();
                readc = (readc + toRead) % size;
                canWriteVar.notify_one();
            }
            dataRead = 0;
            while (dataRead < skip)
            {
                toRead = std::min<int>(waitUntilReadable(), skip - dataRead);
                if (toRead < 0)
                {
                    return -1;
                };

                dataRead += toRead;

                _readable_mtx.lock();
                readable -= toRead;
                _readable_mtx.unlock();
                _writable_mtx.lock();
                writable += toRead;
                _writable_mtx.unlock();
                readc = (readc + toRead) % size;
                canWriteVar.notify_one();
            }
            return len;
        }

        int waitUntilReadable()
        {
            if (_stopReader)
            {
                return -1;
            }
            int _r = getReadable();
            if (_r != 0)
            {
                return _r;
            }
            std::unique_lock<std::mutex> lck(_readable_mtx);
            canReadVar.wait(lck, [=]()
                            { return ((this->getReadable(false) > 0) || this->getReadStop()); });
            if (_stopReader)
            {
                return -1;
            }
            return getReadable(false);
        }

        int getReadable(bool lock = true)
        {
            if (lock)
            {
                _readable_mtx.lock();
            };
            int _r = readable;
            if (lock)
            {
                _readable_mtx.unlock();
            };
            return _r;
        }

        int write(T *data, int len)
        {
            int dataWritten = 0;
            int toWrite = 0;
            while (dataWritten < len)
            {
                toWrite = std::min<int>(waitUntilwritable(), len - dataWritten);
                if (toWrite < 0)
                {
                    return -1;
                };

                if ((toWrite + writec) > size)
                {
                    memcpy(&_buffer[writec], &data[dataWritten], (size - writec) * sizeof(T));
                    memcpy(&_buffer[0], &data[dataWritten + (size - writec)], (toWrite - (size - writec)) * sizeof(T));
                }
                else
                {
                    memcpy(&_buffer[writec], &data[dataWritten], toWrite * sizeof(T));
                }

                dataWritten += toWrite;

                _readable_mtx.lock();
                readable += toWrite;
                _readable_mtx.unlock();
                _writable_mtx.lock();
                writable -= toWrite;
                _writable_mtx.unlock();
                writec = (writec + toWrite) % size;

                canReadVar.notify_one();
            }
            return len;
        }

        int waitUntilwritable()
        {
            if (_stopWriter)
            {
                return -1;
            }
            int _w = getWritable();
            if (_w != 0)
            {
                return _w;
            }
            std::unique_lock<std::mutex> lck(_writable_mtx);
            canWriteVar.wait(lck, [=]()
                             { return ((this->getWritable(false) > 0) || this->getWriteStop()); });
            if (_stopWriter)
            {
                return -1;
            }
            return getWritable(false);
        }

        int getWritable(bool lock = true)
        {
            if (lock)
            {
                _writable_mtx.lock();
            };
            int _w = writable;
            if (lock)
            {
                _writable_mtx.unlock();
                _readable_mtx.lock();
            };
            int _r = readable;
            if (lock)
            {
                _readable_mtx.unlock();
            };
            return std::max<int>(std::min<int>(_w, maxLatency - _r), 0);
        }

        // The flag must be written under the same mutex the waiter re-checks its predicate with,
        // otherwise a stop landing between the predicate test and the wait is lost and the waiter
        // parks forever - the shutdown hang. notify_all because several may be parked.
        void stopReader()
        {
            {
                std::lock_guard<std::mutex> lck(_readable_mtx);
                _stopReader = true;
            }
            canReadVar.notify_all();
        }

        void stopWriter()
        {
            {
                std::lock_guard<std::mutex> lck(_writable_mtx);
                _stopWriter = true;
            }
            canWriteVar.notify_all();
        }

        bool getReadStop() { return _stopReader; }
        bool getWriteStop() { return _stopWriter; }

        // Cleared under the lock too, so a waiter can't see a stale true for a just-restarted buffer
        void clearReadStop()
        {
            std::lock_guard<std::mutex> lck(_readable_mtx);
            _stopReader = false;
        }

        void clearWriteStop()
        {
            std::lock_guard<std::mutex> lck(_writable_mtx);
            _stopWriter = false;
        }
        void setMaxLatency(int maxLatency) { this->maxLatency = maxLatency; }

    private:
        T *_buffer;
        int size = 0;
        int readc;
        int writec;
        int readable;
        int writable;
        int maxLatency;
        std::atomic<bool> _stopReader; // Polled unlocked on the fast paths, so must be atomic
        std::atomic<bool> _stopWriter;
        std::mutex _readable_mtx;
        std::mutex _writable_mtx;
        std::condition_variable canReadVar;
        std::condition_variable canWriteVar;
    };
};