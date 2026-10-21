// SPDX-License-Identifier: GPL-3.0-only
#include <signal.h>
#include <stdio.h>
static volatile sig_atomic_t handled;
static void app_handler(int signal) {handled=signal;}
int main(void) {
    struct sigaction previous={0},own={0};
    if(sigaction(SIGSEGV,NULL,&previous) || previous.sa_handler!=SIG_DFL) {
        puts("FAIL: runtime owns SIGSEGV by default");return 1;
    }
    own.sa_handler=app_handler;sigemptyset(&own.sa_mask);
    if(sigaction(SIGSEGV,&own,NULL) || raise(SIGSEGV) || handled!=SIGSEGV)return 2;
    puts("PASS: default SIGSEGV untouched; app signal handler returns normally");return 0;
}
