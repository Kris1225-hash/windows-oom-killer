#include "oom_policy.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    const OOM_THRESHOLDS thresholds = {512, 3, 2, 3};
    const OOM_SAMPLE healthy = {1000, 2000};
    const OOM_SAMPLE commit_exhausted_with_reclaimed_ram = {1900, 2000};
    const OOM_SAMPLE critical = {1900, 2000};
    const OOM_SAMPLE overcommitted = {2100, 2000};
    OOM_POLICY_STATE state = {0};

    assert(!OomSampleIsCritical(&thresholds, &healthy));
    assert(OomSampleIsCritical(&thresholds, &commit_exhausted_with_reclaimed_ram));
    assert(OomSampleIsCritical(&thresholds, &critical));
    assert(OomSampleIsCritical(&thresholds, &overcommitted));
    assert(OomPolicyObserve(&state, &thresholds, &critical) == OOM_ACTION_NONE);
    assert(OomPolicyObserve(&state, &thresholds, &healthy) == OOM_ACTION_NONE);
    assert(OomPolicyObserve(&state, &thresholds, &critical) == OOM_ACTION_NONE);
    assert(OomPolicyObserve(&state, &thresholds, &critical) == OOM_ACTION_NONE);
    assert(OomPolicyObserve(&state, &thresholds, &critical) == OOM_ACTION_KILL);

    OomPolicyKillIssued(&state);
    assert(OomPolicyObserve(&state, &thresholds, &critical) == OOM_ACTION_NONE);
    assert(OomPolicyObserve(&state, &thresholds, &critical) == OOM_ACTION_KILL);
    OomPolicyKillIssued(&state);
    assert(OomPolicyObserve(&state, &thresholds, &critical) == OOM_ACTION_NONE);
    assert(OomPolicyObserve(&state, &thresholds, &critical) == OOM_ACTION_KILL);
    OomPolicyKillIssued(&state);
    assert(OomPolicyObserve(&state, &thresholds, &critical) == OOM_ACTION_NONE);
    assert(OomPolicyObserve(&state, &thresholds, &critical) == OOM_ACTION_ESCALATE);
    assert(OomPolicyObserve(&state, &thresholds, &healthy) == OOM_ACTION_NONE);
    assert(!state.kill_pending && !state.critical_samples && !state.kills_issued);

    assert(OomWatchdogObserve(0, 1, 0, 10, 0, 0, 5) == OOM_WATCHDOG_NONE);
    assert(OomWatchdogObserve(1, 0, 0, 10, 0, 0, 5) == OOM_WATCHDOG_NONE);
    assert(OomWatchdogObserve(1, 1, 0, 4, 0, 0, 5) == OOM_WATCHDOG_NONE);
    assert(OomWatchdogObserve(1, 1, 0, 5, 0, 0, 5) == OOM_WATCHDOG_MONITOR_TIMEOUT);
    assert(OomWatchdogObserve(1, 1, 1, 5, 0, 10, 5) == OOM_WATCHDOG_NONE);
    assert(OomWatchdogObserve(1, 1, 1, 10, 0, 10, 5) == OOM_WATCHDOG_KILL_TIMEOUT);

    puts("oom policy tests passed");
    return 0;
}
