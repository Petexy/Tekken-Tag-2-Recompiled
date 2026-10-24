# Eliza gameplay fixes

Regenerate Eliza with the character importer to apply these source changes. Existing packs do not update automatically. No Tekken 7 assets or generated packs are committed.

- Restore buffered special cancels from her configured jab/string, crouching, back-turned, and while-standing attacks. Preserve hit/block contact gates; fireball-to-EX Moon Glide uses its original non-contact route.
- Make Eliza's EX and super routes meter-free through an explicit character opt-in, retaining the required EX moves and removing meter costs.
- Restore the final 4 of b+1,2,4 using the original secondary string buffer's input timing.
- Restore d+4 from standing and full crouch without changing its attack data.
- Restore light, heavy, and EX jumping divekick inputs across neutral/forward/backward jump states. Replace unsupported T7 physics gates with windows limited to the baked jump's airborne frames; preserve original command priority.
- Map qcb+1, qcb+2, and qcb+1+2 hitboxes onto native arm capsules.
- Remove Bound from standing 4, Moon Glide 1,4, and Moon Glide 3,4; keep their other hit reactions.
- Add a native tag-launch cancel to the final hit of Moon Glide 3,3.

The unsuccessful b+4 → EX Moon Glide Tag Assault experiments remain reverted.

Validation: focused converter regression tests; regenerated Eliza moveset and motion match the installed candidate byte-for-byte. The user confirmed b+1,2,4 in game. The latest d+4 and jump-divekick routes still need in-game confirmation.
