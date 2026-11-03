#pragma once

namespace ChurchDAWDistribution
{
    // Presentation metadata only. These categories never authorize a download
    // or replace signature/hash validation and the existing success booleans.
    enum class Failure { none, connection, httpResponse, validation };

    constexpr Failure failedResponse (bool transportSucceeded, int statusCode) noexcept
    {
        return ! transportSucceeded ? Failure::connection
             : statusCode >= 200 && statusCode < 300 ? Failure::validation
                                                    : Failure::httpResponse;
    }

    constexpr Failure retainFailure (Failure previous, Failure current) noexcept
    {
        // A failed fallback connection must not erase a real validation error
        // already received from the primary endpoint.
        return static_cast<int> (previous) > static_cast<int> (current) ? previous : current;
    }
}
