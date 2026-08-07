# mod-gold-perks — Donny the Dealer

Donny the Dealer is a temporary summoned goblin convenience broker for AzerothCore.

## Current features

- `.goldperks summon` summons Donny temporarily.
- Donny is not permanently spawned. The world SQL creates only `creature_template` and `creature_template_model` rows.
- Donny copies the model from `"Honest" Max <Slightly Used Flying Mounts>` entry `30464`.
- Donny can preview/sell:
  - gray junk
  - gray + white misc junk
  - gray + white misc + low-level greens
  - dangerous all-greens mode
- Donny's sell scanner now has a universal **Never Sell** gate before quality checks.
- Earth Totem (`5175`), Fire Totem (`5176`), Water Totem (`5177`), and Air Totem (`5178`) are hard-protected even if the config is changed incorrectly.
- Wrath's Totem of the Earthen Ring (`46978`) is included in the default Never-Sell configuration.
- Quest-starting items, quest-bound items, quest items, keys, recipes, containers, quivers, and zero-vendor-price items are not sold.
- Additional protected entries can be added with `GoldPerks.Sell.NeverSellEntries`.
- Donny opens the player's bank for a fee.
- Donny can sell a permanent per-character **Magical Overflow / Lost & Found** perk.
- Donny randomly upcharges.
- Character named `Mang` pays an extra 10% if enabled.

## Magical Overflow / Lost & Found

The module already had an unused `pocket_rank` column in `mod_gold_perks_character`. Stage 2 uses it as the permanent Magical Overflow perk:

- `pocket_rank = 0`: not purchased
- `pocket_rank >= 1`: Magical Overflow purchased

The default base purchase price is 50 gold and is configurable with:

```ini
GoldPerks.Overflow.PurchaseCostCopper = 500000
```

Donny's normal random upcharge and optional Mang tax also apply to the purchase.

### Required AzerothCore setting

AzerothCore itself contains the safe group-roll recovery path for a player who wins an item but receives `EQUIP_ERR_INVENTORY_FULL`. To recover those items in **all group types**, set this in `worldserver.conf`:

```ini
LFG.MailItemOnFullInventory = 2
```

Values are:

- `0` — disabled
- `1` — LFG/RDF groups only
- `2` — all group types

By default, `GoldPerks.Overflow.RequireRecoveryEverywhere = 1`, so Donny refuses to sell Magical Overflow unless recovery mode `2` is active.

This intentionally relies on AzerothCore's own recovery path instead of having the module manufacture a duplicate copy of the lost item. AzerothCore only treats a genuine `EQUIP_ERR_INVENTORY_FULL` result as recoverable; unique/max-count failures remain normal loot failures. The core recovery path also preserves important item state such as random properties and group-loot binding/trade information.

### What Donny does

After purchasing the perk, summon Donny and choose:

```text
Open Donny's Magical Overflow / Lost & Found
```

Donny temporarily acts as a portable mailbox and opens the character's mail storage. Recovered group-roll items can be collected there.

**Current limitation:** AzerothCore stores recovered roll items as normal recovery mail, so those items are also reachable from an ordinary mailbox. The perk controls Donny's portable Lost & Found access; making recovery itself exclusive to perk owners would require a small new hook in AzerothCore's group-roll full-inventory path.

## Install

1. Copy `mod-gold-perks` into your AzerothCore `modules/` folder.
2. Import `sql/world/mod_gold_perks_world.sql` into your world DB.
3. Import `sql/characters/mod_gold_perks_characters.sql` into your characters DB.
4. Copy/merge `conf/mod_gold_perks.conf.dist` into your module config setup.
5. Set `LFG.MailItemOnFullInventory = 2` in `worldserver.conf` if Magical Overflow is enabled.
6. Re-run CMake if your build setup requires it, then rebuild `worldserver`.

The existing character SQL already contains `pocket_rank`, so an installation that already imported this module's character table does not need a new column for Stage 2.

## First test

```text
.goldperks status
.goldperks summon
```

### Sell safety test

Use a Shaman carrying one or more elemental totems and some disposable gray/white/green items.

1. Summon Donny.
2. Preview and sell gray + white clutter.
3. Test low-green cleanup if desired.
4. Verify Earth/Fire/Water/Air Totems remain in inventory.
5. If available on the character, verify Totem of the Earthen Ring (`46978`) is also skipped.
6. Put a known disposable item ID into `GoldPerks.Sell.NeverSellEntries`, restart/reload config as appropriate, and verify Donny skips it too.

### Magical Overflow test

1. Confirm `LFG.MailItemOnFullInventory = 2`.
2. Run `.goldperks status`; recovery mode should show `2`.
3. Summon Donny and buy Magical Overflow.
4. Fill the character's bags completely.
5. In a group, win a normal need/greed roll on an item that would otherwise fit if a slot were free.
6. Confirm AzerothCore reports the inventory-full error and creates recovery mail.
7. Summon Donny and choose **Open Donny's Magical Overflow / Lost & Found**.
8. Retrieve the recovered item.
9. Also test a unique/max-count conflict and verify it is **not** incorrectly treated as an inventory-full recovery.

## Important

No module `CMakeLists.txt` is included because this setup is intended for your existing module build style.
