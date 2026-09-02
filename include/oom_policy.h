#ifndef OOM_POLICY_H
#define OOM_POLICY_H

#include "oom_types.h"

typedef enum OOM_ACTION {
    OOM_ACTION_NONE,
    OOM_ACTION_KILL,
    OOM_ACTION_ESCALATE
} OOM_ACTION;

typedef enum OOM_WATCHDOG_ACTION {
    OOM_WATCHDOG_NONE,
    OOM_WATCHDOG_KILL_TIMEOUT,
    OOM_WATCHDOG_MONITOR_TIMEOUT
} OOM_WATCHDOG_ACTION;

typedef struct OOM_THRESHOLDS {
    OOM_U64 min_commit_headroom;
    OOM_U32 confirmation_samples;
    OOM_U32 kill_retry_samples;
    OOM_U32 max_kills;
} OOM_THRESHOLDS;

typedef struct OOM_SAMPLE {
    OOM_U64 commit_charge;
    OOM_U64 commit_limit;
} OOM_SAMPLE;

typedef struct OOM_POLICY_STATE {
    OOM_U32 critical_samples;
    OOM_U32 samples_since_kill;
    OOM_U32 kills_issued;
    int kill_pending;
} OOM_POLICY_STATE;

int OomSampleIsCritical(const OOM_THRESHOLDS *thresholds, const OOM_SAMPLE *sample);
OOM_ACTION OomPolicyObserve(OOM_POLICY_STATE *state, const OOM_THRESHOLDS *thresholds,
                            const OOM_SAMPLE *sample);
void OomPolicyKillIssued(OOM_POLICY_STATE *state);
OOM_WATCHDOG_ACTION OomWatchdogObserve(int armed, int critical, int kill_pending,
                                        OOM_U64 now, OOM_U64 last_heartbeat,
                                        OOM_U64 kill_deadline, OOM_U64 heartbeat_timeout);

#endif
