#include "game/item_parity.h"

#include <cassert>

int main() {
    assert(defaultItemPickupAmount(ItemKind::Health) == 25.0f);
    assert(defaultItemPickupAmount(ItemKind::Ammo) == 10.0f);
    // Mission items without a resolvable datablock use this native amount.
    assert(itemPickupAmount(ItemKind::Ammo, defaultItemPickupAmount(ItemKind::Ammo)) == 10.0f);
    assert(itemPickupAmount(ItemKind::Health, 40.0f) == 40.0f);
    assert(itemPickupAmount(ItemKind::Ammo, 0.0f) == 10.0f);
    assert(defaultItemRespawnDelay() == 20.0f);
    assert(itemRespawnDelay(7.5f) == 7.5f);
    assert(itemRespawnDelay(-1.0f) == 20.0f);
    assert(itemProxyVisible(true, true, true));
    assert(!itemProxyVisible(true, true, false));
    assert(!itemProxyVisible(true, false, true));
    assert(!itemProxyVisible(false, true, true));
    assert(itemCanBeCollected(true, true, true));
    assert(!itemCanBeCollected(true, true, false));
    assert(!itemCanBeCollected(false, true, true));
    assert(itemActiveAfterEnable(true, 12.0f));
    assert(!itemActiveAfterEnable(false, 12.0f));
    assert(itemActiveAfterEnable(false, 0.0f));
    assert(itemRespawnReady(0.0f, 0.0f));
    assert(itemRespawnReady(-1.0f, 0.0f));
    assert(!itemRespawnReady(1.0f, 0.0f));
    assert(itemDatablockNameEquals("AmmoBox", "ammobox"));
    assert(!itemDatablockNameEquals("AmmoBox", "AmmoPack"));
    return 0;
}
