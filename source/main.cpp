// bt-autoconnect: Atmosphere sysmodule that reconnects the last used
// Bluetooth audio device (headphones) when the Switch boots or wakes.
//
// It asks btm:sys to connect, the same call System Settings makes when you
// pick a paired device. See README.md.

#include <switch.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/stat.h>

#define DIR_PATH   "sdmc:/config/bt-autoconnect"
#define STATE_PATH DIR_PATH "/last_device.txt"
#define LOG_PATH   DIR_PATH "/log.txt"
#define LOG_OLD    DIR_PATH "/log.old.txt"

namespace {

constexpr u64 kMs = 1'000'000ULL;
constexpr u64 kSec = 1'000'000'000ULL;

// How long to keep trying after boot / wake. Boot is longer because btm and
// the home menu come up well after boot2 starts us.
constexpr u64 kBootWindow = 90 * kSec;
constexpr u64 kWakeWindow = 45 * kSec;
// A page to an absent device times out after ~5 s; give the request this long.
constexpr u64 kConnectTimeout = 10 * kSec;
constexpr u64 kPollConnecting = 200 * kMs;
constexpr u64 kRetryDelay = 1 * kSec;
constexpr long kLogMax = 64 * 1024;

// Custom PSC module id, not used by Nintendo (sys-con uses 126, btred 123).
constexpr u32 kPscModuleId = 124;
// Wake order: we hear Awake only after these modules are awake.
constexpr u32 kPscDeps[] = {PscPmModuleId_Bluetooth, PscPmModuleId_Btm, PscPmModuleId_Fs};

void Log(const char *fmt, ...) {
    FILE *f = fopen(LOG_PATH, "a");
    if (!f) return;
    if (ftell(f) > kLogMax) {
        fclose(f);
        remove(LOG_OLD);
        rename(LOG_PATH, LOG_OLD);
        f = fopen(LOG_PATH, "a");
        if (!f) return;
    }
    u64 ms = armTicksToNs(armGetSystemTick()) / kMs;
    time_t now = time(nullptr);
    struct tm tm_buf;
    char stamp[32] = "";
    if (now > 0 && localtime_r(&now, &tm_buf)) strftime(stamp, sizeof(stamp), "%m-%d %H:%M:%S", &tm_buf);
    fprintf(f, "[%s t=%llu.%03llu] ", stamp, (unsigned long long)(ms / 1000), (unsigned long long)(ms % 1000));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

u64 Now() { return armTicksToNs(armGetSystemTick()); }

bool AddrEq(const BtdrvAddress &a, const BtdrvAddress &b) { return memcmp(a.address, b.address, 6) == 0; }

bool AddrZero(const BtdrvAddress &a) {
    static const BtdrvAddress zero = {};
    return AddrEq(a, zero);
}

void AddrStr(const BtdrvAddress &a, char *out) {
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", a.address[0], a.address[1], a.address[2], a.address[3],
             a.address[4], a.address[5]);
}

bool ParseAddr(const char *s, BtdrvAddress &out) {
    unsigned b[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) return false;
    for (int i = 0; i < 6; i++) out.address[i] = (u8)b[i];
    return true;
}

BtdrvAddress g_target = {};

void LoadTarget() {
    FILE *f = fopen(STATE_PATH, "r");
    if (!f) return;
    char line[64] = "";
    if (fgets(line, sizeof(line), f) && ParseAddr(line, g_target)) {
        char s[18];
        AddrStr(g_target, s);
        Log("target from state file: %s", s);
    }
    fclose(f);
}

void SaveTarget(const BtdrvAddress &a, const char *name) {
    if (AddrEq(a, g_target)) return;
    g_target = a;
    char s[18];
    AddrStr(a, s);
    FILE *f = fopen(STATE_PATH, "w");
    if (f) {
        fprintf(f, "%s\n%s\n", s, name);
        fclose(f);
    }
    Log("new target: %s \"%s\"", s, name);
}

// Returns true when some audio device is connected; remembers it as the target.
bool CheckConnected() {
    static BtmAudioDevice devs[4];
    s32 total = 0;
    Result rc = btmsysGetConnectedAudioDevices(devs, 4, &total);
    if (R_FAILED(rc)) {
        Log("GetConnectedAudioDevices rc=0x%X", rc);
        return false;
    }
    if (total <= 0) return false;
    char name[0xFA] = {};
    memcpy(name, devs[0].name, sizeof(devs[0].name));
    SaveTarget(devs[0].addr, name);
    return true;
}

void LogPaired() {
    static BtmAudioDevice devs[10];
    s32 total = 0;
    Result rc = btmsysGetPairedAudioDevices(devs, 10, &total);
    if (R_FAILED(rc)) {
        Log("GetPairedAudioDevices rc=0x%X", rc);
        return;
    }
    Log("paired audio devices: %d", total);
    for (s32 i = 0; i < total && i < 10; i++) {
        char s[18], name[0xFA] = {};
        AddrStr(devs[i].addr, s);
        memcpy(name, devs[i].name, sizeof(devs[i].name));
        Log("  [%d] %s \"%s\"", i, s, name);
    }
}

enum class Phase { Idle, Pending, Connecting };

Phase g_phase = Phase::Idle;
u64 g_deadline = 0;    // end of the current burst
u64 g_next = 0;        // Pending: when to try next
u64 g_started = 0;     // Connecting: when the request went out
u64 g_burst_start = 0;
int g_attempts = 0;

void StartBurst(const char *why, u64 window) {
    u64 now = Now();
    g_burst_start = now;
    g_deadline = now + window;
    g_next = now;
    g_attempts = 0;
    g_phase = Phase::Pending;
    Log("burst start (%s)", why);
}

void StopBurst(const char *why) {
    if (g_phase == Phase::Idle) return;
    g_phase = Phase::Idle;
    Log("burst stop (%s) after %d attempt(s), %llu ms", why, g_attempts, (unsigned long long)((Now() - g_burst_start) / kMs));
}

void Step() {
    u64 now = Now();
    if (now >= g_deadline) {
        StopBurst("window over, giving up");
        return;
    }
    if (g_phase == Phase::Connecting) {
        bool connecting = false;
        Result rc = btmsysIsConnectingAudioDevice(&connecting);
        if (R_SUCCEEDED(rc) && connecting && now - g_started < kConnectTimeout) return;
        if (CheckConnected()) {
            StopBurst("connected");
            return;
        }
        Log("attempt %d did not connect (connecting=%d rc=0x%X, %llu ms)", g_attempts, connecting, rc,
            (unsigned long long)((now - g_started) / kMs));
        g_phase = Phase::Pending;
        g_next = now + kRetryDelay;
        return;
    }
    // Pending
    if (now < g_next) return;
    if (CheckConnected()) {
        StopBurst("already connected");
        return;
    }
    if (AddrZero(g_target)) {
        // Paired-list order says nothing about which headphones are yours;
        // wait until a device is connected once by hand and remember it.
        StopBurst("no target yet: connect your headphones once in System Settings");
        return;
    }
    bool connecting = false;
    if (R_SUCCEEDED(btmsysIsConnectingAudioDevice(&connecting)) && connecting) {
        g_phase = Phase::Connecting;
        g_started = now;
        return;
    }
    g_attempts++;
    char s[18];
    AddrStr(g_target, s);
    Result rc = btmsysConnectAudioDevice(g_target);
    Log("attempt %d: ConnectAudioDevice(%s) rc=0x%X", g_attempts, s, rc);
    if (R_SUCCEEDED(rc)) {
        g_phase = Phase::Connecting;
        g_started = now;
    } else {
        g_next = now + kRetryDelay;
    }
}

u64 TimeoutNs() {
    u64 now = Now();
    switch (g_phase) {
        case Phase::Idle: return UINT64_MAX;
        case Phase::Connecting: return kPollConnecting;
        case Phase::Pending: return g_next > now ? g_next - now : 0;
    }
    return UINT64_MAX;
}

}  // namespace

extern "C" {
u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 1;

void __libnx_initheap(void) {
    static char heap[0x20000];
    extern char *fake_heap_start;
    extern char *fake_heap_end;
    fake_heap_start = heap;
    fake_heap_end = heap + sizeof(heap);
}

void __libnx_init_time(void);

void __appInit(void) {
    if (R_FAILED(smInitialize())) diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_SM));
    if (R_SUCCEEDED(setsysInitialize())) {
        SetSysFirmwareVersion fw;
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw))) hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
        setsysExit();
    }
    if (R_FAILED(fsInitialize())) diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_FS));
    fsdevMountSdmc();
    // Wall-clock timestamps in the log; harmless if it fails.
    if (R_SUCCEEDED(timeInitialize())) __libnx_init_time();
    // Keep sm open: btm:sys and psc:m are looked up later from main().
}

void __appExit(void) {
    timeExit();
    fsdevUnmountAll();
    fsExit();
    smExit();
}
}

int main(int, char **) {
    mkdir("sdmc:/config", 0777);
    mkdir(DIR_PATH, 0777);
    Log("bt-autoconnect start, HOS %u.%u.%u", HOSVER_MAJOR(hosversionGet()), HOSVER_MINOR(hosversionGet()),
        HOSVER_MICRO(hosversionGet()));
    LoadTarget();

    Result rc = 0;
    for (int i = 0; i < 30; i++) {
        rc = btmsysInitialize();
        if (R_SUCCEEDED(rc)) break;
        Log("btmsysInitialize rc=0x%X, retrying", rc);
        svcSleepThread(2 * kSec);
    }
    if (R_FAILED(rc)) {
        Log("btmsysInitialize gave up, exiting");
        return 0;
    }
    Event audio_event;
    rc = btmsysAcquireAudioDeviceConnectionEvent(&audio_event);
    if (R_FAILED(rc)) {
        Log("AcquireAudioDeviceConnectionEvent rc=0x%X, exiting", rc);
        return 0;
    }

    PscPmModule psc = {};
    bool psc_ok = false;
    rc = pscmInitialize();
    if (R_SUCCEEDED(rc)) rc = pscmGetPmModule(&psc, (PscPmModuleId)kPscModuleId, kPscDeps,
                                              sizeof(kPscDeps) / sizeof(kPscDeps[0]), true);
    if (R_SUCCEEDED(rc)) psc_ok = true;
    else Log("PSC registration rc=0x%X: wake will not trigger reconnects", rc);

    LogPaired();
    StartBurst("boot", kBootWindow);

    while (true) {
        s32 idx = -1;
        if (psc_ok)
            rc = waitMulti(&idx, TimeoutNs(), waiterForEvent(&psc.event), waiterForEvent(&audio_event));
        else
            rc = waitMulti(&idx, TimeoutNs(), waiterForEvent(&audio_event));
        if (R_SUCCEEDED(rc)) {
            bool is_psc = psc_ok && idx == 0;
            if (is_psc) {
                PscPmState state;
                u32 flags;
                if (R_FAILED(pscPmModuleGetRequest(&psc, &state, &flags))) continue;
                // Acknowledge first: a module that stalls here stalls the
                // whole sleep/wake sequence (SnapBack held it 3 s).
                pscPmModuleAcknowledge(&psc, state);
                switch (state) {
                    case PscPmState_Awake:
                        Log("psc: awake");
                        StartBurst("wake", kWakeWindow);
                        break;
                    case PscPmState_ReadySleep:
                    case PscPmState_ReadyShutdown:
                        Log("psc: %s", state == PscPmState_ReadySleep ? "sleep" : "shutdown");
                        StopBurst("going to sleep");
                        break;
                    default:
                        break;
                }
            } else {
                // Audio connection changed: remember whatever got connected.
                if (CheckConnected()) StopBurst("connected (event)");
            }
            continue;
        }
        if (g_phase != Phase::Idle) Step();
    }
    return 0;
}
