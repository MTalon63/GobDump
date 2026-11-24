#pragma once
#include "common/dsp_source_sink/dsp_sample_source.h"
#ifdef __ANDROID__
#include "ad9361.h"
#include "iio.h"
#include <dlfcn.h>
#include <sys/resource.h> // for testing live RDAS on android
#include <sys/syscall.h>
#include <unistd.h>
#else
#include <ad9361.h>
#include <iio.h>
#endif
#include "common/rimgui.h"
#include "common/widgets/double_list.h"
#include "logger.h"
#include <thread>

class PlutoSDRSource : public dsp::DSPSampleSource
{
protected:
    bool is_open = false, is_started = false;

    iio_context *ctx = NULL;
    iio_device *phy = NULL;
    iio_device *dev = NULL;

    satdump::widgets::DoubleList samplerate_widget;
    satdump::widgets::DoubleList bandwidth_widget;

    bool is_usb = false;
    bool manual_bandwidth = false;

    int gain = 0;
    int gain_mode = 0;
    int iq_mode = 0;
    int rf_input = 0;
    std::string ip_address = "192.168.2.1";
    int port = 30431; // Default iiod network port (Pluto's standard)

    void set_gains();
    void set_rfinput();
    void set_bandwidth();
    std::thread work_thread;
    bool thread_should_run = false;
    std::mutex work_thread_mtx;

    bool auto_reconnect = false;

    void mainThread()
    {
        work_thread_mtx.lock();
#ifdef __ANDROID__
        setpriority(PRIO_PROCESS, 0, -10); // Prevent WALT scheduler from starving IIO USB/TCP refills during heavy live demodulation

        // Hint to Qualcomm/OxygenOS PowerHAL via ADPF that DSP threads need sustained high clocks
        void *libandroid = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
        void *hint_mgr = nullptr;
        void *hint_session = nullptr;
        typedef void *(*pfn_getManager)();
        typedef void *(*pfn_createSession)(void *, const int32_t *, size_t, int64_t);
        typedef int (*pfn_reportActual)(void *, int64_t);
        typedef void (*pfn_closeSession)(void *);
        pfn_reportActual report_actual = nullptr;
        pfn_closeSession close_session = nullptr;

        if (libandroid)
        {
            auto get_mgr = (pfn_getManager)dlsym(libandroid, "APerformanceHint_getManager");
            auto create_sess = (pfn_createSession)dlsym(libandroid, "APerformanceHint_createSession");
            report_actual = (pfn_reportActual)dlsym(libandroid, "APerformanceHint_reportActualWorkDuration");
            close_session = (pfn_closeSession)dlsym(libandroid, "APerformanceHint_closeSession");
            if (get_mgr && create_sess && report_actual)
            {
                hint_mgr = get_mgr();
                int32_t tid = (int32_t)syscall(SYS_gettid);
                if (hint_mgr)
                    hint_session = create_sess(hint_mgr, &tid, 1, 10000000LL); // 10 ms target
            }
        }
#endif

    restart:
        int blockSize = dsp::STREAM_BUFFER_SIZE;
        if (iq_mode == 0) // CS16
            blockSize = std::min<int>(samplerate_widget.get_value() / 20.0, dsp::STREAM_BUFFER_SIZE);
        else
            blockSize = std::min<int>(samplerate_widget.get_value() / (20.0 * 2), dsp::STREAM_BUFFER_SIZE);
        int kernel_buffer_cnt = 16; // 16 buffers (~333 ms at 48 MSPS CS8) avoids DMA/TCP underflows on mobile schedulers
        struct iio_channel *rx0_i, *rx0_q;
        struct iio_buffer *rxbuf;

        rx0_i = iio_device_find_channel(dev, "voltage0", 0);
        rx0_q = iio_device_find_channel(dev, "voltage1", 0);

        iio_device_set_kernel_buffers_count(dev, kernel_buffer_cnt);

        logger->trace("PlutoSDR stream with %d buffers of size %d", kernel_buffer_cnt, blockSize);
        iio_channel_enable(rx0_i);
        if (iq_mode == 0) // CS16
        {
            iio_channel_enable(rx0_q);
            logger->trace("PlutoSDR stream in CS16");
        }
        else
        {
            iio_channel_disable(rx0_q);
            logger->trace("PlutoSDR stream in CS8");
        }
        rxbuf = iio_device_create_buffer(dev, blockSize, false);
        if (!rxbuf)
        {
            logger->error("Could not create RX buffer");
#ifdef __ANDROID__
            if (hint_session && close_session)
                close_session(hint_session);
            if (libandroid)
                dlclose(libandroid);
#endif
            work_thread_mtx.unlock();
            return;
        }

        uint32_t val = 0;
        // Reset underflow to avoid false flag
        iio_device_reg_write(dev, 0x80000088, val);
        while (thread_should_run)
        {
            if (iio_buffer_refill(rxbuf) < 0)
                break;

            iio_device_reg_read(dev, 0x80000088, &val);
            if (val & 4)
            {
                logger->warn("PlutoSDR underflow!");
                iio_device_reg_write(dev, 0x80000088, val);
            }
            if (iq_mode == 0)
            {
                int16_t *buf = (int16_t *)iio_buffer_first(rxbuf, rx0_i);
                volk_16i_s32f_convert_32f((float *)output_stream->writeBuf, buf, 2048.0f, blockSize * 2);
            }
            else
            {
                int8_t *buf = (int8_t *)iio_buffer_first(rxbuf, rx0_i);
                volk_8i_s32f_convert_32f((float *)output_stream->writeBuf, buf, 128.0f, blockSize * 2);
            }
            output_stream->swap(blockSize);
#ifdef __ANDROID__
            if (hint_session && report_actual)
                report_actual(hint_session, 12000000LL); // Signal sustained compute demand (> target)
#endif
        }

        iio_buffer_destroy(rxbuf);

        if (thread_should_run && auto_reconnect && !is_usb)
        {
            iio_context_destroy(ctx);
            is_started = false;
            while (thread_should_run)
            {
                logger->trace("Trying to reconnect PlutoSDR");
                try
                {
                    sdr_startup();
                    goto restart;
                }
                catch (std::runtime_error &e)
                {
                    logger->trace(e.what());
                }
            }
        }

#ifdef __ANDROID__
        if (hint_session && close_session)
            close_session(hint_session);
        if (libandroid)
            dlclose(libandroid);
#endif
        work_thread_mtx.unlock();
    }

    void start_thread()
    {
        work_thread_mtx.lock();
        work_thread_mtx.unlock();

        if (!thread_should_run)
        {
            thread_should_run = true;
            work_thread = std::thread(&PlutoSDRSource::mainThread, this);
        }
    }

    void stop_thread()
    {
        thread_should_run = false;
        logger->info("Waiting for the thread...");
        if (is_started)
            output_stream->stopWriter();
        if (work_thread.joinable())
            work_thread.join();
        logger->info("Thread stopped");
    }

    void sdr_startup();

public:
    PlutoSDRSource(dsp::SourceDescriptor source) : DSPSampleSource(source), samplerate_widget("Samplerate"), bandwidth_widget("Bandwidth") { thread_should_run = false; }

    ~PlutoSDRSource()
    {
        stop();
        close();
    }

    void set_settings(nlohmann::json settings);
    nlohmann::json get_settings();

    void open();
    void start();
    void stop();
    void close();

    void set_frequency(uint64_t frequency);

    void drawControlUI();

    void set_samplerate(uint64_t samplerate);
    uint64_t get_samplerate();

    static std::string getID() { return "plutosdr"; }
    static std::shared_ptr<dsp::DSPSampleSource> getInstance(dsp::SourceDescriptor source) { return std::make_shared<PlutoSDRSource>(source); }
    static std::vector<dsp::SourceDescriptor> getAvailableSources();
};
