#include "DesktopSecureRandom.h"

#if defined(__APPLE__)

#import <Security/Security.h>

namespace ChurchDAWDesktopLicensing
{
bool fillSecureRandom (void* destination, size_t bytes) noexcept
{
    return destination != nullptr
        && bytes > 0
        && SecRandomCopyBytes (
               kSecRandomDefault,
               bytes,
               static_cast<uint8_t*> (destination)) == errSecSuccess;
}
}

#endif
