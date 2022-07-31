#pragma once

#include <cstdint>
#include <functional>
#include <assert.h>
#include <cstring>
#include <limits>

/*
 * Bit types
 */
typedef uintptr_t	Bitu;
typedef intptr_t	Bits;
typedef uint32_t	Bit32u;
typedef int32_t		Bit32s;
typedef uint16_t	Bit16u;
typedef int16_t		Bit16s;
typedef uint8_t		Bit8u;
typedef int8_t		Bit8s;

/*
 * utility
 */
// Signed-only integer division with ceiling
template<typename T1, typename T2>
inline constexpr T1 ceil_sdivide(const T1 x, const T2 y) noexcept {
        static_assert(std::is_signed<T1>::value, "First parameter should be signed");
        static_assert(std::is_signed<T2>::value, "Second parameter should be signed.");
        return x / y + (((x < 0) ^ (y > 0)) && (x % y));
        // https://stackoverflow.com/a/33790603
}

// Unsigned-only integer division with ceiling
template<typename T1, typename T2>
inline constexpr T1 ceil_udivide(const T1 x, const T2 y) noexcept {
        static_assert(std::is_unsigned<T1>::value, "First parameter should be unsigned");
        static_assert(std::is_unsigned<T2>::value, "Second parameter should be unsigned");
        return (x != 0) ? 1 + ((x - 1) / y) : 0;
        // https://stackoverflow.com/a/2745086
}

// Clamp: given a value that can be compared with the given minimum and maximum
//        values, this function will:
//          * return the value if it's in-between or equal to either bounds, or
//          * return either bound depending on which bound the value is beyond
template <class T> T clamp(const T& n, const T& lower, const T& upper) {
        return std::max<T>(lower, std::min<T>(n, upper));
}

// Raspberry Pi is little endian, no need to swap
constexpr uint16_t le16_to_host(const uint16_t x)
{
	return x;
}

// Read a uint16 from unaligned 8-bit byte-ordered memory.
static inline uint16_t read_unaligned_uint16(const uint8_t *arr) noexcept
{
	uint16_t val;
	memcpy(&val, arr, sizeof(val));
	return val;
}

// Read a 16-bit word from 8-bit DOS/little-endian byte-ordered memory.
static inline uint16_t host_readw(const uint8_t *arr) noexcept
{
	return le16_to_host(read_unaligned_uint16(arr));
}

/*
 * logging
 */
#ifdef CIRCLE
#define LOG_MSG(...) m_Logger.Write("Gus", LogNotice, __VA_ARGS__);
#define DEBUG_LOG_MSG(...) m_Logger.Write("Gus", LogDebug, __VA_ARGS__);
#else
#define LOG_MSG(msg, ...) printf(msg "\n", ##__VA_ARGS__);
#define DEBUG_LOG_MSG(msg, ...) printf(msg "\n", ##__VA_ARGS__);
#endif


/*
 * audio classes
 */
struct AudioFrame {
        float left = 0;
        float right = 0;
};
#define MAX_AUDIO ((1<<(16-1))-1)
#define MIN_AUDIO -(1<<(16-1))


/*
 * io classes -- not using for now
 */
using io_port_t = uint16_t; // DOS only supports 16-bit port addresses
using io_val_t = uint32_t; // Handling exists up to a dword (or less)
enum class io_width_t : uint8_t {
    byte = 1, // bytes
    word = 2, // bytes
    dword = 4, // bytes
};
/*
using io_read_f = std::function<io_val_t(io_port_t port, io_width_t width)>;
using io_write_f = std::function<void(io_port_t port, io_val_t val, io_width_t width)>;

class IO_Base{
protected:
        bool installed = false;
        io_port_t m_port = 0u;
        io_width_t m_width = io_width_t::byte;
        io_port_t m_range = 0u;
};

class IO_ReadHandleObject: private IO_Base{
public:
        void Install(io_port_t port,
                     io_read_f handler,
                     io_width_t max_width,
                     io_port_t range = 1);

        void Uninstall();
        ~IO_ReadHandleObject();
};
class IO_WriteHandleObject: private IO_Base{
public:
        void Install(io_port_t port,
                     io_write_f handler,
                     io_width_t max_width,
                     io_port_t range = 1);

        void Uninstall();
        ~IO_WriteHandleObject();
};
*/

/*
 * silly math
 */
#ifdef CIRCLE
#define M_PI   3.14159265358979323846264338327950288
#endif

/*
 * casting
 */

// Select the next larger signed integer type
template <typename T>
using next_int_t = typename std::conditional<
        sizeof(T) == sizeof(int8_t),
        int16_t,
        typename std::conditional<sizeof(T) == sizeof(int16_t), int32_t, int64_t>::type>::type;

template <typename cast_t, typename check_t>
constexpr cast_t check_cast(const check_t in)
{
	// Ensure the two types are integers, can't handle floats/doubles
	static_assert(std::numeric_limits<cast_t>::is_integer,
	              "The casting type must be an integer type");
	static_assert(std::numeric_limits<check_t>::is_integer,
	              "The argument must be an integer type");

	// ensure the inbound value is within the limits of the casting type
	assert(static_cast<next_int_t<check_t>>(in) >=
	       static_cast<next_int_t<cast_t>>(std::numeric_limits<cast_t>::min()));
	assert(static_cast<next_int_t<check_t>>(in) <=
	       static_cast<next_int_t<cast_t>>(std::numeric_limits<cast_t>::max()));

	return static_cast<cast_t>(in);
}

/*
 * assert
 */
// Include a message in assert, similar to static_assert:
#define assertm(exp, msg) assert(((void)msg, exp))
