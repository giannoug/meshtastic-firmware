// Renders the T5 InkHUD screens on the host and saves each frame, for checking against the design mockups.
// Frames go to $T5_SHOT_DIR (default ./t5shots).
#include "TestUtil.h"
#include <unity.h>

#ifdef T5_SCREENS_HOST
#include "configuration.h"

#include "DrawLog.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "ReliableRouter.h"
#include "ScreenCapture.h"
#include "graphics/niche/InkHUD/InkHUD.h"
#include "graphics/niche/InkHUD/SystemApplet.h"
#include "modules/Modules.h"

#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <unity.h>

void setupNicheGraphics();

using namespace NicheGraphics;

static std::string shotDir()
{
    const char *d = getenv("T5_SHOT_DIR");
    std::string dir = d ? d : "t5shots";
    mkdir(dir.c_str(), 0755);
    return dir;
}

static void renderNow()
{
    InkHUD::InkHUD::getInstance()->forceUpdate(Drivers::EInk::UpdateTypes::FULL, true, false);
}

// The boot logo holds the screen for 8 s on the device; the harness never waits that long
static void dismissBootScreens()
{
    if (InkHUD::SystemApplet *logo = InkHUD::InkHUD::getInstance()->getSystemApplet("Logo"))
        logo->sendToBackground();
}

// Render the current state and save <name>-<carry|console>.pbm plus its draw log
static void snap(const std::string &name)
{
    dismissBootScreens();
    drawLogStart();
    renderNow();
    drawLogStop();
    std::string base = shotDir() + "/" + name + (t5CurrentMode() == T5Mode::CARRY ? "-carry" : "-console");
    TEST_ASSERT_TRUE(captureSavePbm((base + ".pbm").c_str()));
    TEST_ASSERT_TRUE(drawLogDump((base + ".json").c_str()));
}

static bool logHasText(const char *needle)
{
    for (const DrawRec &r : drawLog())
        if (r.kind == DrawRec::TEXT && r.text.find(needle) != std::string::npos)
            return true;
    return false;
}

void setUp() {}
void tearDown() {}

static void test_home_renders_in_both_modes()
{
    for (T5Mode mode : {T5Mode::CARRY, T5Mode::CONSOLE}) {
        t5SetMode(mode);
        uint32_t before = captureFrameCount();
        snap("home");
        TEST_ASSERT_GREATER_THAN_UINT32(before, captureFrameCount());
        TEST_ASSERT_GREATER_THAN_UINT32(1000, captureBlackPixels());
        TEST_ASSERT_TRUE(logHasText("HOME") || logHasText("Home"));
    }
}

void setup()
{
    initializeTestEnvironment();
    nodeDB = new NodeDB();
    service = new MeshService();
    router = new ReliableRouter();
    setupModules();
    setupNicheGraphics();
    UNITY_BEGIN();
    RUN_TEST(test_home_renders_in_both_modes);
    exit(UNITY_END());
}

void loop() {}

#else // Only env:native-t5-screens builds InkHUD and the T5 sources

void setUp() {}
void tearDown() {}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    exit(UNITY_END());
}

void loop() {}

#endif
