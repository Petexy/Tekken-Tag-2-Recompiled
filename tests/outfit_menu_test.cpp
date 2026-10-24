#include "title/outfit_menu.h"

#include <cstdio>
#include <cstdlib>

namespace outfit = cafe::title::outfit_menu;

void check(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

int main() {
    // Base/alias collisions must be rejected in either discovery order.
    for (bool reversed : {false, true}) {
        outfit::CodeOwners codes;
        codes.add(reversed ? 1 : 0, "kza");
        codes.add(reversed ? 0 : 1, "kza");
        codes.add(2, "nsc");
        check(codes.conflicts(0) && codes.conflicts(1), "reject both duplicate owners");
        check(!codes.conflicts(2), "unrelated fighter remains available");
    }

    outfit::BodySelections bodies;
    bodies.remember(0, 74, 0x10000);
    bodies.remember(1, 74, 0x10001);
    check(bodies.resolve(0, 74) == 0x10000, "first mirror outfit");
    check(bodies.resolve(1, 74) == 0x10001, "second mirror outfit stays independent");
    // Menu return bypasses the loader, then another match reuses the slot.
    check(bodies.resolve(0, 60) == 0, "menu recipe does not inherit the fighter body");
    check(bodies.resolve(0, 74) == 0, "menu transition clears the stale selection");
    bodies.remember(0, 74, 0x10002);
    check(bodies.resolve(0, 74) == 0x10002, "new match can choose another outfit");
    bodies.remember(0, 0, 0);
    check(bodies.resolve(0, 13) == 0, "native fighter clears the selection");
    check(bodies.resolve(5, 74) == 0, "out-of-range slot has no selection");
    check(outfit::native_style(10, 0x033E2000, 2) == 2, "opening keeps the menu request");
    check(outfit::native_style(10, 0x033E3B30, 10) == 1, "confirmation normalizes native style");
    check(outfit::needs_reload(1, 2, false, 160, 160), "new outfit reloads a completed body");
    check(!outfit::needs_reload(1, 2, true, 160, 160), "pending body is not repeatedly invalidated");
    std::puts("Outfit namespace and lifecycle tests passed");
}
