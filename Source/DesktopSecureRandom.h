#pragma once

#include <cstddef>

namespace ChurchDAWDesktopLicensing
{
// Usa exclusivamente o CSPRNG fornecido pelo sistema operacional.
bool fillSecureRandom (void* destination, size_t bytes) noexcept;
}
