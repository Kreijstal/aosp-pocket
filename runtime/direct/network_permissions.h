// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <climits>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace pocket {
// Framework bookkeeping for virtual UIDs. All guest sockets still use the
// host application's kernel UID and its network policy, not these virtual IDs.
class NetworkPermissions {
    std::map<int,std::string> users;
public:
    bool apply(const std::string& command) {
        std::istringstream input(command);
        std::vector<std::string> words; std::string word;
        while(input>>word) words.push_back(word);
        if(words.size()<5 || words[0]!="network" || words[1]!="permission" || words[2]!="user")return false;
        bool setting=words[3]=="set";
        size_t first=setting ? 5 : 4;
        if((!setting && words[3]!="clear") || words.size()<=first)return false;
        if(setting && words[4]!="NETWORK" && words[4]!="SYSTEM")return false;
        std::vector<int> ids;
        for(size_t i=first;i<words.size();++i) {
            int uid=0;
            for(char digit:words[i]) {
                if(digit<'0' || digit>'9' || uid>(INT_MAX-(digit-'0'))/10)return false;
                uid=uid*10+(digit-'0');
            }
            ids.push_back(uid);
        }
        // Validate the complete request before updating any assignment.
        for(int uid:ids) { if(setting)users[uid]=words[4];else users.erase(uid); }
        return true;
    }
    std::string get(int uid) const {
        auto found=users.find(uid);return found==users.end() ? "" : found->second;
    }
};
}
