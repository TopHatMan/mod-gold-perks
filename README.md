# mod-gold-perks — Stage 1

Donny the Dealer is a temporary summoned goblin convenience broker.

## Current features

- `.goldperks summon` summons Donny temporarily.
- Donny is not permanently spawned. The world SQL creates only `creature_template` and `creature_template_model` rows.
- Donny copies the model from `"Honest" Max <Slightly Used Flying Mounts>` entry `30464`.
- Donny can preview/sell:
  - gray junk
  - gray + white misc junk
  - gray + white misc + low-level greens
  - dangerous all-greens mode
- Donny opens the player's bank for a fee.
- Donny randomly upcharges.
- Character named `Mang` pays an extra 10% if enabled.

## Install

1. Copy `mod-gold-perks` into your AzerothCore `modules/` folder.
2. Import `sql/world/mod_gold_perks_world.sql` into your world DB.
3. Import `sql/characters/mod_gold_perks_characters.sql` into your characters DB.
4. Copy/merge `conf/mod_gold_perks.conf.dist` into your config setup.
5. Re-run CMake if your build setup requires it, then rebuild `worldserver`.

## First test

```txt
.goldperks status
.goldperks summon
```

Click Donny and test preview first before selling greens.

## Important

No module `CMakeLists.txt` is included because this setup is intended for your existing module build style.
