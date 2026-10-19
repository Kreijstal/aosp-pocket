// SPDX-License-Identifier: GPL-3.0-only
#include "anbox/network/connections.h"
#include <cassert>

struct Connection {
    anbox::network::Connections<Connection>* owner;
    int number;
    int* destructions;
    int id() const { return number; }
    ~Connection() {
        // Model a close callback or joined render thread re-entering the
        // registry. The old erase-under-lock implementation deadlocked here.
        assert(!owner->includes(number));
        ++*destructions;
    }
};
int main() {
    anbox::network::Connections<Connection> registry;
    int destroyed = 0;
    for (int id = 0; id < 3; id++) {
        auto connection = std::shared_ptr<Connection>(new Connection{&registry, id, &destroyed});
        registry.add(connection);
    }
    assert(registry.size() == 3);
    registry.remove(1);
    assert(destroyed == 1 && registry.size() == 2);
    {
        auto active = registry.snapshot();
        assert(active.size() == 2 && active[0]->id() == 0 && active[1]->id() == 2);
    }
    registry.clear();
    assert(destroyed == 3 && registry.size() == 0);
}
