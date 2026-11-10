// SPDX-License-Identifier: GPL-3.0-only
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
static int guest_identity=1;
static unsigned guest_uid[3];
static long raw(long nr,long a,long b,long c,long d,long e,long f){long n=syscall(nr,a,b,c,d,e,f);return n<0?-errno:n;}
#include "../loader/external_kernel_scope.h"
static pthread_barrier_t entered,released;
static volatile sig_atomic_t signals,failed;
static void handler(int signal) {
    (void)signal;
    if(external_kernel_scope(1)!=0 || !external_kernel_active() || external_kernel_scope(0)!=0)failed=1;
    signals++;
}
static void* worker(void* unused) {
    (void)unused;assert(!external_kernel_active() && !external_kernel_scope(1));pthread_barrier_wait(&entered);
    pthread_barrier_wait(&released);
    // The lower slot has become free while this thread still owns a slot.
    // Nesting must use that existing scope, enforcing the actual depth bound.
    for(unsigned i=1;i<32;i++)assert(!external_kernel_scope(1));
    assert(external_kernel_scope(1)==-EOVERFLOW);
    for(unsigned i=0;i<32;i++)assert(!external_kernel_scope(0));
    assert(!external_kernel_active());return NULL;
}
int main(void) {
    assert(!external_kernel_active() && !external_kernel_scope(0));
    assert(!external_kernel_scope(1) && external_kernel_active());
    assert(!pthread_barrier_init(&entered,NULL,2) && !pthread_barrier_init(&released,NULL,2));pthread_t thread;assert(!pthread_create(&thread,NULL,worker,NULL));
    pthread_barrier_wait(&entered);assert(!external_kernel_scope(0) && !external_kernel_active());pthread_barrier_wait(&released);assert(!pthread_join(thread,NULL));
    assert(!pthread_barrier_destroy(&entered) && !pthread_barrier_destroy(&released));
    assert(!external_kernel_scope(1));pid_t child=fork();assert(child>=0);
    if(!child){if(external_kernel_active() || external_kernel_scope(0) || external_kernel_scope(1) || !external_kernel_active() || external_kernel_scope(0) || external_kernel_active())_exit(1);_exit(0);}
    int status;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);assert(external_kernel_active() && !external_kernel_scope(0));
    struct sigaction action={0};action.sa_handler=handler;action.sa_flags=SA_NODEFER;sigemptyset(&action.sa_mask);assert(!sigaction(SIGUSR1,&action,NULL));
    for(unsigned i=0;i<1000;i++){assert(!external_kernel_scope(1));assert(!raise(SIGUSR1));assert(external_kernel_active() && !external_kernel_scope(0));}
    assert(signals==1000 && !failed && !external_kernel_active());
    guest_uid[1]=10001;assert(external_kernel_scope(1)==-EPERM && !external_kernel_active());guest_uid[1]=0;
    guest_identity=0;assert(external_kernel_scope(1)==-EPERM && !external_kernel_active());guest_identity=1;
    puts("EXTERNAL_KERNEL_SCOPE_PASS thread_isolation=1 nesting_limit=1 freed_slot_not_duplicate=1 fork_reset=1 nested_signal=1 ordinary_app_denied=1 host_privilege_unchanged=1");
}
