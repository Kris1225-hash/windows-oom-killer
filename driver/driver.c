#include <ntifs.h>
#include <wdf.h>
#include <wdmsec.h>

#include "../include/oom_protocol.h"
#include "../include/oom_policy.h"

#define OOM_HEARTBEAT_TIMEOUT_100NS (5ull * 1000 * 1000 * 10)
#define OOM_KILL_WAIT_100NS          (5ull * 1000 * 1000 * 10)
#define OOM_KILL_WATCHDOG_100NS      (6ull * 1000 * 1000 * 10)
#define OOM_MIN_VICTIM_BYTES         (64ull * 1024 * 1024)
#define OOM_COMMIT_HEADROOM_BYTES    (512ull * 1024 * 1024)
#define OOM_PROCESS_TERMINATE         0x0001u

NTKERNELAPI BOOLEAN PsIsProtectedProcess(PEPROCESS process);
NTKERNELAPI BOOLEAN PsIsProtectedProcessLight(PEPROCESS process);

C_ASSERT(sizeof(OOM_TELEMETRY) == 64);
C_ASSERT(sizeof(OOM_KILL_REQUEST) == 88);

typedef struct DEVICE_CONTEXT {
    WDFSPINLOCK lock;
    PKEVENT maximum_commit_event;
    HANDLE maximum_commit_handle;
    OOM_TELEMETRY telemetry;
    ULONGLONG last_heartbeat;
    ULONGLONG kill_deadline;
    ULONG owner_pid;
    NTSTATUS last_kill_status;
    BOOLEAN armed;
    BOOLEAN kill_pending;
} DEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, DeviceGetContext)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL OomEvtIoDeviceControl;
EVT_WDF_TIMER OomEvtWatchdog;
EVT_WDF_DRIVER_UNLOAD OomEvtDriverUnload;
EVT_WDF_OBJECT_CONTEXT_CLEANUP OomEvtDeviceCleanup;
EVT_WDF_FILE_CLEANUP OomEvtFileCleanup;

static BOOLEAN OomTelemetryValid(const OOM_TELEMETRY *t)
{
    return t->version == OOM_PROTOCOL_VERSION &&
           !(t->flags & ~(OOM_FLAG_ARMED | OOM_FLAG_CRITICAL)) &&
           t->sequence && t->service_pid > 4 && t->page_size >= 4096 &&
           t->page_size <= 65536 &&
           !(t->page_size & (t->page_size - 1)) && t->commit_limit_pages &&
           t->backing_reason <= OOM_BACKING_KILL_REJECTED && !t->reserved;
}

static BOOLEAN OomTelemetryCritical(const OOM_TELEMETRY *t)
{
    ULONGLONG headroom = t->commit_charge_pages < t->commit_limit_pages
                             ? t->commit_limit_pages - t->commit_charge_pages
                             : 0;
    return (t->flags & OOM_FLAG_CRITICAL) &&
           headroom <= OOM_COMMIT_HEADROOM_BYTES / t->page_size;
}

static ULONG_PTR OomBackingParameter(const OOM_TELEMETRY *t)
{
    ULONG free_pages = t->pagefile_free_pages > MAXULONG
                           ? MAXULONG
                           : (ULONG)t->pagefile_free_pages;
    return ((ULONG_PTR)t->backing_reason << 32) | free_pages;
}

static ULONG OomBugcheckForBackingReason(ULONG reason)
{
    switch (reason) {
    case OOM_BACKING_PAGEFILE_DISABLED:
        return OOM_FATAL_PAGEFILE_DISABLED;
    case OOM_BACKING_PAGEFILE_EXHAUSTED:
        return OOM_FATAL_PAGEFILE_EXHAUSTED;
    case OOM_BACKING_PAGEFILE_GROWTH_FAILED:
        return OOM_FATAL_PAGEFILE_GROWTH_FAILED;
    case OOM_BACKING_NO_ELIGIBLE_PROCESS:
        return OOM_FATAL_NO_KILLABLE_PROCESS;
    case OOM_BACKING_KILL_REJECTED:
        return OOM_FATAL_KILL_FAILED;
    default:
        return OOM_FATAL_NO_BACKING_STORE;
    }
}

static BOOLEAN OomMaximumCommitReached(const DEVICE_CONTEXT *ctx)
{
    return ctx->maximum_commit_event && KeReadStateEvent(ctx->maximum_commit_event) != 0;
}

__declspec(noreturn) static VOID OomBugcheck(ULONG code, const OOM_TELEMETRY *t)
{
    KeBugCheckEx(code, (ULONG_PTR)t->commit_charge_pages,
                 (ULONG_PTR)t->commit_limit_pages,
                 (ULONG_PTR)t->available_physical_pages,
                 OomBackingParameter(t));
}

static NTSTATUS OomSaveTelemetry(WDFREQUEST request, DEVICE_CONTEXT *ctx,
                                 const OOM_TELEMETRY *telemetry, BOOLEAN allow_equal_sequence)
{
    ULONG caller = WdfRequestGetRequestorProcessId(request);
    ULONGLONG now = KeQueryInterruptTime();
    BOOLEAN new_owner;

    if (!OomTelemetryValid(telemetry) || caller != telemetry->service_pid)
        return STATUS_INVALID_PARAMETER;

    WdfSpinLockAcquire(ctx->lock);
    new_owner = ctx->owner_pid == 0;
    if (ctx->owner_pid && ctx->owner_pid != caller) {
        WdfSpinLockRelease(ctx->lock);
        return STATUS_ACCESS_DENIED;
    }
    if (!new_owner && (telemetry->sequence < ctx->telemetry.sequence ||
                       (!allow_equal_sequence &&
                        telemetry->sequence == ctx->telemetry.sequence))) {
        WdfSpinLockRelease(ctx->lock);
        return STATUS_RETRY;
    }

    ctx->owner_pid = caller;
    ctx->telemetry = *telemetry;
    ctx->armed = !!(telemetry->flags & OOM_FLAG_ARMED);
    ctx->last_heartbeat = now;
    if (!ctx->armed || !OomTelemetryCritical(telemetry))
        ctx->kill_pending = FALSE;
    WdfSpinLockRelease(ctx->lock);
    return STATUS_SUCCESS;
}

static NTSTATUS OomTerminateVictim(WDFREQUEST request, DEVICE_CONTEXT *ctx,
                                   const OOM_KILL_REQUEST *kill)
{
    PEPROCESS process = NULL;
    HANDLE process_handle = NULL;
    NTSTATUS status;
    LARGE_INTEGER timeout;
    BOOLEAN armed;

    status = OomSaveTelemetry(request, ctx, &kill->telemetry, TRUE);
    if (!NT_SUCCESS(status))
        return status;
    if (!(kill->telemetry.flags & OOM_FLAG_ARMED) ||
        !OomTelemetryCritical(&kill->telemetry) || kill->victim_pid <= 4 ||
        kill->victim_pid == kill->telemetry.service_pid ||
        kill->reserved || !kill->victim_create_time ||
        kill->victim_private_pages < OOM_MIN_VICTIM_BYTES / kill->telemetry.page_size)
        return STATUS_ACCESS_DENIED;

    status = PsLookupProcessByProcessId(ULongToHandle(kill->victim_pid), &process);
    if (!NT_SUCCESS(status))
        return status;
    if (process == PsInitialSystemProcess || PsIsProtectedProcess(process) ||
        PsIsProtectedProcessLight(process) ||
        (ULONGLONG)PsGetProcessCreateTimeQuadPart(process) != kill->victim_create_time) {
        ObDereferenceObject(process);
        return STATUS_ACCESS_DENIED;
    }

    status = ObOpenObjectByPointer(process, OBJ_KERNEL_HANDLE, NULL,
                                   OOM_PROCESS_TERMINATE | SYNCHRONIZE, *PsProcessType,
                                   KernelMode, &process_handle);
    ObDereferenceObject(process);
    if (!NT_SUCCESS(status))
        return status;

    WdfSpinLockAcquire(ctx->lock);
    ctx->kill_pending = TRUE;
    ctx->kill_deadline = KeQueryInterruptTime() + OOM_KILL_WATCHDOG_100NS;
    WdfSpinLockRelease(ctx->lock);

    status = ZwTerminateProcess(process_handle, STATUS_NO_MEMORY);
    if (NT_SUCCESS(status) || status == STATUS_PROCESS_IS_TERMINATING) {
        timeout.QuadPart = -(LONGLONG)OOM_KILL_WAIT_100NS;
        status = ZwWaitForSingleObject(process_handle, FALSE, &timeout);
    }
    ZwClose(process_handle);

    WdfSpinLockAcquire(ctx->lock);
    ctx->last_kill_status = status;
    ctx->kill_pending = FALSE;
    armed = ctx->armed;
    WdfSpinLockRelease(ctx->lock);

    if (!NT_SUCCESS(status) && armed)
        OomBugcheck(status == STATUS_TIMEOUT ? OOM_FATAL_KILL_TIMEOUT
                                             : OOM_FATAL_KILL_FAILED,
                    &kill->telemetry);
    return status;
}

VOID OomEvtWatchdog(WDFTIMER timer)
{
    DEVICE_CONTEXT *ctx = DeviceGetContext((WDFDEVICE)WdfTimerGetParentObject(timer));
    OOM_TELEMETRY telemetry;
    ULONGLONG now = KeQueryInterruptTime();
    ULONG bugcheck = 0;
    OOM_WATCHDOG_ACTION action;
    BOOLEAN maximum_commit = OomMaximumCommitReached(ctx);

    WdfSpinLockAcquire(ctx->lock);
    telemetry = ctx->telemetry;
    action = OomWatchdogObserve(ctx->armed,
                               OomTelemetryCritical(&telemetry) || maximum_commit,
                               ctx->kill_pending, now, ctx->last_heartbeat,
                               ctx->kill_deadline, OOM_HEARTBEAT_TIMEOUT_100NS);
    WdfSpinLockRelease(ctx->lock);

    if (action == OOM_WATCHDOG_KILL_TIMEOUT)
        bugcheck = OOM_FATAL_KILL_TIMEOUT;
    else if (action == OOM_WATCHDOG_MONITOR_TIMEOUT) {
        if (maximum_commit) {
            telemetry.backing_reason = OOM_BACKING_PAGEFILE_GROWTH_FAILED;
            bugcheck = OOM_FATAL_PAGEFILE_GROWTH_FAILED;
        } else {
            bugcheck = OOM_FATAL_MONITOR_UNRESPONSIVE;
        }
    }

    if (bugcheck)
        OomBugcheck(bugcheck, &telemetry);
}

VOID OomEvtDeviceCleanup(WDFOBJECT device_object)
{
    DEVICE_CONTEXT *ctx = DeviceGetContext((WDFDEVICE)device_object);

    ctx->maximum_commit_event = NULL;
    if (ctx->maximum_commit_handle) {
        ZwClose(ctx->maximum_commit_handle);
        ctx->maximum_commit_handle = NULL;
    }
}

VOID OomEvtDriverUnload(WDFDRIVER driver)
{
    UNREFERENCED_PARAMETER(driver);
}

VOID OomEvtFileCleanup(WDFFILEOBJECT file_object)
{
    DEVICE_CONTEXT *ctx = DeviceGetContext(WdfFileObjectGetDevice(file_object));

    WdfSpinLockAcquire(ctx->lock);
    ctx->owner_pid = 0;
    if (!ctx->armed) {
        RtlZeroMemory(&ctx->telemetry, sizeof(ctx->telemetry));
        ctx->kill_pending = FALSE;
    }
    WdfSpinLockRelease(ctx->lock);
}

VOID OomEvtIoDeviceControl(WDFQUEUE queue, WDFREQUEST request, size_t output_length,
                           size_t input_length, ULONG ioctl)
{
    DEVICE_CONTEXT *ctx = DeviceGetContext(WdfIoQueueGetDevice(queue));
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    VOID *buffer;
    size_t length;

    UNREFERENCED_PARAMETER(output_length);
    UNREFERENCED_PARAMETER(input_length);

    switch (ioctl) {
    case IOCTL_OOM_HEARTBEAT:
        status = WdfRequestRetrieveInputBuffer(request, sizeof(OOM_TELEMETRY), &buffer, &length);
        if (NT_SUCCESS(status))
            status = OomSaveTelemetry(request, ctx, (const OOM_TELEMETRY *)buffer, FALSE);
        break;

    case IOCTL_OOM_KILL:
        status = WdfRequestRetrieveInputBuffer(request, sizeof(OOM_KILL_REQUEST), &buffer, &length);
        if (NT_SUCCESS(status))
            status = OomTerminateVictim(request, ctx, (const OOM_KILL_REQUEST *)buffer);
        break;

    case IOCTL_OOM_ESCALATE:
        status = WdfRequestRetrieveInputBuffer(request, sizeof(OOM_TELEMETRY), &buffer, &length);
        if (NT_SUCCESS(status)) {
            OOM_TELEMETRY telemetry = *(const OOM_TELEMETRY *)buffer;
            status = OomSaveTelemetry(request, ctx, &telemetry, TRUE);
            if (NT_SUCCESS(status) && (telemetry.flags & OOM_FLAG_ARMED) &&
                OomTelemetryCritical(&telemetry)) {
                if (OomMaximumCommitReached(ctx))
                    telemetry.backing_reason = OOM_BACKING_PAGEFILE_GROWTH_FAILED;
                WdfRequestComplete(request, STATUS_SUCCESS);
                OomBugcheck(OomBugcheckForBackingReason(telemetry.backing_reason), &telemetry);
            }
        }
        break;

    case IOCTL_OOM_STATUS:
        status = WdfRequestRetrieveOutputBuffer(request, sizeof(OOM_DRIVER_STATUS), &buffer, &length);
        if (NT_SUCCESS(status)) {
            OOM_DRIVER_STATUS *out = (OOM_DRIVER_STATUS *)buffer;
            WdfSpinLockAcquire(ctx->lock);
            RtlZeroMemory(out, sizeof(*out));
            out->version = OOM_PROTOCOL_VERSION;
            out->armed = ctx->armed;
            out->kill_pending = ctx->kill_pending;
            out->last_kill_status = ctx->last_kill_status;
            out->last_sequence = ctx->telemetry.sequence;
            out->owner_pid = ctx->owner_pid;
            WdfSpinLockRelease(ctx->lock);
            WdfRequestSetInformation(request, sizeof(*out));
        }
        break;

#if defined(OOM_ENABLE_TEST_IOCTL)
    case IOCTL_OOM_TEST_BUGCHECK:
        status = WdfRequestRetrieveInputBuffer(request, sizeof(OOM_BUGCHECK_REQUEST),
                                               &buffer, &length);
        if (NT_SUCCESS(status)) {
            const OOM_BUGCHECK_REQUEST *test = buffer;
            if (test->confirm != OOM_TEST_CONFIRM ||
                test->code < OOM_FATAL_NO_BACKING_STORE ||
                test->code > OOM_FATAL_MONITOR_UNRESPONSIVE) {
                status = STATUS_INVALID_PARAMETER;
                break;
            }
            WdfRequestComplete(request, STATUS_SUCCESS);
            KeBugCheckEx(test->code, (ULONG_PTR)test->parameters[0],
                         (ULONG_PTR)test->parameters[1],
                         (ULONG_PTR)test->parameters[2],
                         (ULONG_PTR)test->parameters[3]);
            return;
        }
        break;
#endif
    }

    WdfRequestComplete(request, status);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT driver_object, PUNICODE_STRING registry_path)
{
    WDF_DRIVER_CONFIG driver_config;
    WDF_OBJECT_ATTRIBUTES attributes;
    PWDFDEVICE_INIT device_init = NULL;
    WDFDEVICE device;
    WDF_IO_QUEUE_CONFIG queue_config;
    WDF_FILEOBJECT_CONFIG file_config;
    WDF_TIMER_CONFIG timer_config;
    WDFTIMER timer;
    UNICODE_STRING device_name = RTL_CONSTANT_STRING(L"\\Device\\WinOomKiller");
    UNICODE_STRING symlink = RTL_CONSTANT_STRING(L"\\DosDevices\\WinOomKiller");
    UNICODE_STRING maximum_commit_name =
        RTL_CONSTANT_STRING(L"\\KernelObjects\\MaximumCommitCondition");
    NTSTATUS status;

    WDF_DRIVER_CONFIG_INIT(&driver_config, WDF_NO_EVENT_CALLBACK);
    driver_config.DriverInitFlags |= WdfDriverInitNonPnpDriver;
    driver_config.EvtDriverUnload = OomEvtDriverUnload;
    status = WdfDriverCreate(driver_object, registry_path, WDF_NO_OBJECT_ATTRIBUTES,
                             &driver_config, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status))
        return status;

    device_init = WdfControlDeviceInitAllocate(WdfGetDriver(), &SDDL_DEVOBJ_SYS_ALL_ADM_ALL);
    if (!device_init)
        return STATUS_INSUFFICIENT_RESOURCES;
    status = WdfDeviceInitAssignName(device_init, &device_name);
    if (!NT_SUCCESS(status))
        goto fail;
    WdfDeviceInitSetDeviceType(device_init, FILE_DEVICE_UNKNOWN);
    WdfDeviceInitSetExclusive(device_init, TRUE);
    WDF_FILEOBJECT_CONFIG_INIT(&file_config, WDF_NO_EVENT_CALLBACK,
                               WDF_NO_EVENT_CALLBACK, OomEvtFileCleanup);
    WdfDeviceInitSetFileObjectConfig(device_init, &file_config, WDF_NO_OBJECT_ATTRIBUTES);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DEVICE_CONTEXT);
    attributes.EvtCleanupCallback = OomEvtDeviceCleanup;
    status = WdfDeviceCreate(&device_init, &attributes, &device);
    if (!NT_SUCCESS(status))
        goto fail;
    status = WdfDeviceCreateSymbolicLink(device, &symlink);
    if (!NT_SUCCESS(status))
        return status;

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    status = WdfSpinLockCreate(&attributes, &DeviceGetContext(device)->lock);
    if (!NT_SUCCESS(status))
        return status;

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queue_config, WdfIoQueueDispatchSequential);
    queue_config.EvtIoDeviceControl = OomEvtIoDeviceControl;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfIoQueueCreate(device, &queue_config, &attributes, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status))
        return status;

    WDF_TIMER_CONFIG_INIT_PERIODIC(&timer_config, OomEvtWatchdog, 1000);
    timer_config.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    status = WdfTimerCreate(&timer_config, &attributes, &timer);
    if (!NT_SUCCESS(status))
        return status;

    DeviceGetContext(device)->maximum_commit_event = IoCreateNotificationEvent(
        &maximum_commit_name, &DeviceGetContext(device)->maximum_commit_handle);
    if (!DeviceGetContext(device)->maximum_commit_event)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    WdfControlFinishInitializing(device);
    WdfTimerStart(timer, WDF_REL_TIMEOUT_IN_MS(1000));
    return STATUS_SUCCESS;

fail:
    if (device_init)
        WdfDeviceInitFree(device_init);
    return status;
}
