// SPDX-License-Identifier: GPL-3.0-only
#ifndef POCKET_INSTALLD_WORKER_ENV_H
#define POCKET_INSTALLD_WORKER_ENV_H
#include <errno.h>
#include <stdlib.h>
#include <string.h>

// Build a child environment without changing the multi-threaded parent.
// These adapters belong to daemon service namespaces, not ART workers.
struct PocketInstalldWorkerEnv {
    char** values;
    char* tokens;
    char* preload;
};
static inline void pocketInstalldFreeWorkerEnv(struct PocketInstalldWorkerEnv* env) {
    free(env->values);free(env->tokens);free(env->preload);
    memset(env,0,sizeof(*env));
}
static inline int pocketInstalldBuildWorkerEnv(char* const original[],struct PocketInstalldWorkerEnv* env) {
    memset(env,0,sizeof(*env));
    size_t count=0;const char* old=NULL;
    while(original[count]) {
        if(!old && !strncmp(original[count],"LD_PRELOAD=",11))old=original[count]+11;
        count++;
    }
    env->values=(char**)calloc(count+1,sizeof(char*));
    if(old) {
        env->tokens=strdup(old);
        env->preload=(char*)calloc(strlen(old)+sizeof("LD_PRELOAD="),1);
    }
    if(!env->values || (old && (!env->tokens || !env->preload))) {
        pocketInstalldFreeWorkerEnv(env);errno=ENOMEM;return -1;
    }
    if(old) {
        strcpy(env->preload,"LD_PRELOAD=");
        char* cursor=NULL;
        for(char* item=strtok_r(env->tokens,": \t",&cursor);item;item=strtok_r(NULL,": \t",&cursor)) {
            if(!strcmp(item,"/system/lib64/libpocket_binder_api32.so") ||
               !strcmp(item,"/system/lib64/libpocket_hidl_api32.so") ||
               !strcmp(item,"/system/lib64/libpocket_policy_api32.so"))continue;
            if(env->preload[11])strcat(env->preload,":");
            strcat(env->preload,item);
        }
    }
    size_t next=0;
    for(size_t i=0;i<count;i++)if(strncmp(original[i],"LD_PRELOAD=",11))env->values[next++]=original[i];
    if(old && env->preload[11])env->values[next++]=env->preload;
    return 0;
}
#endif
