// SPDX-License-Identifier: GPL-3.0-only
// Android 9 property client, using the guest's normal bionic property area.
#include <stdio.h>
#include <sys/system_properties.h>
static void print_value(void *unused, const char *name, const char *value, unsigned serial) {
    (void)unused; (void)serial;
    printf("[%s]: [%s]\n", name, value);
}
static void print_property(const prop_info *property, void *unused) {
    __system_property_read_callback(property, print_value, unused);
}
int main(int argc, char **argv) {
    if (argc == 1) return __system_property_foreach(print_property, NULL) ? 1 : 0;
    if (argc > 3 || argv[1][0] == '-') return 64;
    char value[PROP_VALUE_MAX] = {0};
    __system_property_get(argv[1], value);
    puts(value[0] ? value : argc == 3 ? argv[2] : "");
    return 0;
}
