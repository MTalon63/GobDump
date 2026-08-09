/**
@file Connection_uLimeSDR.cpp
@author Lime Microsystems
@brief Implementation of uLimeSDR board connection.
*/

#include "ConnectionFT601.h"
#include <cstring>
#include <iostream>
#include <vector>

#include <thread>
#include <chrono>
#include <FPGA_common.h>
#include <ciso646>
#include "Logger.h"

using namespace std;
using namespace lime;

const int ConnectionFT601::streamWrEp = 0x03;
const int ConnectionFT601::streamRdEp = 0x83;
const int ConnectionFT601::ctrlWrEp = 0x02;
const int ConnectionFT601::ctrlRdEp = 0x82;

ConnectionFT601::ConnectionFT601(void *arg)
{
    isConnected = false;

    dev_handle = 0;
    mUsbCounter = 0;
    ctx = (libusb_context *)arg;
}

/**	@brief Initializes port type and object necessary to communicate to usb device.
*/
ConnectionFT601::ConnectionFT601(void *arg, const ConnectionHandle &handle)
{
    isConnected = false;
    int pid = -1;
    int vid = -1;
    mSerial = std::strtoll(handle.serial.c_str(),nullptr,16);

    const auto pidvid = handle.addr;
    const auto splitPos = pidvid.find(":");
    pid = std::stoi(pidvid.substr(0, splitPos));
    vid = std::stoi(pidvid.substr(splitPos+1));
    dev_handle = 0;
    mUsbCounter = 0;
    ctx = (libusb_context *)arg;

    if (this->Open(handle.serial, vid, pid) != 0)
        lime::error("Failed to open device");
}

/**	@brief Initializes port type and object necessary to communicate to usb device.
*/
ConnectionFT601::ConnectionFT601(void *arg, const ConnectionHandle &handle, int fd)
{
    isConnected = false;

    dev_handle = 0;
    mUsbCounter = 0;
    ctx = (libusb_context *)arg;

    if (this->Open_fd(handle.serial, fd) != 0)
        lime::error("Failed to open device");
}

/**	@brief Closes connection to chip and deallocates used memory.
*/
ConnectionFT601::~ConnectionFT601()
{
    Close();
}

int ConnectionFT601::FT_FlushPipe(unsigned char ep)
{
    int actual = 0;
    unsigned char wbuffer[20]={0};

    mUsbCounter++;
    wbuffer[0] = (mUsbCounter)&0xFF;
    wbuffer[1] = (mUsbCounter>>8)&0xFF;
    wbuffer[2] = (mUsbCounter>>16)&0xFF;
    wbuffer[3] = (mUsbCounter>>24)&0xFF;
    wbuffer[4] = ep;
    libusb_bulk_transfer(dev_handle, 0x01, wbuffer, 20, &actual, 1000);
    if (actual != 20)
        return -1;

    mUsbCounter++;
    wbuffer[0] = (mUsbCounter)&0xFF;
    wbuffer[1] = (mUsbCounter>>8)&0xFF;
    wbuffer[2] = (mUsbCounter>>16)&0xFF;
    wbuffer[3] = (mUsbCounter>>24)&0xFF;
    wbuffer[4] = ep;
    wbuffer[5] = 0x03;
    libusb_bulk_transfer(dev_handle, 0x01, wbuffer, 20, &actual, 1000);
    if (actual != 20)
        return -1;
    return 0;
}

int ConnectionFT601::FT_SetStreamPipe(unsigned char ep, size_t size)
{
    int actual = 0;
    unsigned char wbuffer[20]={0};

    mUsbCounter++;
    wbuffer[0] = (mUsbCounter)&0xFF;
    wbuffer[1] = (mUsbCounter>>8)&0xFF;
    wbuffer[2] = (mUsbCounter>>16)&0xFF;
    wbuffer[3] = (mUsbCounter>>24)&0xFF;
    wbuffer[4] = ep;
    libusb_bulk_transfer(dev_handle, 0x01, wbuffer, 20, &actual, 1000);
    if (actual != 20)
        return -1;

    mUsbCounter++;
    wbuffer[0] = (mUsbCounter)&0xFF;
    wbuffer[1] = (mUsbCounter>>8)&0xFF;
    wbuffer[2] = (mUsbCounter>>16)&0xFF;
    wbuffer[3] = (mUsbCounter>>24)&0xFF;
    wbuffer[5] = 0x02;
    wbuffer[8] = (size)&0xFF;
    wbuffer[9] = (size>>8)&0xFF;
    wbuffer[10] = (size>>16)&0xFF;
    wbuffer[11] = (size>>24)&0xFF;
    libusb_bulk_transfer(dev_handle, 0x01, wbuffer, 20, &actual, 1000);
    if (actual != 20)
        return -1;
    return 0;
}


/**	@brief Tries to open connected USB device and find communication endpoints.
@return Returns 0-Success, other-EndPoints not found or device didn't connect.
*/
int ConnectionFT601::Open(const std::string &serial, int vid, int pid)
{
    libusb_device **devs; //pointer to pointer of device, used to retrieve a list of devices
    int usbDeviceCount = libusb_get_device_list(ctx, &devs);

    if (usbDeviceCount < 0)
        return ReportError(-1, "libusb_get_device_list failed: %s", libusb_strerror(libusb_error(usbDeviceCount)));

    for(int i=0; i<usbDeviceCount; ++i)
    {
        libusb_device_descriptor desc;
        int r = libusb_get_device_descriptor(devs[i], &desc);
        if(r<0) {
            lime::error("failed to get device description");
            continue;
        }
        if (desc.idProduct != pid) continue;
        if (desc.idVendor != vid) continue;
        if(libusb_open(devs[i], &dev_handle) != 0) continue;

        std::string foundSerial;
        if (desc.iSerialNumber > 0)
        {
            char data[255];
            r = libusb_get_string_descriptor_ascii(dev_handle,desc.iSerialNumber,(unsigned char*)data, sizeof(data));
            if(r<0)
                lime::error("failed to get serial number");
            else
                foundSerial = std::string(data, size_t(r));
        }

        if (serial == foundSerial) break; //found it
        libusb_close(dev_handle);
        dev_handle = nullptr;
    }
    libusb_free_device_list(devs, 1);

    if(dev_handle == nullptr)
        return ReportError(ENODEV, "libusb_open failed");

    if(libusb_kernel_driver_active(dev_handle, 1) == 1)   //find out if kernel driver is attached
    {
        lime::debug("Kernel Driver Active");
        if(libusb_detach_kernel_driver(dev_handle, 1) == 0) //detach it
            lime::debug("Kernel Driver Detached!");
    }
    int r = libusb_claim_interface(dev_handle, 0); //claim interface 0 (the first) of device
    if (r < 0)
        return ReportError(-1, "Cannot claim interface - %s", libusb_strerror(libusb_error(r)));

    if ((r = libusb_claim_interface(dev_handle, 1))<0) //claim interface 1 of device
        return ReportError(-1, "Cannot claim interface - %s", libusb_strerror(libusb_error(r)));
    lime::debug("Claimed Interface");

    if (libusb_reset_device(dev_handle)!=0)
        return ReportError(-1, "USB reset failed", libusb_strerror(libusb_error(r)));

    FT_FlushPipe(ctrlRdEp);  //clear ctrl ep rx buffer
    FT_SetStreamPipe(ctrlRdEp,64);
    FT_SetStreamPipe(ctrlWrEp,64);
    isConnected = true;
    return 0;
}

/**	@brief Tries to open connected USB device and find communication endpoints.
@return Returns 0-Success, other-EndPoints not found or device didn't connect.
*/
int ConnectionFT601::Open_fd(const std::string &serial, int fd)
{
    libusb_wrap_sys_device(ctx, (intptr_t)fd, &dev_handle);

    if(dev_handle == nullptr)
        return ReportError(ENODEV, "libusb_open failed");

    if(libusb_kernel_driver_active(dev_handle, 1) == 1)   //find out if kernel driver is attached
    {
        lime::debug("Kernel Driver Active");
        if(libusb_detach_kernel_driver(dev_handle, 1) == 0) //detach it
            lime::debug("Kernel Driver Detached!");
    }
    int r = libusb_claim_interface(dev_handle, 0); //claim interface 0 (the first) of device
    if (r < 0)
        return ReportError(-1, "Cannot claim interface - %s", libusb_strerror(libusb_error(r)));

    if ((r = libusb_claim_interface(dev_handle, 1))<0) //claim interface 1 of device
        return ReportError(-1, "Cannot claim interface - %s", libusb_strerror(libusb_error(r)));
    lime::debug("Claimed Interface");

    if (libusb_reset_device(dev_handle)!=0)
        return ReportError(-1, "USB reset failed", libusb_strerror(libusb_error(r)));

    FT_FlushPipe(ctrlRdEp);  //clear ctrl ep rx buffer
    FT_SetStreamPipe(ctrlRdEp,64);
    FT_SetStreamPipe(ctrlWrEp,64);
    isConnected = true;
    return 0;
}

/**	@brief Closes communication to device.
*/
void ConnectionFT601::Close()
{
    if(dev_handle != 0)
    {
        FT_FlushPipe(streamRdEp);
        FT_FlushPipe(ctrlRdEp);
        libusb_release_interface(dev_handle, 1);
        libusb_close(dev_handle);
        dev_handle = 0;
    }

    isConnected = false;
}

/**	@brief Returns connection status
@return 1-connection open, 0-connection closed.
*/
bool ConnectionFT601::IsOpen()
{
    return isConnected;
}

/**	@brief Sends given data buffer to chip through USB port.
@param buffer data buffer, must not be longer than 64 bytes.
@param length given buffer size.
@param timeout_ms timeout limit for operation in milliseconds
@return number of bytes sent.
*/
int ConnectionFT601::Write(const unsigned char *buffer, const int length, int timeout_ms)
{
    std::lock_guard<std::mutex> lock(mExtraUsbMutex);
    long len = 0;
    if (IsOpen() == false)
        return 0;

    unsigned char* wbuffer = new unsigned char[length];
    memcpy(wbuffer, buffer, length);
    int actual = 0;
    libusb_bulk_transfer(dev_handle, ctrlWrEp, wbuffer, length, &actual, timeout_ms);
    len = actual;
    delete[] wbuffer;
    return len;
}

/**	@brief Reads data coming from the chip through USB port.
@param buffer pointer to array where received data will be copied, array must be
big enough to fit received data.
@param length number of bytes to read from chip.
@param timeout_ms timeout limit for operation in milliseconds
@return number of bytes received.
*/

int ConnectionFT601::Read(unsigned char *buffer, const int length, int timeout_ms)
{
    std::lock_guard<std::mutex> lock(mExtraUsbMutex);
    long len = length;
    if(IsOpen() == false)
        return 0;

    int actual = 0;
    libusb_bulk_transfer(dev_handle, ctrlRdEp, buffer, len, &actual, timeout_ms);
    len = actual;

    return len;
}


/**	@brief Function for handling libusb callbacks
*/
static void callback_libusbtransfer(libusb_transfer *trans)
{
    ConnectionFT601::USBTransferContext *context = reinterpret_cast<ConnectionFT601::USBTransferContext*>(trans->user_data);
    std::unique_lock<std::mutex> lck(context->transferLock);
    switch(trans->status)
    {
        case LIBUSB_TRANSFER_CANCELLED:
            context->bytesXfered = trans->actual_length;
            context->done.store(true);
            break;
        case LIBUSB_TRANSFER_COMPLETED:
            context->bytesXfered = trans->actual_length;
            context->done.store(true);
            break;
        case LIBUSB_TRANSFER_ERROR:
            lime::error("TRANSFER ERROR");
            context->bytesXfered = trans->actual_length;
            context->done.store(true);
            break;
        case LIBUSB_TRANSFER_TIMED_OUT:
            lime::error("USB transfer timed out");
            context->done.store(true);
            break;
        case LIBUSB_TRANSFER_OVERFLOW:
            lime::error("transfer overflow\n");
            break;
        case LIBUSB_TRANSFER_STALL:
            lime::error("transfer stalled");
            break;
        case LIBUSB_TRANSFER_NO_DEVICE:
            lime::error("transfer no device");
            break;
    }
    lck.unlock();
    context->cv.notify_one();
}

int ConnectionFT601::GetBuffersCount() const
{
    return USB_MAX_CONTEXTS;
}

int ConnectionFT601::CheckStreamSize(int size)const
{
    return size;
}

/**
@brief Starts asynchronous data reading from board
@param *buffer buffer where to store received data
@param length number of bytes to read
@return handle of transfer context
*/
int ConnectionFT601::BeginDataReading(char *buffer, uint32_t length, int ep)
{
    int i = 0;
    bool contextFound = false;
    //find not used context
    for(i = 0; i<USB_MAX_CONTEXTS; i++)
    {
        if(!contexts[i].used)
        {
            contextFound = true;
            break;
        }
    }
    if(!contextFound)
    {
        lime::error("No contexts left for reading data");
        return -1;
    }
    contexts[i].used = true;

    libusb_transfer *tr = contexts[i].transfer;
    libusb_fill_bulk_transfer(tr, dev_handle, streamRdEp, (unsigned char*)buffer, length, callback_libusbtransfer, &contexts[i], 0);
    contexts[i].done = false;
    contexts[i].bytesXfered = 0;
    int status = libusb_submit_transfer(tr);
    if(status != 0)
    {
        lime::error("ERROR BEGIN DATA READING %s", libusb_error_name(status));
        contexts[i].used = false;
        return -1;
    }

    return i;
}

/**
@brief Waits for asynchronous data reception
@param contextHandle handle of which context data to wait
@param timeout_ms number of miliseconds to wait
@return true - wait finished, false - still waiting for transfer to complete
*/
bool ConnectionFT601::WaitForReading(int contextHandle, unsigned int timeout_ms)
{
    if(contextHandle >= 0 && contexts[contextHandle].used == true)
    {
        //blocking not to waste CPU
        std::unique_lock<std::mutex> lck(contexts[contextHandle].transferLock);
        return contexts[contextHandle].cv.wait_for(lck, chrono::milliseconds(timeout_ms), [&](){return contexts[contextHandle].done.load();});
    }
    return true;  //there is nothing to wait for (signal wait finished)
}

/**
@brief Finishes asynchronous data reading from board
@param buffer array where to store received data
@param length number of bytes to read
@param contextHandle handle of which context to finish
@return false failure, true number of bytes received
*/
int ConnectionFT601::FinishDataReading(char *buffer, uint32_t length, int contextHandle)
{
    if(contextHandle >= 0 && contexts[contextHandle].used == true)
    {
        length = contexts[contextHandle].bytesXfered;
        contexts[contextHandle].used = false;
        return length;
    }
    return 0;
}

/**
@brief Aborts reading operations
*/
void ConnectionFT601::AbortReading(int ep)
{
    for(int i = 0; i<USB_MAX_CONTEXTS; ++i)
    {
        if(contexts[i].used)
	{
            if (WaitForReading(i, 100))
                FinishDataReading(nullptr, 0, i);
            else
            	libusb_cancel_transfer(contexts[i].transfer);
	}
    }
    for(int i=0; i<USB_MAX_CONTEXTS; ++i)
    {
        if(contexts[i].used)
        {
            WaitForReading(i, 100);
            FinishDataReading(nullptr, 0, i);
        }
    }
}

/**
@brief Starts asynchronous data Sending to board
@param *buffer buffer to send
@param length number of bytes to send
@return handle of transfer context
*/
int ConnectionFT601::BeginDataSending(const char *buffer, uint32_t length, int ep)
{
    int i = 0;
    //find not used context
    bool contextFound = false;
    for(i = 0; i<USB_MAX_CONTEXTS; i++)
    {
        if(!contextsToSend[i].used)
        {
            contextFound = true;
            break;
        }
    }
    if(!contextFound)
        return -1;
    contextsToSend[i].used = true;

    libusb_transfer *tr = contextsToSend[i].transfer;
    contextsToSend[i].done = false;
    contextsToSend[i].bytesXfered = 0;
    libusb_fill_bulk_transfer(tr, dev_handle, streamWrEp, (unsigned char*)buffer, length, callback_libusbtransfer, &contextsToSend[i], 0);
    int status = libusb_submit_transfer(tr);
    if(status != 0)
    {
        lime::error("ERROR BEGIN DATA SENDING %s", libusb_error_name(status));
        contextsToSend[i].used = false;
        return -1;
    }
    return i;
}

/**
@brief Waits for asynchronous data sending
@param contextHandle handle of which context data to wait
@param timeout_ms number of miliseconds to wait
@return true - wait finished, false - still waiting for transfer to complete
*/
bool ConnectionFT601::WaitForSending(int contextHandle, unsigned int timeout_ms)
{
    if(contextHandle >= 0 && contextsToSend[contextHandle].used == true)
    {
        //blocking not to waste CPU
        std::unique_lock<std::mutex> lck(contextsToSend[contextHandle].transferLock);
        return contextsToSend[contextHandle].cv.wait_for(lck, chrono::milliseconds(timeout_ms), [&](){return contextsToSend[contextHandle].done.load();});
    }
    return true; //there is nothing to wait for (signal wait finished)
}

/**
@brief Finishes asynchronous data sending to board
@param buffer array where to store received data
@param length number of bytes to read
@param contextHandle handle of which context to finish
@return false failure, true number of bytes sent
*/
int ConnectionFT601::FinishDataSending(const char *buffer, uint32_t length, int contextHandle)
{
    if(contextHandle >= 0 && contextsToSend[contextHandle].used == true)
    {
        length = contextsToSend[contextHandle].bytesXfered;
        contextsToSend[contextHandle].used = false;
        return length;
    }
    else
        return 0;
}

/**
@brief Aborts sending operations
*/
void ConnectionFT601::AbortSending(int ep)
{
    for(int i = 0; i<USB_MAX_CONTEXTS; ++i)
    {
        if(contextsToSend[i].used)
        {
            if (WaitForSending(i, 100))
                FinishDataSending(nullptr, 0, i);
            else
                libusb_cancel_transfer(contextsToSend[i].transfer);
        }
    }
    for (int i = 0; i<USB_MAX_CONTEXTS; ++i)
    {
        if(contextsToSend[i].used)
        {
            WaitForSending(i, 100);
            FinishDataSending(nullptr, 0, i);
        }
    }
}

int ConnectionFT601::ResetStreamBuffers()
{
    if (FT_FlushPipe(streamWrEp)!=0)
        return -1;
    if (FT_FlushPipe(streamRdEp)!=0)
        return -1;
    if (FT_SetStreamPipe(streamWrEp,sizeof(FPGA_DataPacket))!=0)
        return -1;
    if (FT_SetStreamPipe(streamRdEp,sizeof(FPGA_DataPacket))!=0)
        return -1;
    return 0;
}

int ConnectionFT601::ProgramWrite(const char *data_src, size_t length, int prog_mode, int device, ProgrammingCallback callback)
{
    int ret;

    if (device != LMS64CProtocol::FPGA)
    {
        lime::error("Unsupported programming target");
        return -1;
    }
    if (prog_mode == 0)
    {
        lime::error("Programming to RAM is not supported");
        return -1;
    }

    if (prog_mode == 2)
        return LMS64CProtocol::ProgramWrite(data_src, length, prog_mode, device, callback);

    FPGAinfo fpgainfo = GetFPGAInfo();

    if (fpgainfo.hwVersion < 3)
    {
        // LimeSDR-Mini v1.X
        if (fpgainfo.gatewareVersion != 0)
        {
            LMS64CProtocol::ProgramWrite(nullptr, 0, 2, 2, nullptr);
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
        }
        const int sizeUFM = 0x8000;
        const int sizeCFM0 = 0x42000;
        const int startUFM = 0x1000;
        const int startCFM0 = 0x4B000;

        if (length != startCFM0 + sizeCFM0)
        {
            lime::error("Invalid image file");
            return -1;
        }
        std::vector<char> buffer(sizeUFM + sizeCFM0);
        memcpy(buffer.data(), data_src + startUFM, sizeUFM);
        memcpy(buffer.data() + sizeUFM, data_src + startCFM0, sizeCFM0);

        ret = LMS64CProtocol::ProgramWrite(buffer.data(), buffer.size(), prog_mode, device, callback);
        LMS64CProtocol::ProgramWrite(nullptr, 0, 2, 2, nullptr);
    }
    else
    {
        // LimeSDR-Mini v2.X
        ret = LMS64CProtocol::ProgramWrite(data_src, length, prog_mode, device, callback);
        LMS64CProtocol::ProgramWrite(nullptr, 0, 2, 2, nullptr);
    }

    return ret;
}

DeviceInfo ConnectionFT601::GetDeviceInfo(void)
{
    DeviceInfo info = LMS64CProtocol::GetDeviceInfo();
    info.boardSerialNumber = mSerial;
    return info;
}

int ConnectionFT601::GPIOWrite(const uint8_t *buffer, size_t len)
{
    if ((!buffer)||(len==0))
        return -1;
    const uint32_t addr = 0xC6;
    const uint32_t value = (len == 1) ? buffer[0] : buffer[0] | (buffer[1]<<8);
    return WriteRegisters(&addr, &value, 1);
}

int ConnectionFT601::GPIORead(uint8_t *buffer, size_t len)
{
    if ((!buffer)||(len==0))
        return -1;
    const uint32_t addr = 0xC2;
    uint32_t value;
    int ret = ReadRegisters(&addr, &value, 1);
    buffer[0] = value;
    if (len > 1)
        buffer[1] = (value >> 8);
    return ret;
}

int ConnectionFT601::GPIODirWrite(const uint8_t *buffer, size_t len )
{
    if ((!buffer)||(len==0))
        return -1;
    const uint32_t addr = 0xC4;
    const uint32_t value = (len == 1) ? buffer[0] : buffer[0] | (buffer[1]<<8);
    return WriteRegisters(&addr, &value, 1);
}

int ConnectionFT601::GPIODirRead(uint8_t *buffer, size_t len)
{
    if ((!buffer)||(len==0))
        return -1;
    const uint32_t addr = 0xC4;
    uint32_t value;
    int ret = ReadRegisters(&addr, &value, 1);
    buffer[0] = value;
    if (len > 1)
        buffer[1] = (value >> 8);
    return ret;
}
