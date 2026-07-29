#include "DesktopSecureRandom.h"

#if defined(_WIN32)

#ifndef NOMINMAX
 #define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <limits>

namespace ChurchDAWDesktopLicensing
{
bool fillSecureRandom (void* destination, size_t bytes) noexcept
{
    if (destination == nullptr || bytes == 0
        || bytes > static_cast<size_t> (std::numeric_limits<ULONG>::max()))
        return false;

    return BCryptGenRandom (
               nullptr,
               static_cast<PUCHAR> (destination),
               static_cast<ULONG> (bytes),
               BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}
}

#endif
