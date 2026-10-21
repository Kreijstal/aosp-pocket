// SPDX-License-Identifier: GPL-3.0-only
#include "../direct/network_permissions.h"
#include <cassert>
#include <iostream>
int main() {
    pocket::NetworkPermissions permissions;
    assert(permissions.apply("network permission user set NETWORK 10016"));
    assert(permissions.get(10016)=="NETWORK");
    assert(permissions.apply("network permission user set SYSTEM 1000 10016"));
    assert(permissions.get(1000)=="SYSTEM" && permissions.get(10016)=="SYSTEM");
    assert(!permissions.apply("network permission user set NETWORK 10016 invalid"));
    assert(permissions.get(10016)=="SYSTEM");
    for(const char* invalid:{"network permission user", "network permission user set NETWORK",
            "network permission user clear", "network permission user set ROOT 10016",
            "network permission user set NETWORK -1", "network permission user set NETWORK +1",
            "network permission user clear 2147483648", "network permission network set NETWORK 100"})
        assert(!permissions.apply(invalid));
    assert(permissions.apply("network permission user clear 10016 12345"));
    assert(permissions.get(10016).empty() && permissions.get(1000)=="SYSTEM");
    assert(permissions.apply("network permission user set NETWORK 0 2147483647"));
    assert(permissions.get(INT_MAX)=="NETWORK");
    std::cout<<"PASS: package-added UID grants, updates, removal and atomic rejection of malformed commands\n";
}
