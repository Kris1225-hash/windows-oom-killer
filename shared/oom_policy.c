#include "oom_policy.h"

/* Commit the system can still grant: up to the current limit, plus what the
 * pagefiles can still grow by. A system-managed pagefile keeps the current
 * limit only a few hundred MiB above the charge and grows on demand, so the
 * current limit alone is not exhaustion. */
OOM_U64 OomCommitHeadroom(const OOM_SAMPLE *sample)
{
    OOM_U64 maximum = sample->commit_limit + sample->commit_growth;

    if (maximum < sample->commit_limit) /* saturate instead of wrapping */
        maximum = ~(OOM_U64)0;
    return sample->commit_charge < maximum ? maximum - sample->commit_charge : 0;
}

int OomSampleIsCritical(const OOM_THRESHOLDS *thresholds, const OOM_SAMPLE *sample)
{
    return OomCommitHeadroom(sample) <= thresholds->min_commit_headroom;
}

OOM_ACTION OomPolicyObserve(OOM_POLICY_STATE *state, const OOM_THRESHOLDS *thresholds,
                            const OOM_SAMPLE *sample)
{
    if (!OomSampleIsCritical(thresholds, sample)) {
        state->critical_samples = 0;
        state->samples_since_kill = 0;
        state->kills_issued = 0;
        state->kill_pending = 0;
        return OOM_ACTION_NONE;
    }

    if (state->kill_pending) {
        if (++state->samples_since_kill >= thresholds->kill_retry_samples) {
            state->kill_pending = 0;
            state->samples_since_kill = 0;
            return state->kills_issued < thresholds->max_kills
                       ? OOM_ACTION_KILL
                       : OOM_ACTION_ESCALATE;
        }
        return OOM_ACTION_NONE;
    }

    /* The kill budget is spent: only escalation remains until pressure clears. */
    if (state->kills_issued >= thresholds->max_kills)
        return OOM_ACTION_ESCALATE;

    if (++state->critical_samples >= thresholds->confirmation_samples) {
        state->critical_samples = 0;
        return OOM_ACTION_KILL;
    }
    return OOM_ACTION_NONE;
}

void OomPolicyKillIssued(OOM_POLICY_STATE *state)
{
    state->kill_pending = 1;
    state->samples_since_kill = 0;
    ++state->kills_issued;
}

OOM_WATCHDOG_ACTION OomWatchdogObserve(int armed, int critical, int kill_pending,
                                        OOM_U64 now, OOM_U64 last_heartbeat,
                                        OOM_U64 kill_deadline, OOM_U64 heartbeat_timeout)
{
    if (!armed || !critical)
        return OOM_WATCHDOG_NONE;
    if (kill_pending)
        return now >= kill_deadline ? OOM_WATCHDOG_KILL_TIMEOUT : OOM_WATCHDOG_NONE;
    if (now >= last_heartbeat && now - last_heartbeat >= heartbeat_timeout)
        return OOM_WATCHDOG_MONITOR_TIMEOUT;
    return OOM_WATCHDOG_NONE;
}
