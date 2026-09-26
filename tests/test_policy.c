#include "oom_policy.h"
#include "oom_protocol.h"

#include <stddef.h>
#include <stdio.h>

/* The wire structs are the driver protocol: pin their real layout here, not a
 * hand-written copy of it. */
_Static_assert(sizeof(OOM_TELEMETRY) == 64, "OOM_TELEMETRY size");
_Static_assert(offsetof(OOM_TELEMETRY, sequence) == 8, "OOM_TELEMETRY.sequence");
_Static_assert(offsetof(OOM_TELEMETRY, commit_charge_pages) == 32, "OOM_TELEMETRY.commit_charge_pages");
_Static_assert(offsetof(OOM_TELEMETRY, pagefile_free_pages) == 56, "OOM_TELEMETRY.pagefile_free_pages");
_Static_assert(sizeof(OOM_KILL_REQUEST) == 88, "OOM_KILL_REQUEST size");
_Static_assert(offsetof(OOM_KILL_REQUEST, victim_pid) == 64, "OOM_KILL_REQUEST.victim_pid");
_Static_assert(offsetof(OOM_KILL_REQUEST, victim_create_time) == 80, "OOM_KILL_REQUEST.victim_create_time");
_Static_assert(sizeof(OOM_DRIVER_STATUS) == 32, "OOM_DRIVER_STATUS size");

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
    const OOM_SAMPLE healthy = {1000, 2000};
    const OOM_SAMPLE commit_exhausted_with_reclaimed_ram = {1900, 2000};
    const OOM_SAMPLE critical = {1900, 2000};
    const OOM_SAMPLE overcommitted = {2100, 2000};
    const OOM_SAMPLE at_threshold = {1488, 2000};
    const OOM_SAMPLE above_threshold = {1487, 2000};
    OOM_POLICY_STATE state = {0};

    CHECK(!OomSampleIsCritical(&thresholds, &healthy));
    CHECK(OomSampleIsCritical(&thresholds, &commit_exhausted_with_reclaimed_ram));
    CHECK(OomSampleIsCritical(&thresholds, &critical));
    CHECK(OomSampleIsCritical(&thresholds, &overcommitted));
    CHECK(OomSampleIsCritical(&thresholds, &at_threshold));
    CHECK(!OomSampleIsCritical(&thresholds, &above_threshold));
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
