#include "core/console_args.h"

#include <cassert>

static bool expectedGui(bool mapper) { return !mapper; }

int main() {
    char demo[] = "-demo";
    char demoPath[] = "sample.rec";
    char* demoArgv[] = {const_cast<char*>("torch"), demo, demoPath};
    assert(findDemoLaunchPath(3, demoArgv) == "sample.rec");

    char playdemo[] = "-playdemo";
    char* playdemoArgv[] = {const_cast<char*>("torch"), playdemo, demoPath};
    assert(findDemoLaunchPath(3, playdemoArgv) == "sample.rec");
    char longDemo[] = "--demo";
    char* longDemoArgv[] = {const_cast<char*>("torch"), longDemo, demoPath};
    assert(findDemoLaunchPath(3, longDemoArgv) == "sample.rec");
    char* incompleteArgv[] = {const_cast<char*>("torch"), playdemo};
    assert(findDemoLaunchPath(2, incompleteArgv).empty());
    char demoMode[] = "-demo-mode";
    char* buildModeArgv[] = {const_cast<char*>("torch"), demoMode};
    assert(findDemoLaunchPath(2, buildModeArgv).empty());
    char* mixedArgv[] = {const_cast<char*>("torch"), demoMode, demo, demoPath};
    assert(findDemoLaunchPath(4, mixedArgv) == "sample.rec");
    assert(shouldSkipLogin(false, false, false) == false);
    assert(shouldSkipLogin(true, false, false));
    assert(shouldSkipLogin(false, true, false));
    assert(shouldSkipLogin(false, false, true));

    uint16_t port = 0;
    assert(parseConsolePort("28000", port) && port == 28000);
    assert(!parseConsolePort("28000junk", port));
    assert(!parseConsolePort("0", port));
    assert(!parseConsolePort("65536", port));

    int frames = -1;
    assert(parseNonNegativeInt("0", frames) && frames == 0);
    assert(parseNonNegativeInt("12", frames) && frames == 12);
    assert(!parseNonNegativeInt("-1", frames));
    assert(!parseNonNegativeInt("12junk", frames));

    std::string host;
    assert(parseConsoleHostPort("example.test:28000", host, port));
    assert(host == "example.test" && port == 28000);
    assert(!parseConsoleHostPort("example.test", host, port));
    assert(!parseConsoleHostPort("a:b:28000", host, port));

    assert(selectMasterServerUrl(true, "", "https://demo.example/list", "https://retail.example/list") ==
           "https://demo.example/list");
    assert(selectMasterServerUrl(false, "", "https://demo.example/list", "https://retail.example/list") ==
           "https://retail.example/list");
    assert(selectMasterServerUrl(false, "https://explicit.example/list", "demo", "retail") ==
           "https://explicit.example/list");
    assert(selectMasterServerUrl(true, "", "", "retail").empty());
    assert(!allowDemoConnection(true, false, false, true));
    assert(allowDemoConnection(true, true, false, true));
    assert(!allowDemoConnection(true, true, false, false));
    assert(allowDemoConnection(true, false, true, false));
    assert(allowDemoConnection(false, false, false, false));

    // Exercise every combination of launch mode, network/login flags, and
    // demo policy. These are pure launch invariants, so the matrix is stable
    // without depending on SDL, assets, or a network service.
    for (int demoMode = 0; demoMode <= 1; ++demoMode) {
        for (int playback = 0; playback <= 1; ++playback) {
            for (int online = 0; online <= 1; ++online) {
                for (int requestedNoLogin = 0; requestedNoLogin <= 1; ++requestedNoLogin) {
                    for (int mapper = 0; mapper <= 1; ++mapper) {
                        for (int preview = 0; preview <= 1; ++preview) {
                            for (int allowConnect = 0; allowConnect <= 1; ++allowConnect) {
                                for (int allowWatch = 0; allowWatch <= 1; ++allowWatch) {
                                    const bool skip = shouldSkipLogin(
                                        demoMode, playback,
                                        requestedNoLogin || mapper || preview);
                                    assert(skip == (demoMode || playback || requestedNoLogin ||
                                                    mapper || preview));
                                    assert(expectedGui(mapper) == !mapper);
                                    assert(online == 0 || online == 1);
                                    assert(selectMasterServerUrl(
                                        demoMode, "", "demo-master", "retail-master") ==
                                        (demoMode ? "demo-master" : "retail-master"));
                                    assert(allowDemoConnection(
                                        demoMode, false, allowConnect, allowWatch) ==
                                        (!demoMode || allowConnect));
                                    assert(allowDemoConnection(
                                        demoMode, true, allowConnect, allowWatch) ==
                                        (!demoMode || allowWatch));
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    return 0;
}
