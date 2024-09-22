#include <cstring>
#include "utils/StringTools.h"
#include <coreinit/cache.h>
#include <coreinit/debug.h>
#include <coreinit/dynload.h>
#include <coreinit/filesystem_fsa.h>
#include <coreinit/foreground.h>
#include <coreinit/ios.h>
#include <coreinit/savedframe.h>
#include <coreinit/screen.h>
#include <coreinit/title.h>
#include <elfio/elfio.hpp>
#include <fcntl.h>
#include <gx2/display.h>
#include <gx2/state.h>
#include <malloc.h>
#include <memory>
#include <nn/act/client_cpp.h>
#include <nsysccr/cdc.h>
#include <proc_ui/procui.h>
#include <sysapp/launch.h>
#include <sysapp/title.h>
#include <vector>
#include <vpad/input.h>
#include <whb/log_cafe.h>
#include <whb/log_module.h>
#include <whb/log_udp.h>
#include "ElfUtils.h"
#include "common/module_defines.h"
#include "fs/DirList.h"
#include "kernel.h"
#include "module/ModuleDataFactory.h"
#include "utils/DrawUtils.h"
#include "utils/FileUtils.h"
#include "utils/InputUtils.h"
#include "utils/OnLeavingScope.h"
#include "utils/PairUtils.h"
#include "utils/utils.h"
#include "utils/wiiu_zlib.hpp"

#define CATYNETWORK_VERSION "v3.0.0"
#define CATYNETWORK_VERSION_EXTRA ""

bool CheckRunning() {
    switch (ProcUIProcessMessages(true)) {
        case PROCUI_STATUS_EXITING: {
            return false;
        }
        case PROCUI_STATUS_RELEASE_FOREGROUND: {
            ProcUIDrawDoneRelease();
            break;
        }
        case PROCUI_STATUS_IN_FOREGROUND: {
            break;
        }
        case PROCUI_STATUS_IN_BACKGROUND:
        default:
            break;
    }
    return true;
}

extern "C" uint32_t textStart();

std::string EnvironmentSelectionScreen(const std::map<std::string, std::string> &payloads, int32_t autobootIndex);
extern "C" void __fini();
extern "C" void __init_wut_malloc();

void AbortQuickStartMenu() {
    CCRCDCDrcState state = {};
    CCRCDCSysGetDrcState(CCR_CDC_DESTINATION_DRC0, &state);
    if (state.state == CCR_CDC_DRC_STATE_SUBACTIVE) {
        state.state = CCR_CDC_DRC_STATE_ACTIVE;
        CCRCDCSysSetDrcState(CCR_CDC_DESTINATION_DRC0, &state);
    }
}

std::string EnvironmentSelectionScreen(const std::map<std::string, std::string> &payloads, int32_t autobootIndex) {
    AbortQuickStartMenu();

    OSScreenInit();

    uint32_t tvBufferSize  = OSScreenGetBufferSizeEx(SCREEN_TV);
    uint32_t drcBufferSize = OSScreenGetBufferSizeEx(SCREEN_DRC);

    auto *screenBuffer = (uint8_t *) memalign(0x100, tvBufferSize + drcBufferSize);
    if (!screenBuffer) {
        OSFatal("CatyNetwork: Fail to allocate screenBuffer");
    }
    memset(screenBuffer, 0, tvBufferSize + drcBufferSize);

    OSScreenSetBufferEx(SCREEN_TV, screenBuffer);
    OSScreenSetBufferEx(SCREEN_DRC, screenBuffer + tvBufferSize);

    OSScreenEnableEx(SCREEN_TV, TRUE);
    OSScreenEnableEx(SCREEN_DRC, TRUE);

    DrawUtils::initBuffers(screenBuffer, tvBufferSize, screenBuffer + tvBufferSize, drcBufferSize);

    if (!DrawUtils::initFont()) {
        OSFatal("CatyNetwork: Failed to init font");
    }

    uint32_t selected = autobootIndex > 0 ? autobootIndex : 0;
    int autoBoot      = autobootIndex;

    {
        PairMenu pairMenu;
        while (true) {
            if (pairMenu.ProcessPairScreen()) {
                continue;
            }

            InputUtils::InputData input = InputUtils::getControllerInput();

            if (input.trigger & VPAD_BUTTON_UP) {
                if (selected > 0) {
                    selected--;
                }
            } else if (input.trigger & VPAD_BUTTON_DOWN) {
                if (selected < payloads.size() - 1) {
                    selected++;
                }
            } else if (input.trigger & VPAD_BUTTON_A) {
                break;
            } else if (input.trigger & (VPAD_BUTTON_X | VPAD_BUTTON_MINUS)) {
                autoBoot = -1;
            } else if (input.trigger & (VPAD_BUTTON_Y | VPAD_BUTTON_PLUS)) {
                autoBoot = selected;
            }


            DrawUtils::beginDraw();
            DrawUtils::clear(COLOR_BACKGROUND);

            uint32_t index = 8 + 24 + 8 + 4;
            uint32_t i     = 0;
            if (!payloads.empty()) {
                for (auto const &[key, val] : payloads) {
                    if (i == selected) {
                        DrawUtils::drawRect(16, index, SCREEN_WIDTH - 16 * 2, 44, 4, COLOR_BORDER_HIGHLIGHTED);
                    } else {
                        DrawUtils::drawRect(16, index, SCREEN_WIDTH - 16 * 2, 44, 2, ((int32_t) i == autoBoot) ? COLOR_AUTOBOOT : COLOR_BORDER);
                    }

                    DrawUtils::setFontSize(24);
                    DrawUtils::setFontColor(((int32_t) i == autoBoot) ? COLOR_AUTOBOOT : COLOR_TEXT);
                    DrawUtils::print(16 * 2, index + 8 + 24, key.c_str());
                    index += 42 + 8;
                    i++;
                }
            } else {
                DrawUtils::setFontSize(24);
                DrawUtils::setFontColor(COLOR_RED);
                const char *noEnvironmentsWarning = "No valid environments found. Press \ue000 to quit the app.";
                DrawUtils::print(SCREEN_WIDTH / 2 + DrawUtils::getTextWidth(noEnvironmentsWarning) / 2, SCREEN_HEIGHT / 2, noEnvironmentsWarning, true);
            }

            DrawUtils::setFontColor(COLOR_TEXT);

            DrawUtils::setFontSize(24);
            DrawUtils::print(16, 6 + 24, "CatyNetwork V3");
            DrawUtils::drawRectFilled(8, 8 + 24 + 4, SCREEN_WIDTH - 8 * 2, 3, COLOR_WHITE);
            DrawUtils::setFontSize(16);
            DrawUtils::print(SCREEN_WIDTH - 16, 6 + 24, CATYNETWORK_VERSION CATYNETWORK_VERSION_EXTRA, true);

            DrawUtils::drawRectFilled(8, SCREEN_HEIGHT - 24 - 8 - 4, SCREEN_WIDTH - 8 * 2, 3, COLOR_WHITE);
            DrawUtils::setFontSize(18);
            if (!payloads.empty()) {
                DrawUtils::print(16, SCREEN_HEIGHT - 8, "\ue07d Navigate ");
                DrawUtils::print(SCREEN_WIDTH - 16, SCREEN_HEIGHT - 8, "\ue000 Start", true);
                const char *autobootHints = "\ue002/\ue046 Clear Default / \ue003/\ue045 Select Default";
                DrawUtils::print(SCREEN_WIDTH / 2 + DrawUtils::getTextWidth(autobootHints) / 2, SCREEN_HEIGHT - 8, autobootHints, true);
            } else {
                DrawUtils::print(SCREEN_WIDTH - 20, SCREEN_HEIGHT - 8, "\ue000 Wii U Menu", true);
            }

            DrawUtils::endDraw();
        }
    }

    DrawUtils::beginDraw();
    DrawUtils::clear(COLOR_BLACK);
    DrawUtils::endDraw();

    DrawUtils::deinitFont();

    GX2Init(nullptr);

    GX2SetTVEnable(FALSE);
    GX2SetDRCEnable(FALSE);

    free(screenBuffer);

    if (autoBoot != autobootIndex) {
        if (autoBoot == -1) {
            DEBUG_FUNCTION_LINE("-1");
        } else {
            int i = 0;
            for (auto const &[key, val] : payloads) {
                if (i == autoBoot) {
                    DEBUG_FUNCTION_LINE("Save config");
                    break;
                }
                i++;
            }
        }
    }

    uint32_t i = 0;
    for (auto const &[key, val] : payloads) {
        if (i == selected) {
            return val;
        }
        i++;
    }

    return "";
}

int main(int argc, char **argv) {
    __init_wut_malloc();
    initLogging();

    if (IOS_Open((char *) ("/dev/iosuhax"), static_cast<IOSOpenMode>(0)) >= 0) {
        auto checkTiramisuHBL = fopen("fs:/vol/external01/wiiu/environments/tiramisu/modules/setup/50_hbl_installer.rpx", "r");
        if (checkTiramisuHBL != nullptr) {
            fclose(checkTiramisuHBL);
            OSFatal("Don't run the CatyNetwork twice.");
        } else {
            OSFatal("Don't run the CatyNetwork twice.");
        }
    }

    char environmentPathFromIOSU[0x100] = {};
    auto handle                         = IOS_Open("/dev/mcp", IOS_OPEN_READ);
    if (handle >= 0) {
        int in = 0xF9;
        if (IOS_Ioctl(handle, 100, &in, sizeof(in), environmentPathFromIOSU, sizeof(environmentPathFromIOSU)) == IOS_ERROR_OK) {
            DEBUG_FUNCTION_LINE("Boot into %s", environmentPathFromIOSU);
        }

        IOS_Close(handle);
    }

    bool noEnvironmentsFound = false;
    bool shownMenu           = false;

    std::string environmentPath = std::string(environmentPathFromIOSU);
    if (!environmentPath.starts_with("fs:/vol/external01/wiiu/environments/")) {
        DirList environmentDirs("fs:/vol/external01/wiiu/environments/", nullptr, DirList::Dirs, 1);

        std::map<std::string, std::string> environmentPaths;
        for (int i = 0; i < environmentDirs.GetFilecount(); i++) {
            environmentPaths[environmentDirs.GetFilename(i)] = environmentDirs.GetFilepath(i);
        }

        bool forceMenu     = true;
        auto autobootIndex = -1;

        InputUtils::Init();

        InputUtils::InputData input = InputUtils::getControllerInput();

        if (forceMenu || ((input.trigger | input.hold) & VPAD_BUTTON_X) == VPAD_BUTTON_X) {
            shownMenu = true;
            environmentPath = EnvironmentSelectionScreen(environmentPaths, autobootIndex);
            if (environmentPaths.empty()) {
                noEnvironmentsFound = true;
            } else {
                DEBUG_FUNCTION_LINE_VERBOSE("Selected %s", environmentPath.c_str());
            }
        } else {
        }
        InputUtils::DeInit();
    }

    if (!shownMenu) {
        OSScreenInit();

        GX2Init(nullptr);

        GX2SetTVEnable(FALSE);
        GX2SetDRCEnable(FALSE);
    }

    RevertMainHook();

    if (!noEnvironmentsFound) {
        DirList setupModules(environmentPath + "/modules/setup", ".rpx", DirList::Files, 1);
        setupModules.SortList();

        for (int i = 0; i < setupModules.GetFilecount(); i++) {
            if (setupModules.GetFilename(i)[0] == '.' || setupModules.GetFilename(i)[0] == '_') {
                DEBUG_FUNCTION_LINE_ERR("Skip file %s", setupModules.GetFilepath(i));
                continue;
            }
        }

    } else {
        ProcUIInit(OSSavesDone_ReadyToRelease);
        for (int i = 0; i < argc; i++) {
            if (strcmp(argv[i], "void forceDefaultTitleIDToWiiUMenu(void)") == 0) {
                if ((i + 1) < argc) {
                    i++;
                    DEBUG_FUNCTION_LINE_VERBOSE("call forceDefaultTitleIDToWiiUMenu");
                    auto forceDefaultTitleIDToWiiUMenu = (void(*)()) argv[i];
                    forceDefaultTitleIDToWiiUMenu();
                }
            }
        }
        SYSLaunchMenu();
    }

    ProcUIInit(OSSavesDone_ReadyToRelease);

    while (CheckRunning()) {
        OSSleepTicks(OSMillisecondsToTicks(100));
    }
    ProcUIShutdown();

    deinitLogging();
    __fini();
    return 0;
}