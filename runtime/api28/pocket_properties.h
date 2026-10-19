// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#include <functional>
#include <string>
using PropertyChanged = std::function<void(const std::string&, const std::string&)>;
using PropertyControl = std::function<uint32_t(const std::string&, const std::string&)>;
int pocketPropertiesStart(PropertyChanged changed = {}, PropertyControl control = {});
bool pocketPropertiesPoll(int timeout_ms);
bool pocketPropertiesStopped();
void pocketPropertiesClose();
uint32_t pocketPropertySet(const std::string& name, const std::string& value);
