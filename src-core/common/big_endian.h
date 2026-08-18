#pragma once

/**
 * @file big_endian.h
 * @brief Big endian integer structs
 */

#include <cstdint>
#if defined(_WIN32)
#include <WS2tcpip.h>
#include <winsock2.h>
#else
#include <arpa/inet.h> // for ntohs() etc.
#endif
#include <stdint.h>

#ifdef _WIN32
#pragma pack(push, 1)
#endif
/**
 * @brief Creates an unsigned 2-byte integer from big-endian bytes
 *
 */
class be_uint16_t
{
public:
    be_uint16_t() : be_val_(0) {}
    // Transparently cast from uint16_t
    be_uint16_t(const uint16_t &val) : be_val_(htons(val)) {}
    // Transparently cast to uint16_t
    operator uint16_t() const { return ntohs(be_val_); }

private:
    uint16_t be_val_;
}
#ifdef _WIN32
;
#else
__attribute__((packed));
#endif
#ifdef _WIN32
#pragma pack(pop)
#endif
///////////////////////////////////
#ifdef _WIN32
#pragma pack(push, 1)
#endif
/**
 * @brief Creates an unsigned 4-byte integer from big-endian bytes
 *
 */
class be_uint32_t
{
public:
    be_uint32_t() : be_val_(0) {}
    // Transparently cast from uint32_t
    be_uint32_t(const uint32_t &val) : be_val_(htonl(val)) {}
    // Transparently cast to uint32_t
    operator uint32_t() const { return ntohl(be_val_); }

private:
    uint32_t be_val_;
}
#ifdef _WIN32
;
#else
__attribute__((packed));
#endif
#ifdef _WIN32
#pragma pack(pop)
#endif
///////////////////////////////////
#ifdef _WIN32
#pragma pack(push, 1)
#endif
/**
 * @brief Takes 6 bytes, creates an unsigned 48-bit integer in big endian. Stored in a 64 bit signed integer
 *
 */
struct be_uint48_t
{
    uint8_t bytes[6];

    uint64_t value() const
    {
        return ((uint64_t)bytes[0] << 40) | ((uint64_t)bytes[1] << 32) | ((uint64_t)bytes[2] << 24) | ((uint64_t)bytes[3] << 16) | ((uint64_t)bytes[4] << 8) | ((uint64_t)bytes[5]);
    }

    operator uint64_t() const { return value(); }
}

#ifdef _WIN32
;
#else
__attribute__((packed));
#endif
#ifdef _WIN32
#pragma pack(pop)
#endif
//////////////////////////////
#ifdef _WIN32
#pragma pack(push, 1)
#endif
/**
 * @brief Creates an unsigned 8-byte integer from big-endian bytes
 *
 */
class be_uint64_t
{
public:
    be_uint64_t() : be_val_(0) {}
    // Was htonl/ntohl (32-bit), which destroyed the upper 4 bytes of every value
    be_uint64_t(const uint64_t &val) : be_val_(swap64(val)) {}
    operator uint64_t() const { return swap64(be_val_); }

private:
    static uint64_t swap64(uint64_t v)
    {
        if (htonl(1) == 1) // already big endian
            return v;
        return ((uint64_t)ntohl((uint32_t)(v & 0xFFFFFFFF)) << 32) | ntohl((uint32_t)(v >> 32));
    }

    uint64_t be_val_;
}
#ifdef _WIN32
;
#else
__attribute__((packed));
#endif
#ifdef _WIN32
#pragma pack(pop)
#endif

static_assert(sizeof(be_uint16_t) == 2, "be_uint16_t must be exactly 2 bytes");
static_assert(sizeof(be_uint32_t) == 4, "be_uint32_t must be exactly 4 bytes");
static_assert(sizeof(be_uint48_t) == 6, "be_uint48_t must be exactly 6 bytes");
static_assert(sizeof(be_uint64_t) == 8, "be_uint64_t must be exactly 8 bytes");