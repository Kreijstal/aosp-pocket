// SPDX-License-Identifier: GPL-3.0-only
// Owner-console test/rollback helper. No interpreter, libc, TLS or host root.
#include <stdint.h>
#include <stddef.h>
#include <sys/syscall.h>
#include <fcntl.h>
static long call(long nr,long a,long b,long c,long d,long e,long f) {
 register long x0 __asm__("x0")=a,x1 __asm__("x1")=b,x2 __asm__("x2")=c;
 register long x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
 register long x8 __asm__("x8")=nr;
 __asm__ volatile("svc #0":"+r"(x0):"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5),"r"(x8):"memory","cc");return x0;
}
static size_t size(const char*s){size_t n=0;while(n<4096 && s[n])n++;return n;}
static int equal(const char*a,const char*b){size_t i=0;while(a[i] && a[i]==b[i])i++;return a[i]==b[i];}
static int prefix(const char*a,const char*b){size_t i=0;while(b[i] && a[i]==b[i])i++;return !b[i];}
__attribute__((noreturn)) static void finish(long code) {
 if(code){const char text[]="native loader launcher failed\n";call(__NR_write,2,(long)text,sizeof(text)-1,0,0,0);}
 call(__NR_exit_group,code,0,0,0,0,0);for(;;){}
}
__attribute__((noreturn)) void launch(uintptr_t*stack) {
 size_t argc=stack[0];if(argc<4 || argc>128)finish(64);
 char**argv=(char**)(stack+1);
 if(equal(argv[1],"restore")) {
  if(argc!=4 || argv[2][0]!='/' || argv[3][0]!='/')finish(64);
  long rc=call(__NR_renameat,AT_FDCWD,(long)argv[2],AT_FDCWD,(long)argv[3],0,0);finish(rc<0?1:0);
 }
 if(!equal(argv[1],"run") || argc<4 || argv[2][0]!='/' || argv[3][0]!='/')finish(64);
 const char key[]="POCKET_LOADER_EXEC_V1=";char request[4096];size_t at=sizeof(key)-1,n=size(argv[3]);
 if(n>=sizeof(request)-at)finish(65);
 for(size_t i=0;i<at;i++)request[i]=key[i];for(size_t i=0;i<=n;i++)request[at+i]=argv[3][i];
 char*environment[260];char**old=argv+argc+1;size_t used=0;
 for(size_t i=0;old[i];i++) {
  if(i>=256)finish(66);
  if(prefix(old[i],"POCKET_LOADER_EXEC_V1=") || prefix(old[i],"POCKET_LOADER_STATE_V1="))continue;
  environment[used++]=old[i];
 }
 environment[used++]=request;environment[used++]="POCKET_LOADER_BOOT=1";environment[used]=0;
 call(__NR_execve,(long)argv[2],(long)(argv+3),(long)environment,0,0,0);finish(67);
}
__asm__(".text\n.global _start\n.type _start,%function\n_start:\nmov x0, sp\nbl launch\nbrk #0\n");
