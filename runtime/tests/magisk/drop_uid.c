// SPDX-License-Identifier: GPL-3.0-only
// Exercise the real guest exec boundary before contacting the Magisk daemon.
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
int main(int argc, char **argv) {
    if (argc < 3) return 64;
    char *end;
    unsigned long uid = strtoul(argv[1], &end, 10);
    if (!*argv[1] || *end || ((uid < 10000 || uid > 19999) && uid != 39999)) return 64;
    if (setresgid(uid, uid, uid) || setresuid(uid, uid, uid)) { perror("set identity"); return 1; }
    execvp(argv[2], argv + 2);
    perror("exec"); return 1;
}
