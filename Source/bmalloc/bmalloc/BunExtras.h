#pragma once

#include <bit>

#if defined(__cpp_lib_bit_cast) && __cpp_lib_bit_cast >= 201806L
#define __bit_cast std::bit_cast
#else
template <
    typename Dest, typename Source,
    typename std::enable_if<sizeof(Dest) == sizeof(Source) &&
                                std::is_trivially_copyable<Source>::value &&
                                std::is_trivially_copyable<Dest>::value,
                            int>::type = 0>
inline constexpr Dest __bit_cast(const Source &source) {
  return __builtin_bit_cast(Dest, source);
}
#endif