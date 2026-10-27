// SPDX-License-Identifier: GPL-3.0-only
// Deliberately limited iptables-compatible OUTPUT socket-policy frontend.
#define _GNU_SOURCE
#include "firewall.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static void die(const char* text) {fprintf(stderr,"pocket-iptables: %s\n",text);exit(2);}
static unsigned number(const char* text,unsigned max) {
    char* end;errno=0;unsigned long n=strtoul(text,&end,10);
    if(errno || !*text || *end || *text=='-' || n>max)die("invalid numeric argument");
    return (unsigned)n;
}
static unsigned action(const char* text) {
    if(!strcmp(text,"ACCEPT"))return POCKET_FW_ACCEPT;
    if(!strcmp(text,"DROP") || !strcmp(text,"REJECT"))return POCKET_FW_DENY;
    die("only ACCEPT, DROP and REJECT are supported");return 0;
}
static void list(const struct PocketFirewall* f,unsigned family) {
    printf("-P OUTPUT %s\n",(family==4?f->policy4:f->policy6)?"DROP":"ACCEPT");
    for(unsigned i=0;i<f->count;i++) {
        const struct PocketFirewallRule* r=&f->rules[i];if(r->family!=family)continue;
        printf("-A OUTPUT");
        if(r->owner)printf(" -m owner --uid-owner %u",r->uid);
        if(r->protocol)printf(" -p %s",r->protocol==6?"tcp":"udp");
        if(r->prefix) {char address[INET6_ADDRSTRLEN];
            if(!inet_ntop(family==4?AF_INET:AF_INET6,r->address,address,sizeof(address)))die("invalid stored address");
            printf(" -d %s/%u",address,r->prefix);}
        if(r->port)printf(" --dport %u",r->port);
        printf(" -j %s\n",r->action?"DROP":"ACCEPT");
    }
}
int main(int argc,char** argv) {
    if(argc==2 && (!strcmp(argv[1],"--help") || !strcmp(argv[1],"--version"))) {
        puts("pocket-iptables 1: guest OUTPUT socket firewall (IPv4/IPv6)\n"
             "iptables [-4|-6] {-A|-I} OUTPUT [-m owner --uid-owner UID] [-p tcp|udp] [-d IP[/PREFIX]] [--dport PORT] -j ACCEPT|DROP|REJECT\n"
             "iptables [-4|-6] {-D OUTPUT NUMBER|-F [OUTPUT]|-P OUTPUT ACCEPT|DROP|-S [OUTPUT]|-L [OUTPUT]}\n"
             "Only the filter table and OUTPUT are supported. Denied sends return EACCES; DROP is not a packet timeout.\n"
             "No INPUT, FORWARD, NAT, conntrack, counters or kernel isolation. Guest root can edit policy.");return 0;
    }
    const char* path=POCKET_FW_PATH;
#ifdef POCKET_FIREWALL_TEST
    path=getenv("POCKET_FIREWALL_FILE");if(!path)die("missing test file");
#else
    if(geteuid()!=0)die("guest root is required");
#endif
    const char* name=strrchr(argv[0],'/');name=name?name+1:argv[0];
    unsigned family=!strcmp(name,"ip6tables")?6:4;const char* op=NULL;const char* policy=NULL;unsigned index=0;
    struct PocketFirewallRule rule={0};int target=0,owner_module=0;
    for(int i=1;i<argc;i++) {
        const char* arg=argv[i];
        if(!strcmp(arg,"-4"))family=4;
        else if(!strcmp(arg,"-6"))family=6;
        else if(!strcmp(arg,"-t")) {if(++i==argc || strcmp(argv[i],"filter"))die("only the filter table is supported");}
        else if(!strcmp(arg,"-A") || !strcmp(arg,"-I") || !strcmp(arg,"-D") || !strcmp(arg,"-P") ||
                !strcmp(arg,"-F") || !strcmp(arg,"-S") || !strcmp(arg,"-L")) {
            if(op)die("one operation is required");
            op=arg;
            if(i+1<argc && argv[i+1][0]!='-') {if(strcmp(argv[++i],"OUTPUT"))die("only OUTPUT is supported");}
            else if(strcmp(op,"-F") && strcmp(op,"-S") && strcmp(op,"-L"))die("OUTPUT chain is required");
            if(!strcmp(op,"-D")) {if(++i==argc)die("rule number is required");index=number(argv[i],POCKET_FW_MAX);if(!index)die("rule numbers start at one");}
            if(!strcmp(op,"-P")) {if(++i==argc)die("policy is required");policy=argv[i];}
        } else if(!strcmp(arg,"-m")) {if(++i==argc || strcmp(argv[i],"owner"))die("only the owner match is supported");owner_module=1;}
        else if(!strcmp(arg,"--uid-owner")) {if(++i==argc)die("UID is required");rule.uid=number(argv[i],UINT32_MAX);rule.owner=1;}
        else if(!strcmp(arg,"-p")) {
            if(++i==argc)die("protocol is required");
            if(!strcmp(argv[i],"tcp"))rule.protocol=6;else if(!strcmp(argv[i],"udp"))rule.protocol=17;else die("only tcp and udp matches are supported");
        } else if(!strcmp(arg,"--dport")) {if(++i==argc)die("port is required");rule.port=number(argv[i],65535);if(!rule.port)die("port zero is unsupported");}
        else if(!strcmp(arg,"-d")) {
            if(++i==argc)die("destination is required");
            char address[INET6_ADDRSTRLEN+5];if(strlen(argv[i])>=sizeof(address))die("invalid destination");strcpy(address,argv[i]);
            char* slash=strchr(address,'/');if(slash)*slash++=0;
            rule.family=strchr(address,':')?6:4;
            rule.prefix=slash?number(slash,rule.family==4?32:128):(rule.family==4?32:128);
            if(inet_pton(rule.family==4?AF_INET:AF_INET6,address,rule.address)!=1)die("destination must be a numeric IP address");
        } else if(!strcmp(arg,"-j")) {if(++i==argc)die("target is required");rule.action=action(argv[i]);target=1;}
        else die("unsupported argument (see --help)");
    }
    if(!op)die("operation is required");
    int editing=!strcmp(op,"-A") || !strcmp(op,"-I");
    if(editing && !target)die("target is required");
    if(!editing && (target || rule.owner || owner_module || rule.protocol || rule.family || rule.port))die("matches require -A or -I");
    if(rule.family && rule.family!=family)die("destination and IP version disagree");
    rule.family=family;
    if(rule.port && !rule.protocol)die("--dport requires -p tcp or udp");
    if(owner_module && !rule.owner)die("owner match requires --uid-owner");
    char lockpath[4096],temporary[4096];
    if(snprintf(lockpath,sizeof(lockpath),"%s.lock",path)>=(int)sizeof(lockpath) ||
       snprintf(temporary,sizeof(temporary),"%s.%ld.tmp",path,(long)getpid())>=(int)sizeof(temporary))die("path is too long");
#ifndef POCKET_FIREWALL_TEST
    if(mkdir("/data/misc",0700) && errno!=EEXIST)die("cannot create policy directory");
#endif
    int lock=open(lockpath,O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
    if(lock<0 || flock(lock,LOCK_EX))die("cannot lock policy");
    struct PocketFirewall f={.magic=POCKET_FW_MAGIC,.version=POCKET_FW_VERSION};
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd>=0) {
        size_t used=0;ssize_t n;
        while(used<sizeof(f) && (n=read(fd,(char*)&f+used,sizeof(f)-used))>0)used+=(size_t)n;
        char extra;int valid=used==sizeof(f) && read(fd,&extra,1)==0 && pocket_fw_valid(&f);close(fd);
        if(!valid)die("policy file is corrupt; refusing to overwrite");
    } else if(errno!=ENOENT)die("cannot read policy");
    if(!strcmp(op,"-L") || !strcmp(op,"-S")) {list(&f,family);close(lock);return 0;}
    if(editing) {
        if(f.count==POCKET_FW_MAX)die("rule limit reached");
        if(!strcmp(op,"-I")) {memmove(f.rules+1,f.rules,f.count*sizeof(rule));f.rules[0]=rule;}
        else f.rules[f.count]=rule;
        f.count++;
    } else if(!strcmp(op,"-P")) {unsigned value=action(policy);if(family==4)f.policy4=value;else f.policy6=value;}
    else {
        unsigned out=0,number_in_family=0;int removed=0;
        for(unsigned i=0;i<f.count;i++) {
            int match=f.rules[i].family==family;
            if(match)number_in_family++;
            if(match && (!strcmp(op,"-F") || number_in_family==index)) {removed=1;continue;}
            f.rules[out++]=f.rules[i];
        }
        if(!strcmp(op,"-D") && !removed)die("rule number does not exist");
        f.count=out;memset(f.rules+out,0,(POCKET_FW_MAX-out)*sizeof(rule));
    }
    fd=open(temporary,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);if(fd<0)die("cannot create atomic update");
    size_t used=0;while(used<sizeof(f)) {ssize_t n=write(fd,(const char*)&f+used,sizeof(f)-used);if(n<0 && errno==EINTR)continue;
        if(n<=0) {unlink(temporary);die("cannot write policy");}used+=(size_t)n;}
    if(fsync(fd) || close(fd) || rename(temporary,path)) {unlink(temporary);die("cannot publish policy");}
    close(lock);return 0;
}
