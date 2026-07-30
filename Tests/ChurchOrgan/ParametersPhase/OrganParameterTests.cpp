#include "OrganParameters.h"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <set>
#include <string>

namespace
{
    std::atomic<std::uint64_t> allocationCount { 0 };
    std::size_t checks = 0;

    void require (bool condition, const char* message)
    {
        ++checks;

        if (! condition)
        {
            std::cerr << "FAIL: " << message << '\n';
            std::exit (1);
        }
    }

    bool approximatelyEqual (
        float a,
        float b,
        float tolerance = 1.0e-5f)
    {
        return std::abs (a - b) <= tolerance;
    }
}

void* operator new (std::size_t size)
{
    allocationCount.fetch_add (1, std::memory_order_relaxed);

    if (auto* result = std::malloc (size))
        return result;

    throw std::bad_alloc();
}

void* operator new[] (std::size_t size)
{
    return ::operator new (size);
}

void operator delete (void* memory) noexcept
{
    std::free (memory);
}

void operator delete[] (void* memory) noexcept
{
    std::free (memory);
}

void operator delete (void* memory, std::size_t) noexcept
{
    std::free (memory);
}

void operator delete[] (void* memory, std::size_t) noexcept
{
    std::free (memory);
}

int main()
{
    using namespace churchorgan;

    const auto& catalog = organParameterCatalog();
    require (catalog.size() == organParameterCount, "catalog size");

    std::set<std::string> stableIds;

    for (std::size_t i = 0; i < catalog.size(); ++i)
    {
        const auto& d = catalog[i];
        require (static_cast<std::size_t> (d.id) == i, "catalog order");
        require (d.stableId != nullptr && d.stableId[0] != '\0',
                 "stable id exists");
        require (stableIds.emplace (d.stableId).second,
                 "stable ids are unique");
        require (std::isfinite (d.minimum), "finite minimum");
        require (std::isfinite (d.maximum), "finite maximum");
        require (std::isfinite (d.defaultValue), "finite default");
        require (d.maximum >= d.minimum, "valid range");
        require (d.defaultValue >= d.minimum
                     && d.defaultValue <= d.maximum,
                 "default inside range");

        const auto id = static_cast<OrganParameterId> (i);
        require (approximatelyEqual (
                     sanitiseOrganParameter (
                         id, std::numeric_limits<float>::quiet_NaN()),
                     d.defaultValue),
                 "NaN becomes default");
        require (approximatelyEqual (
                     sanitiseOrganParameter (id, d.minimum - 100000.0f),
                     d.minimum),
                 "lower clamp");
        require (approximatelyEqual (
                     sanitiseOrganParameter (id, d.maximum + 100000.0f),
                     d.maximum),
                 "upper clamp");

        for (int step = 0; step <= 100; ++step)
        {
            const auto normalised = static_cast<float> (step) / 100.0f;
            const auto plain = organParameterFromNormalised (id, normalised);
            const auto restored = organParameterToNormalised (id, plain);
            require (std::isfinite (plain), "finite plain value");
            require (restored >= 0.0f && restored <= 1.0f,
                     "normalised range");

            if (d.kind == OrganParameterKind::continuous)
                require (approximatelyEqual (
                             restored, normalised, 2.0e-5f),
                         "continuous round trip");
        }
    }

    require (
        approximatelyEqual (
            sanitiseOrganParameter (
                OrganParameterId::rotaryMode, 2.6f),
            3.0f),
        "choice snaps to step");
    require (
        approximatelyEqual (
            sanitiseOrganParameter (
                OrganParameterId::upper8, 5.375f),
            5.375f),
        "drawbar automation remains continuous");

    OrganParameterStore store;

    for (const auto& d : catalog)
        require (
            approximatelyEqual (store.getPlain (d.id), d.defaultValue),
            "store starts at default");

    const auto defaultRevision = store.revision();
    require (defaultRevision > 0U, "default reset publishes revision");
    store.setPlain (OrganParameterId::drive, 0.73f);
    const auto changedRevision = store.revision();
    require (
        changedRevision != defaultRevision,
        "changed value advances revision");
    store.setPlain (OrganParameterId::drive, 0.73f);
    require (
        store.revision() == changedRevision,
        "identical value does not churn revision");
    store.setNormalised (OrganParameterId::rotaryMode, 0.74f);
    const auto initialSnapshot = store.snapshot();
    require (approximatelyEqual (
                 initialSnapshot.get (OrganParameterId::drive), 0.73f),
             "plain write");
    require (approximatelyEqual (
                 initialSnapshot.get (OrganParameterId::rotaryMode), 2.0f),
             "normalised choice write");

    const auto allocationsBefore =
        allocationCount.load (std::memory_order_relaxed);

    float checksum = 0.0f;
    for (int pass = 0; pass < 20000; ++pass)
    {
        const auto value = static_cast<float> (pass % 1000) / 999.0f;
        store.setNormalised (OrganParameterId::expression, value);
        store.setPlain (
            OrganParameterId::scannerMode,
            static_cast<float> (pass % 7));

        const auto snapshot = store.snapshot();
        checksum += snapshot.get (OrganParameterId::expression);
        checksum += snapshot.get (OrganParameterId::scannerMode);
    }

    const auto allocationsAfter =
        allocationCount.load (std::memory_order_relaxed);

    require (allocationsAfter == allocationsBefore,
             "parameter bridge allocates nothing");
    require (std::isfinite (checksum) && checksum > 0.0f,
             "snapshot checksum");

    store.resetToDefaults();
    require (
        approximatelyEqual (
            store.getPlain (OrganParameterId::expression),
            descriptor (OrganParameterId::expression).defaultValue),
        "reset restores defaults");

    std::cout << "CHURCH_ORGAN_PARAMETER_TESTS_OK checks="
              << checks
              << " allocations="
              << (allocationsAfter - allocationsBefore)
              << '\n';
    return 0;
}
