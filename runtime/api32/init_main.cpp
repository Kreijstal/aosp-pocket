// SPDX-License-Identifier: GPL-3.0-only
// Rootless entry point for the pinned AOSP Android 12L parser/action/service core.
// It is a child supervisor, not PID 1, and cannot mount or change host policy.
#include "action_manager.h"
#include "action_parser.h"
#include "import_parser.h"
#include "parser.h"
#include "service.h"
#include "service_list.h"
#include "service_parser.h"
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
#include "bootstrap_ready.h"
#include "owned_children.h"
#ifndef POCKET_GUEST_API
#define POCKET_GUEST_API 32
#endif
#define POCKET_INIT_STRING_INNER(value) #value
#define POCKET_INIT_STRING(value) POCKET_INIT_STRING_INNER(value)

namespace android { namespace init {
bool Subcontext::PathMatchesSubcontext(const std::string&) { return false; }
void Subcontext::Fork() { LOG(FATAL) << "Host SELinux subcontexts unavailable"; }
// No subcontext is constructed: fail explicitly if that invariant ever changes.
Result<void> Subcontext::Execute(const std::vector<std::string>&) {
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
unsigned descendantsReaped;
bool failed;
std::set<pid_t> children;

uint32_t control(const std::string& command, const std::string& name) {
    if(stopping && command!="stop")return PROP_ERROR_PERMISSION_DENIED;
    auto* service = ServiceList::GetInstance().FindService(name);
    if (!service) return PROP_ERROR_INVALID_VALUE;
    if (command == "start") {
        auto result = service->Start();
        if (!result.ok()) { LOG(ERROR) << result.error(); return PROP_ERROR_SET_FAILED; }
    } else if (command == "stop") service->Stop();
    else if (command == "restart") service->Restart();
    else return PROP_ERROR_PERMISSION_DENIED;
    printf("INIT_CONTROL command=%s service=%s\n", command.c_str(), name.c_str());
    return PROP_SUCCESS;
}

// The upstream action queue performs argument expansion and trigger matching.
// Only the builtins whose effects are meaningful under an app UID are exposed.
const BuiltinFunctionMap functions = {
            {"setprop", {2, 2, {false, [](const BuiltinArguments& a) -> Result<void> {
                auto code = pocketPropertySet(a[1], a[2]);
                if (code != PROP_SUCCESS) { failed = true; return Error() << "setprop error " << code; }
                return {};
            }}}},
            {"trigger", {1, 1, {false, [](const BuiltinArguments& a) -> Result<void> {
                ActionManager::GetInstance().QueueEventTrigger(a[1]); return {};
            }}}},
            {"start", {1, 1, {false, [](const BuiltinArguments& a) -> Result<void> {
                if (control("start", a[1]) != PROP_SUCCESS) { failed = true; return Error() << "start failed"; }
                return {};
            }}}},
            {"stop", {1, 1, {false, [](const BuiltinArguments& a) -> Result<void> {
                if (control("stop", a[1]) != PROP_SUCCESS) { failed = true; return Error() << "stop failed"; }
                return {};
            }}}},
            {"class_start", {1, 1, {false, [](const BuiltinArguments& a) -> Result<void> {
                for (const auto& service : ServiceList::GetInstance()) {
                    if (!service->classnames().count(a[1])) continue;
                    auto result = service->StartIfNotDisabled();
                    if (!result.ok()) { failed = true; return result; }
                }
                return {};
            }}}},
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
        const bool bootstrap=service && service->name()=="pocket-runtime-bootstrap";
        if (service) {
            printf("INIT_REAP service=%s pid=%d code=%d status=%d\n", service->name().c_str(),
                   info.si_pid, info.si_code, info.si_status);
            service->Reap(info);
            ++reaped;
        } else {
            // Subreaping makes daemonized app children ours as well. They
            // have a kernel parent relationship to this process and are
            // accounted separately from declared service lifetimes.
            ++descendantsReaped;
            printf("INIT_DESCENDANT_REAP pid=%d code=%d status=%d\n",info.si_pid,info.si_code,info.si_status);
        }
        while (waitpid(info.si_pid, nullptr, 0) < 0 && errno == EINTR) {}
        children.erase(info.si_pid);
        if(bootstrap && !stopping && !pocketPropertiesStopped()) {
            if(info.si_code!=CLD_EXITED || info.si_status!=0 ||
               !pocketPublishBootReady32(getenv("POCKET_API32_RUN_ID"),"/tmp/pocket-runtime-services-ready","/tmp/pocket-runtime-ready")) {
                failed=true;printf("API32_RUNTIME_BOOTSTRAP_FAILED pid=%d code=%d status=%d\n",info.si_pid,info.si_code,info.si_status);
            }else {
                printf("API32_RUNTIME_BOOTSTRAP_REAP pid=%d ready=1\n",info.si_pid);
                puts("POCKET_RUNTIME_BOOT_READY sdk=32 component_checks=0 owner=init");
            }
        }
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
    int ownership=pocketBecomeSubreaper32();
    if(ownership){printf("API32_INIT_SUBREAPER_FAILED error=%d\n",ownership);return 68;}
    puts("API32_INIT_SUBREAPER_READY kernel_child_ownership=1");
    Action::set_function_map(&functions);
    auto& actions = ActionManager::GetInstance();
    auto& services = ServiceList::GetInstance();
    if (pocketPropertiesStart([&actions](const std::string& name, const std::string& value) {
            actions.QueuePropertyChange(name, value);
        }, control)) return 65;
    Parser parser;
    parser.AddSectionParser("service", std::make_unique<ServiceParser>(&services, nullptr, std::nullopt));
    parser.AddSectionParser("on", std::make_unique<ActionParser>(&actions, nullptr));
    parser.AddSectionParser("import", std::make_unique<ImportParser>(&parser));
    size_t errors = 0;
    bool parsed = parser.ParseConfig(argv[1]);
    errors = parser.parse_error_count();
    if (!parsed || errors) {
        printf("INIT_CONFIG_REJECTED errors=%zu parsed=%d\n", errors, parsed);
        pocketPropertiesClose();
        return 66; // No service is launched from a partly accepted configuration.
    }
    actions.ClearQueue();
    actions.QueueEventTrigger("early-init");
    actions.QueueEventTrigger("init");
    actions.QueueEventTrigger("boot");
    actions.QueueAllPropertyActions();
    printf("API" POCKET_INIT_STRING(POCKET_GUEST_API) "_INIT_READY pid=%d sdk=%s pid1=0\n", getpid(), GetProperty("ro.build.version.sdk", "").c_str());
    while (!stopping && !pocketPropertiesStopped() && !failed) {
        if (actions.HasMoreCommands()) actions.ExecuteOneCommand();
        rememberChildren();
        reap();
        // Preserve the version-matched service restart interval.
        for (const auto& service : services) {
            if (service->IsRunning() && service->timeout_period() &&
                boot_clock::now() >= service->time_started() + *service->timeout_period())
                service->Timeout();
            if ((service->flags() & SVC_RESTARTING) &&
                boot_clock::now() >= service->time_started() + service->restart_period()) {
                auto result = service->Start();
                if (!result.ok()) { failed = true; LOG(ERROR) << result.error(); }
            }
        }
        rememberChildren();
        if (!pocketPropertiesPoll(actions.HasMoreCommands() ? 0 : 50)) failed = true;
    }
    stopping=1;
    actions.ClearQueue();
    // Disable all restarts before terminating children in reverse start order.
    auto shutdown = services.services_in_shutdown_order();
    for (const auto& service : services) if (!service->pid()) service->Stop();
    // Stop the app forker first, while Binder/hardware services still exist.
    // Its separate app process groups become this subreaper's direct children.
    std::set<pid_t> signalled;
    auto* framework=services.FindService("pocket-framework");
    if(framework && framework->pid()) {
        framework->Terminate();
        auto appDeadline=boot_clock::now()+std::chrono::seconds(1);
        while(boot_clock::now()<appDeadline) {
            reap();
            int error=pocketSignalAdopted32(children,SIGTERM,&signalled);
            if(error){failed=true;printf("API32_INIT_DESCENDANT_QUERY_FAILED error=%d\n",error);break;}
            std::set<pid_t> all;error=pocketOwnedChildren32(&all);
            if(error){failed=true;break;}
            bool adopted=false;for(pid_t pid:all)if(!children.count(pid)){adopted=true;break;}
            if(!framework->pid() && !adopted)break;
            usleep(10000);
        }
    }
    for (auto* service : shutdown) service->Terminate();
    auto deadline = boot_clock::now() + std::chrono::seconds(2);
    auto drain=[&](int signal) {
        while(boot_clock::now()<deadline) {
            reap();int error=pocketSignalAdopted32(children,signal,&signalled);
            if(error){failed=true;printf("API32_INIT_DESCENDANT_QUERY_FAILED error=%d\n",error);return;}
            std::set<pid_t> all;error=pocketOwnedChildren32(&all);
            if(error){failed=true;return;}
            if(children.empty() && all.empty())return;
            usleep(10000);
        }
    };
    drain(SIGTERM);
    for (auto* service : shutdown) if (service->pid()) service->Stop();
    deadline = boot_clock::now() + std::chrono::seconds(2);
    drain(SIGKILL);reap();
    std::set<pid_t> remaining;
    int query=pocketOwnedChildren32(&remaining);
    if(query || !remaining.empty())failed=true;
    printf("API32_INIT_DESCENDANTS_STOPPED reaped=%u signalled=%zu remaining=%zu query_error=%d\n",descendantsReaped,signalled.size(),remaining.size(),query);
    printf("API" POCKET_INIT_STRING(POCKET_GUEST_API) "_INIT_STOPPED reaped=%u remaining=%zu failed=%d\n", reaped, children.size(), failed);
    pocketPropertiesClose();
    return failed || !children.empty() ? 67 : 0;
}
