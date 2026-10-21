// SPDX-License-Identifier: GPL-3.0-only
#define _GNU_SOURCE
#include <dlfcn.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
static volatile sig_atomic_t handled;
static void app_handler(int signal) {handled=signal;}
int main(void) {
    struct sigaction previous={0},own={0};
    if(sigaction(SIGSEGV,NULL,&previous))return 1;
    // Android's linker normally installs debuggerd's handler. Require Pocket
    // to leave that platform handler alone, rather than assuming SIG_DFL.
    Dl_info owner={0};
    if(previous.sa_handler!=SIG_DFL && previous.sa_handler!=SIG_IGN &&
       dladdr((void*)previous.sa_sigaction,&owner)) {
        printf("Existing SIGSEGV handler: %s\n",owner.dli_fname);
        if(strstr(owner.dli_fname,"libpocket_direct_boot.so")) {
            puts("FAIL: runtime owns SIGSEGV by default");return 1;
        }
    }
    own.sa_handler=app_handler;sigemptyset(&own.sa_mask);
    if(sigaction(SIGSEGV,&own,NULL) || raise(SIGSEGV) || handled!=SIGSEGV)return 2;
    puts("PASS: platform SIGSEGV handler retained; app signal handler returns normally");return 0;
}
