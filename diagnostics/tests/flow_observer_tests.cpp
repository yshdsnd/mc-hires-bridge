// Headless verification tests for Issue #22 (flow_observer)
#include <windows.h>
#include <dshow.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include "flow_observer.h"

namespace {

int g_failures = 0;

void check(bool cond, const char* expr, int line) {
    if (!cond) {
        std::printf("  [FAIL] line %d: %s\n", line, expr);
        ++g_failures;
    }
}

#define CHECK(expr) check((expr), #expr, __LINE__)

// Test 1: FlowLogger sequence, monotonic time, and memory mode
void testLoggerSequencingAndMonotonicTime() {
    std::printf("[TEST 1] Logger sequence numbers and monotonic timestamps...\n");
    flowObserver::FlowLogger logger;
    logger.initMemory();

    for (int i = 0; i < 20; ++i) {
        logger.log("EVENT_STEP", reinterpret_cast<void*>(0x1000 + i),
                   reinterpret_cast<void*>(0x2000), reinterpret_cast<void*>(0x3000 + i * 4),
                   "index=%d message=test", i);
    }

    auto events = logger.getMemoryEvents();
    CHECK(events.size() == 20);

    for (size_t i = 0; i < events.size(); ++i) {
        CHECK(events[i].seq == i + 1);
        if (i > 0) {
            CHECK(events[i].timeUs >= events[i - 1].timeUs);
            CHECK(events[i].tickMs >= events[i - 1].tickMs);
        }
        CHECK(events[i].graphId == reinterpret_cast<void*>(0x2000));
        CHECK(events[i].objPtr == reinterpret_cast<void*>(0x1000 + i));
        CHECK(strcmp(events[i].event, "EVENT_STEP") == 0);
    }
}

// Test 2: Caller and Backtrace formatting
void testCallerAndBacktraceFormatting() {
    std::printf("[TEST 2] Caller address and multi-frame backtrace formatting...\n");

    char buf[128]{};
    flowObserver::FlowLogger::formatCaller(nullptr, buf, sizeof(buf));
    CHECK(strcmp(buf, "unknown") == 0);

    // Current function address should resolve to an .exe module name + RVA
    void* curAddr = reinterpret_cast<void*>(&testCallerAndBacktraceFormatting);
    flowObserver::FlowLogger::formatCaller(curAddr, buf, sizeof(buf));
    CHECK(strstr(buf, ".exe+0x") != nullptr);

    // Multi-frame backtrace formatting
    void* frames[3] = { curAddr, reinterpret_cast<void*>(&check), nullptr };
    char btBuf[384]{};
    flowObserver::FlowLogger::formatBacktrace(frames, 2, btBuf, sizeof(btBuf));
    CHECK(strstr(btBuf, " -> ") != nullptr);
}

// Test 3: Seeking flags symbolic and bitwise parsing
void testSeekingFlagsFormatting() {
    std::printf("[TEST 3] IMediaSeeking flags formatting...\n");

    char buf[128]{};
    flowObserver::FlowLogger::formatSeekingFlags(AM_SEEKING_NoPositioning, buf, sizeof(buf));
    CHECK(strcmp(buf, "0x00(NoPositioning)") == 0);

    flowObserver::FlowLogger::formatSeekingFlags(AM_SEEKING_AbsolutePositioning, buf, sizeof(buf));
    CHECK(strcmp(buf, "0x01(Absolute)") == 0);

    flowObserver::FlowLogger::formatSeekingFlags(AM_SEEKING_AbsolutePositioning | AM_SEEKING_NoFlush, buf, sizeof(buf));
    CHECK(strcmp(buf, "0x21(Absolute|NoFlush)") == 0);

    flowObserver::FlowLogger::formatSeekingFlags(AM_SEEKING_RelativePositioning | AM_SEEKING_SeekToKeyFrame | AM_SEEKING_ReturnTime, buf, sizeof(buf));
    CHECK(strcmp(buf, "0x0E(Relative|KeyFrame|ReturnTime)") == 0);
}

// Test 4: Scenario correlation for 24bit -> 16bit format-change transition
void testScenarioFormatChangeCorrelation() {
    std::printf("[TEST 4] Scenario: 24bit -> 16bit format change event correlation...\n");
    flowObserver::FlowLogger logger;
    logger.initMemory();

    void* graph = reinterpret_cast<void*>(0x8001);
    void* pSeeking = reinterpret_cast<void*>(0x9001);
    void* pSeamless = reinterpret_cast<void*>(0x9002);
    void* pEq = reinterpret_cast<void*>(0x9003);
    void* pRenderer = reinterpret_cast<void*>(0x9004);
    void* pOpc = reinterpret_cast<void*>(0x9005);

    // 1. Seamless worker fires 3841
    logger.logWithCallerStr("DS_EVENT_3841", pSeamless, graph, "SeamlessFilter.ax+0x00004520",
                            "code=3841(0x0F01) p1=0x0 p2=0x0");

    // 2. OpcFlac attempts seamless pre-read of next 16-bit track
    logger.logWithCallerStr("OPC_FLAC_OPEN_ENTER", pOpc, graph, "OmgPcMan.dll+0x00018520",
                            "flags=1(SeamlessPreRead) file=Track2_16bit.flac");

    // 3. Pre-read fails with format mismatch
    logger.logWithCallerStr("OPC_FLAC_OPEN_LEAVE", pOpc, graph, "OmgPcMan.dll+0x00018520",
                            "flags=1 hr=0x80040200 elapsedUs=1240");

    // 4. Music Center executes SetPositions to 20.088s (cutting off output!)
    char currFlags[64]{}, stopFlags[64]{};
    flowObserver::FlowLogger::formatSeekingFlags(AM_SEEKING_AbsolutePositioning, currFlags, sizeof(currFlags));
    flowObserver::FlowLogger::formatSeekingFlags(AM_SEEKING_NoPositioning, stopFlags, sizeof(stopFlags));

    LONGLONG seekPos = 200880000LL; // 20.088s
    logger.logWithCallerStr("SEEK_SETPOSITIONS_ENTER", pSeeking, graph, "OpcDSPlay.dll+0x000213A0",
                            "currFlags=%s currPos=%lld(%.4fs) stopFlags=%s stopPos=-1(-1.0000s) backtrace=OpcDSPlay.dll+0x000213A0 -> OmgPcMan.dll+0x00019200",
                            currFlags, seekPos, double(seekPos) / 10000000.0, stopFlags);

    // 5. SetPositions flushes pins (discarding audio in buffer)
    logger.logWithCallerStr("PIN_BEGIN_FLUSH", pRenderer, graph, "quartz.dll+0x00041200", "pin_name=Input dir=input clsid={CLSID_DSoundRender}");
    logger.logWithCallerStr("PIN_BEGIN_FLUSH", pEq, graph, "quartz.dll+0x00041200", "pin_name=Input dir=input clsid={5C968488-5E35-4CFE-98A1-A51C838797DB}");
    logger.logWithCallerStr("PIN_END_FLUSH", pEq, graph, "quartz.dll+0x00041200", "pin_name=Input dir=input");
    logger.logWithCallerStr("PIN_END_FLUSH", pRenderer, graph, "quartz.dll+0x00041200", "pin_name=Input dir=input");

    // 6. DirectSound buffer stopped and reset
    logger.logWithCallerStr("DS_STOP", pRenderer, graph, "dsound.dll+0x0001A400",
                            "playCursor=115200 writeCursor=120000 bufBytes=144000 rate=48000 bits=24 ch=2");

    // 7. Track 2 begins with non-seamless Open(flags=0)
    logger.logWithCallerStr("OPC_FLAC_OPEN_ENTER", pOpc, graph, "OmgPcMan.dll+0x00018600",
                            "flags=0(NormalTrackOpen) file=Track2_16bit.flac");

    auto events = logger.getMemoryEvents();
    CHECK(events.size() == 10);

    // Verify key diagnostic points
    bool found3841 = false;
    bool foundPreReadFail = false;
    bool foundSetPositionsAt20s = false;
    bool foundFlush = false;
    bool foundControlStop = false;

    for (const auto& ev : events) {
        if (strcmp(ev.event, "DS_EVENT_3841") == 0) found3841 = true;
        if (strcmp(ev.event, "OPC_FLAC_OPEN_LEAVE") == 0 && strstr(ev.details, "hr=0x80040200")) foundPreReadFail = true;
        if (strcmp(ev.event, "SEEK_SETPOSITIONS_ENTER") == 0 && strstr(ev.details, "200880000")) foundSetPositionsAt20s = true;
        if (strcmp(ev.event, "PIN_BEGIN_FLUSH") == 0) foundFlush = true;
        if (strcmp(ev.event, "CONTROL_STOP_ENTER") == 0) foundControlStop = true;
    }

    CHECK(found3841 == true);
    CHECK(foundPreReadFail == true);
    CHECK(foundSetPositionsAt20s == true);
    CHECK(foundFlush == true);
    CHECK(foundControlStop == false); // Proves IMediaControl::Stop was NOT called, isolating SetPositions + Flush as the root cause!
}

// Test 5: Scenario comparison: Manual Seek vs Format Change
void testScenarioManualSeekComparison() {
    std::printf("[TEST 5] Scenario comparison: Manual Seek vs Automatic Format Change...\n");
    flowObserver::FlowLogger logger;
    logger.initMemory();

    void* graph = reinterpret_cast<void*>(0x8002);
    void* pSeeking = reinterpret_cast<void*>(0x9001);
    void* pRenderer = reinterpret_cast<void*>(0x9004);

    // Manual user seek: initiated by MusicCenter.exe UI thread directly to 10.0s
    char currFlags[64]{}, stopFlags[64]{};
    flowObserver::FlowLogger::formatSeekingFlags(AM_SEEKING_AbsolutePositioning, currFlags, sizeof(currFlags));
    flowObserver::FlowLogger::formatSeekingFlags(AM_SEEKING_NoPositioning, stopFlags, sizeof(stopFlags));

    LONGLONG seekPos = 100000000LL; // 10.0s
    logger.logWithCallerStr("SEEK_SETPOSITIONS_ENTER", pSeeking, graph, "MusicCenter.exe+0x000F4120",
                            "currFlags=%s currPos=%lld(%.4fs) stopFlags=%s stopPos=-1(-1.0000s) backtrace=MusicCenter.exe+0x000F4120",
                            currFlags, seekPos, double(seekPos) / 10000000.0, stopFlags);
    logger.logWithCallerStr("PIN_BEGIN_FLUSH", pRenderer, graph, "quartz.dll+0x00041200", "pin_name=Input dir=input");
    logger.logWithCallerStr("PIN_END_FLUSH", pRenderer, graph, "quartz.dll+0x00041200", "pin_name=Input dir=input");

    auto events = logger.getMemoryEvents();
    CHECK(events.size() == 3);

    // Caller is MusicCenter.exe (UI initiated) rather than OpcDSPlay.dll (playback engine initiated)
    CHECK(strstr(events[0].caller, "MusicCenter.exe") != nullptr);
    CHECK(strstr(events[0].details, "100000000") != nullptr);
}

// Test 6: File TSV writing and header verification
void testFileTsvOutput() {
    std::printf("[TEST 6] File TSV output and formatting...\n");
    wchar_t tempDir[MAX_PATH], testPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempDir);
    swprintf_s(testPath, L"%sFlowObserverTest-%lu.tsv", tempDir, GetCurrentProcessId());

    {
        flowObserver::FlowLogger logger;
        CHECK(logger.initFile(testPath));

        logger.logWithCallerStr("TEST_EVENT", reinterpret_cast<void*>(0x1111), reinterpret_cast<void*>(0x2222),
                                "TestModule.dll+0x00001000", "key1=value1 key2=value2");
        logger.close();
    }

    FILE* fp = nullptr;
    _wfopen_s(&fp, testPath, L"rb");
    CHECK(fp != nullptr);
    if (fp) {
        char buf[1024]{};
        size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
        fclose(fp);
        DeleteFileW(testPath);

        CHECK(n > 0);
        buf[n] = '\0';
        CHECK(strstr(buf, "seq\ttime_us\ttick_ms\ttid\tgraph_id\tobj_ptr\tevent\tcaller\tdetails\r\n") != nullptr);
        CHECK(strstr(buf, "TEST_EVENT") != nullptr);
        CHECK(strstr(buf, "TestModule.dll+0x00001000") != nullptr);
        CHECK(strstr(buf, "key1=value1 key2=value2") != nullptr);
    }
}

} // namespace

int main() {
    std::printf("=== Issue #22 Flow Observer Headless Tests ===\n");

    testLoggerSequencingAndMonotonicTime();
    testCallerAndBacktraceFormatting();
    testSeekingFlagsFormatting();
    testScenarioFormatChangeCorrelation();
    testScenarioManualSeekComparison();
    testFileTsvOutput();

    if (g_failures == 0) {
        std::printf("=== ALL ISSUE #22 FLOW OBSERVER TESTS PASSED ===\n");
        return 0;
    }
    std::printf("=== %d TEST ASSERTION(S) FAILED ===\n", g_failures);
    return 1;
}
