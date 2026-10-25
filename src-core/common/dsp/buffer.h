#pragma once

#include <vector>
#include <deque>
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
    static constexpr int STREAM_QUEUE_LEN = 4; // headroom so producers rarely block

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
        stream(int stream_size = STREAM_BUFFER_SIZE) : buffer_size(stream_size)
        {
            for (int i = 0; i < STREAM_QUEUE_LEN; i++)
            {
                T *b = create_volk_buffer<T>(stream_size);
                bufs.push_back(b);
                freeQ.push_back(b);
            }
            writeBuf = freeQ.front();
            freeQ.pop_front();
            // Some callers touch readBuf outside a read()/flush() pair.
            readBuf = bufs[0];
        }
        ~stream() { for (size_t i = 0; i < bufs.size(); i++) volk_free(bufs[i]); }
        stream(const stream &) = delete;
        stream &operator=(const stream &) = delete;
        bool swap(int size)
        {
            std::unique_lock<std::mutex> lck(mtx);
            if (writerStop) return false;
            readyQ.push_back(std::make_pair(writeBuf, size));
            dataSize = size;
            lck.unlock(); cvReady.notify_all(); lck.lock();
            cvSwap.wait(lck, [this] { return (!freeQ.empty() || writerStop); });
            if (writerStop) return false;
            writeBuf = freeQ.front(); freeQ.pop_front();
            return true;
        }
        int read()
        {
            std::unique_lock<std::mutex> lck(mtx);
            // writerStop must wake a parked reader, or shutdown hangs.
            cvReady.wait(lck, [this] { return (!readyQ.empty() || readerStop || writerStop); });
            // Drain queued chunks before reporting end-of-stream.
            if (!readyQ.empty())
            {
                readBuf = readyQ.front().first;
                dataSize = readyQ.front().second;
                readyQ.pop_front();
                held = true;
                return dataSize;
            }
            return -1;
        }
        void flush()
        {
            {
                std::lock_guard<std::mutex> lck(mtx);
                if (held)
                {
                    freeQ.push_back(readBuf);
                    held = false;
                }
            }
            cvSwap.notify_all();
        }
        void stopWriter() { { std::lock_guard<std::mutex> lck(mtx); writerStop = true; } cvSwap.notify_all(); cvReady.notify_all(); }
        void clearWriteStop() { std::lock_guard<std::mutex> lck(mtx); writerStop = false; }
        void stopReader() { { std::lock_guard<std::mutex> lck(mtx); readerStop = true; } cvSwap.notify_all(); cvReady.notify_all(); }
        void clearReadStop() { std::lock_guard<std::mutex> lck(mtx); readerStop = false; }
        bool exhausted()
        {
            std::lock_guard<std::mutex> lck(mtx);
            return writerStop && readyQ.empty();
        }
        int getDataSize() { return dataSize; }
        int getBufferSize() { return buffer_size; }
        T *writeBuf;
        T *readBuf;
    private:
        std::mutex mtx;
        std::condition_variable cvSwap, cvReady;
        std::deque<std::pair<T *, int>> readyQ;
        std::deque<T *> freeQ;
        std::vector<T *> bufs;
        bool readerStop = false, writerStop = false, held = false;
        const int buffer_size;
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
            // Report queued data BEFORE honouring the stop flag. Returning -1 the moment
            // _stopReader is set discards whatever is still buffered, which is the tail of the
            // capture - the reader must be allowed to drain first.
            int _r = getReadable();
            if (_r != 0)
            {
                return _r;
            }
            if (_stopReader)
            {
                return -1;
            }
            std::unique_lock<std::mutex> lck(_readable_mtx);
            canReadVar.wait(lck, [=]()
                            { return ((this->getReadable(false) > 0) || this->getReadStop()); });
            _r = getReadable(false);
            if (_r != 0)
            {
                return _r;
            }
            return _stopReader ? -1 : 0;
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
        // "No data now" vs "no data ever again, and none buffered". A consumer uses this to
        // finish only once it has actually drained, instead of exiting on a stop flag and
        // discarding what is still queued.
        bool exhausted()
        {
            _readable_mtx.lock();
            int r = readable;
            _readable_mtx.unlock();
            return _stopReader && r == 0;
        }
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