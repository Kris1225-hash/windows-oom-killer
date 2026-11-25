#include "oom_policy.h"

#if defined(_WIN32)
#include <windows.h>
#include <winioctl.h>
#endif

#include "oom_protocol.h"

#include <stddef.h>
#include <stdio.h>

/* The host build includes the real wire header; pin every field the driver,
 * service, and bugcheck text depend on so a reorder cannot pass G2 with an
 * unchanged total size. driver.c has the matching FIELD_OFFSET C_ASSERTs. */
_Static_assert(sizeof(OOM_TELEMETRY) == 64, "OOM_TELEMETRY size changed");
_Static_assert(offsetof(OOM_TELEMETRY, sequence) == 8, "OOM_TELEMETRY.sequence moved");
_Static_assert(offsetof(OOM_TELEMETRY, service_pid) == 16, "OOM_TELEMETRY.service_pid moved");
_Static_assert(offsetof(OOM_TELEMETRY, page_size) == 20, "OOM_TELEMETRY.page_size moved");
_Static_assert(offsetof(OOM_TELEMETRY, backing_reason) == 24,
               "OOM_TELEMETRY.backing_reason moved");
_Static_assert(offsetof(OOM_TELEMETRY, pagefile_growth_pages) == 28,
               "OOM_TELEMETRY.pagefile_growth_pages moved");
_Static_assert(offsetof(OOM_TELEMETRY, commit_charge_pages) == 32,
               "OOM_TELEMETRY.commit_charge_pages moved");
_Static_assert(offsetof(OOM_TELEMETRY, commit_limit_pages) == 40,
               "OOM_TELEMETRY.commit_limit_pages moved");
_Static_assert(offsetof(OOM_TELEMETRY, available_physical_pages) == 48,
               "OOM_TELEMETRY.available_physical_pages moved");
_Static_assert(offsetof(OOM_TELEMETRY, pagefile_free_pages) == 56,
               "OOM_TELEMETRY.pagefile_free_pages moved");
_Static_assert(sizeof(OOM_KILL_REQUEST) == 88, "OOM_KILL_REQUEST size changed");
_Static_assert(offsetof(OOM_KILL_REQUEST, victim_pid) == 64, "OOM_KILL_REQUEST.victim_pid moved");
_Static_assert(offsetof(OOM_KILL_REQUEST, reserved) == 68, "OOM_KILL_REQUEST.reserved moved");
_Static_assert(offsetof(OOM_KILL_REQUEST, victim_private_pages) == 72,
               "OOM_KILL_REQUEST.victim_private_pages moved");
_Static_assert(offsetof(OOM_KILL_REQUEST, victim_create_time) == 80,
               "OOM_KILL_REQUEST.victim_create_time moved");
_Static_assert(sizeof(OOM_DRIVER_STATUS) == 32, "OOM_DRIVER_STATUS size changed");
_Static_assert(offsetof(OOM_DRIVER_STATUS, last_sequence) == 16,
               "OOM_DRIVER_STATUS.last_sequence moved");
_Static_assert(offsetof(OOM_DRIVER_STATUS, owner_pid) == 24, "OOM_DRIVER_STATUS.owner_pid moved");
_Static_assert(offsetof(OOM_DRIVER_STATUS, reserved) == 28, "OOM_DRIVER_STATUS.reserved moved");

/* Not assert(): these checks must survive an NDEBUG build, and the policy
 * calls inside them have side effects that must run exactly once. */
static int failures;

static void check(int ok, const char *expression, int line)
{
    if (!ok) {
        fprintf(stderr, "test_policy.c:%d: check failed: %s\n", line, expression);
        ++failures;
    }
}

#define CHECK(expression) check((expression) != 0, #expression, __LINE__)

int main(void)
{
    const OOM_THRESHOLDS thresholds = {512, 3, 2, 3};
    const OOM_SAMPLE healthy = {1000, 2000, 0};
    const OOM_SAMPLE commit_exhausted_with_reclaimed_ram = {1900, 2000, 0};
    const OOM_SAMPLE critical = {1900, 2000, 0};
    const OOM_SAMPLE overcommitted = {2100, 2000, 0};
    const OOM_SAMPLE at_threshold = {1488, 2000, 0};
    const OOM_SAMPLE above_threshold = {1487, 2000, 0};
    /* a system-managed pagefile: near the current limit, but it can still grow */
    const OOM_SAMPLE growable = {1900, 2000, 1000};
    const OOM_SAMPLE growable_at_threshold = {1900, 2000, 412};
    const OOM_SAMPLE growable_above_threshold = {1900, 2000, 413};
    const OOM_SAMPLE growth_overcommitted = {2100, 2000, 50};
    const OOM_SAMPLE growth_saturates = {1, ~(OOM_U64)0 - 10, 100};
    OOM_POLICY_STATE state = {0};

    CHECK(!OomSampleIsCritical(&thresholds, &healthy));
    CHECK(OomSampleIsCritical(&thresholds, &commit_exhausted_with_reclaimed_ram));
    CHECK(OomSampleIsCritical(&thresholds, &critical));
    CHECK(OomSampleIsCritical(&thresholds, &overcommitted));
    CHECK(OomSampleIsCritical(&thresholds, &at_threshold));
    CHECK(!OomSampleIsCritical(&thresholds, &above_threshold));
    CHECK(!OomSampleIsCritical(&thresholds, &growable));
    CHECK(OomSampleIsCritical(&thresholds, &growable_at_threshold));
    CHECK(!OomSampleIsCritical(&thresholds, &growable_above_threshold));
    CHECK(OomSampleIsCritical(&thresholds, &growth_overcommitted));
    CHECK(OomCommitHeadroom(&growable) == 1100);
    CHECK(OomCommitHeadroom(&growth_overcommitted) == 0);
    CHECK(OomCommitHeadroom(&growth_saturates) == ~(OOM_U64)0 - 1);
    /* growth that clears the band is no pressure episode at all */
    {
        OOM_POLICY_STATE growing = {0};
        int i;
        for (i = 0; i < 10; ++i)
            CHECK(OomPolicyObserve(&growing, &thresholds, &growable) == OOM_ACTION_NONE);
        CHECK(!growing.critical_samples && !growing.kills_issued);
    }
    CHECK(OomPolicyObserve(&state, &thresholds, &critical) == OOM_ACTION_NONE);
    CHECK(OomPolicyObserve(&state, &thresholds, &healthy) == OOM_ACTION_NONE);
    CHECK(OomPolicyObserve(&state, &thresholds, &critical) == OOM_ACTION_NONE);
    CHECK(OomPolicyObserve(&state, &thresholds, &critical) == OOM_ACTION_NONE);
    CHECK(OomPolicyObserve(&state, &thresholds, &critical) == OOM_ACTION_KILL);

    OomPolicyKillIssued(&state);
    CHECK(OomPolicyObserve(&state, &thresholds, &critical) == OOM_ACTION_NONE);
    CHECK(OomPolicyObserve(&state, &thresholds, &critical) == OOM_ACTION_KILL);
    OomPolicyKillIssued(&state);
    CHECK(OomPolicyObserve(&state, &thresholds, &critical) == OOM_ACTION_NONE);
    CHECK(OomPolicyObserve(&state, &thresholds, &critical) == OOM_ACTION_KILL);
    OomPolicyKillIssued(&state);
    CHECK(OomPolicyObserve(&state, &thresholds, &critical) == OOM_ACTION_NONE);
    CHECK(OomPolicyObserve(&state, &thresholds, &critical) == OOM_ACTION_ESCALATE);

    /* an escalation the driver declined must never turn back into more kills */
    CHECK(OomPolicyObserve(&state, &thresholds, &critical) == OOM_ACTION_ESCALATE);
    CHECK(OomPolicyObserve(&state, &thresholds, &critical) == OOM_ACTION_ESCALATE);
    CHECK(OomPolicyObserve(&state, &thresholds, &critical) == OOM_ACTION_ESCALATE);
    CHECK(OomPolicyObserve(&state, &thresholds, &critical) == OOM_ACTION_ESCALATE);
    CHECK(state.kills_issued == thresholds.max_kills);

    CHECK(OomPolicyObserve(&state, &thresholds, &healthy) == OOM_ACTION_NONE);
    CHECK(!state.kill_pending && !state.critical_samples && !state.kills_issued);

    /* a new pressure episode gets a fresh kill budget */
    CHECK(OomPolicyObserve(&state, &thresholds, &critical) == OOM_ACTION_NONE);
    CHECK(OomPolicyObserve(&state, &thresholds, &critical) == OOM_ACTION_NONE);
    CHECK(OomPolicyObserve(&state, &thresholds, &critical) == OOM_ACTION_KILL);

    CHECK(OomWatchdogObserve(0, 1, 0, 10, 0, 0, 5) == OOM_WATCHDOG_NONE);
    CHECK(OomWatchdogObserve(1, 0, 0, 10, 0, 0, 5) == OOM_WATCHDOG_NONE);
    CHECK(OomWatchdogObserve(1, 1, 0, 4, 0, 0, 5) == OOM_WATCHDOG_NONE);
    CHECK(OomWatchdogObserve(1, 1, 0, 5, 0, 0, 5) == OOM_WATCHDOG_MONITOR_TIMEOUT);
    CHECK(OomWatchdogObserve(1, 1, 1, 5, 0, 10, 5) == OOM_WATCHDOG_NONE);
    CHECK(OomWatchdogObserve(1, 1, 1, 10, 0, 10, 5) == OOM_WATCHDOG_KILL_TIMEOUT);
    CHECK(OomWatchdogObserve(1, 1, 0, 3, 7, 0, 5) == OOM_WATCHDOG_NONE);

    if (failures) {
        fprintf(stderr, "%d oom policy check(s) failed\n", failures);
        return 1;
    }
    puts("oom policy tests passed");
    return 0;
}
