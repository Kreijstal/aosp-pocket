// SPDX-License-Identifier: GPL-3.0-only
// Rootless entry point for the pinned AOSP Android 9 parser/action/service core.
// It is a child supervisor, not PID 1, and cannot mount or change host policy.
#include "action_manager.h"
#include "action_parser.h"
#include "import_parser.h"
#include "parser.h"
#include "service.h"
#include "util.h"
#include "property_service.h"
#include <android-base/logging.h>
#include <android-base/properties.h>
#define _REALLY_INCLUDE_SYS__SYSTEM_PROPERTIES_H_
#include <sys/_system_properties.h>
#include <sys/wait.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include <chrono>
#include <map>
#include <set>

// This header is ours, not AOSP's same-named header.
#include "pocket_properties.h"

namespace android { namespace init {
const std::string kInitContext = ""; // No guest SELinux domains are manufactured.
const std::string kVendorContext = "";
std::string default_console;
uint32_t (*property_set)(const std::string&, const std::string&) = pocketPropertySet;
// No subcontext is constructed: fail explicitly if that invariant ever changes.
Result<Success> Subcontext::Execute(const std::vector<std::string>&) {
    return Error() << "SELinux subcontexts unsupported in rootless mode";
}
Result<std::vector<std::string>> Subcontext::ExpandArgs(const std::vector<std::string>&) {
    return Error() << "SELinux subcontexts unsupported in rootless mode";
}
} }
using namespace android::init;
using android::base::GetProperty;
namespace {
volatile sig_atomic_t stopping;
void stop(int) { stopping = 1; }
unsigned reaped;
bool failed;
std::set<pid_t> children;

uint32_t control(const std::string& command, const std::string& name) {
    auto* service = ServiceList::GetInstance().FindService(name);
    if (!service) return PROP_ERROR_INVALID_VALUE;
    if (command == "start") {
        auto result = service->Start();
        if (!result) { LOG(ERROR) << result.error(); return PROP_ERROR_SET_FAILED; }
    } else if (command == "stop") service->Stop();
    else if (command == "restart") service->Restart();
    else return PROP_ERROR_PERMISSION_DENIED;
    printf("INIT_CONTROL command=%s service=%s\n", command.c_str(), name.c_str());
    return PROP_SUCCESS;
}

// The upstream action queue performs argument expansion and trigger matching.
// Only the builtins whose effects are meaningful under an app UID are exposed.
class Functions final : public KeywordFunctionMap {
    const Map& map() const override {
        static const Map functions = {
            {"setprop", {2, 2, {false, [](const BuiltinArguments& a) -> Result<Success> {
                auto code = pocketPropertySet(a[1], a[2]);
                if (code != PROP_SUCCESS) { failed = true; return Error() << "setprop error " << code; }
                return Success();
            }}}},
            {"trigger", {1, 1, {false, [](const BuiltinArguments& a) -> Result<Success> {
                ActionManager::GetInstance().QueueEventTrigger(a[1]); return Success();
            }}}},
            {"start", {1, 1, {false, [](const BuiltinArguments& a) -> Result<Success> {
                if (control("start", a[1]) != PROP_SUCCESS) { failed = true; return Error() << "start failed"; }
                return Success();
            }}}},
            {"stop", {1, 1, {false, [](const BuiltinArguments& a) -> Result<Success> {
                if (control("stop", a[1]) != PROP_SUCCESS) { failed = true; return Error() << "stop failed"; }
                return Success();
            }}}},
            {"class_start", {1, 1, {false, [](const BuiltinArguments& a) -> Result<Success> {
                for (const auto& service : ServiceList::GetInstance()) {
                    if (!service->classnames().count(a[1])) continue;
                    auto result = service->StartIfNotDisabled();
                    if (!result) { failed = true; return result; }
                }
                return Success();
            }}}},
        };
        return functions;
    }
};

// Same waitid(WNOWAIT) ordering as AOSP sigchld_handler.cpp: keep the zombie's
// PID allocated until Service::Reap has signalled its remaining process group.
void reap() {
    for (;;) {
        siginfo_t info{};
        if (waitid(P_ALL, 0, &info, WEXITED | WNOHANG | WNOWAIT)) {
            if (errno != ECHILD && errno != EINTR) failed = true;
            return;
        }
        if (!info.si_pid) return;
        auto* service = ServiceList::GetInstance().FindService(info.si_pid, &Service::pid);
        if (service) {
            printf("INIT_REAP service=%s pid=%d code=%d status=%d\n", service->name().c_str(),
                   info.si_pid, info.si_code, info.si_status);
            service->Reap(info);
        } else { failed = true; printf("INIT_UNTRACKED_CHILD pid=%d\n", info.si_pid); }
        while (waitpid(info.si_pid, nullptr, 0) < 0 && errno == EINTR) {}
        children.erase(info.si_pid);
        ++reaped;
    }
}

void rememberChildren() {
    for (const auto& service : ServiceList::GetInstance())
        if (service->pid() > 1 && children.insert(service->pid()).second)
            printf("INIT_CHILD service=%s pid=%d\n", service->name().c_str(), service->pid());
}
}

int main(int argc, char** argv) {
    setbuf(stdout, nullptr);
    android::base::InitLogging(argv, android::base::StderrLogger);
    signal(SIGTERM, stop); signal(SIGINT, stop); signal(SIGPIPE, SIG_IGN);
    // Poll/reap avoids a thread and a lost-SIGCHLD window; no PID-1 assumption.
    signal(SIGCHLD, [](int) {});
    if (getuid() != 0 || argc != 2) return 64;
    Functions functions;
    Action::set_function_map(&functions);
    auto& actions = ActionManager::GetInstance();
    auto& services = ServiceList::GetInstance();
    if (pocketPropertiesStart([&actions](const std::string& name, const std::string& value) {
            actions.QueuePropertyChange(name, value);
        }, control)) return 65;
    Parser parser;
    parser.AddSectionParser("service", std::make_unique<ServiceParser>(&services, nullptr));
    parser.AddSectionParser("on", std::make_unique<ActionParser>(&actions, nullptr));
    parser.AddSectionParser("import", std::make_unique<ImportParser>(&parser));
    size_t errors = 0;
    bool parsed = parser.ParseConfig(argv[1], &errors);
    if (!parsed || errors || parser.errors()) {
        printf("INIT_CONFIG_REJECTED errors=%zu parsed=%d\n", parser.errors(), parsed);
        pocketPropertiesClose();
        return 66; // No service is launched from a partly accepted configuration.
    }
    actions.ClearQueue();
    actions.QueueEventTrigger("early-init");
    actions.QueueEventTrigger("init");
    actions.QueueEventTrigger("boot");
    actions.QueueAllPropertyActions();
    printf("API28_INIT_READY pid=%d sdk=%s pid1=0\n", getpid(), GetProperty("ro.build.version.sdk", "").c_str());
    while (!stopping && !pocketPropertiesStopped() && !failed) {
        if (actions.HasMoreCommands()) actions.ExecuteOneCommand();
        rememberChildren();
        reap();
        // Android 9's five-second minimum restart interval.
        for (const auto& service : services) {
            if ((service->flags() & SVC_RESTARTING) &&
                boot_clock::now() >= service->time_started() + std::chrono::seconds(5)) {
                auto result = service->Start();
                if (!result) { failed = true; LOG(ERROR) << result.error(); }
            }
        }
        rememberChildren();
        if (!pocketPropertiesPoll(actions.HasMoreCommands() ? 0 : 50)) failed = true;
    }
    actions.ClearQueue();
    // Disable all restarts before terminating children in reverse start order.
    auto shutdown = services.services_in_shutdown_order();
    for (const auto& service : services) if (!service->pid()) service->Stop();
    for (auto* service : shutdown) service->Terminate();
    auto deadline = boot_clock::now() + std::chrono::seconds(2);
    while (!children.empty() && boot_clock::now() < deadline) { reap(); usleep(10000); }
    for (auto* service : shutdown) if (service->pid()) service->Stop();
    deadline = boot_clock::now() + std::chrono::seconds(2);
    while (!children.empty() && boot_clock::now() < deadline) { reap(); usleep(10000); }
    printf("API28_INIT_STOPPED reaped=%u remaining=%zu failed=%d\n", reaped, children.size(), failed);
    pocketPropertiesClose();
    return failed || !children.empty() ? 67 : 0;
}
