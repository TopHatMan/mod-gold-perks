from pathlib import Path
import re

CPP = Path('src/mod_gold_perks.cpp')
CONF = Path('conf/mod_gold_perks.conf.dist')
README = Path('README.md')
WORLD_SQL = Path('sql/world/mod_gold_perks_world.sql')


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f'{label}: expected 1 anchor, found {count}')
    return text.replace(old, new, 1)


cpp = CPP.read_text()

# Current AzerothCore uses fmt-style {} placeholders in PSendSysMessage/Acore::StringFormat.
fmt_replacements = {
    'ChatHandler(player->GetSession()).PSendSysMessage("|cff66ff66Donny the Dealer|r: %s", text.c_str());':
        'ChatHandler(player->GetSession()).PSendSysMessage("|cff66ff66Donny the Dealer|r: {}", text);',
    'handler.PSendSysMessage("Protected bag slot: %u", sConfigMgr->GetOption<uint32>("GoldPerks.Sell.ProtectedBagSlot", 4));':
        'handler.PSendSysMessage("Protected bag slot: {}", sConfigMgr->GetOption<uint32>("GoldPerks.Sell.ProtectedBagSlot", 4));',
    'handler.PSendSysMessage("Magical Overflow: %s", HasOverflowPerk(player) ? "PURCHASED" : "not purchased");':
        'handler.PSendSysMessage("Magical Overflow: {}", HasOverflowPerk(player) ? "PURCHASED" : "not purchased");',
    'handler.PSendSysMessage("Full-inventory group-roll recovery mode: %u (2 = all groups)", GetOverflowRecoveryMode());':
        'handler.PSendSysMessage("Full-inventory group-roll recovery mode: {} (2 = all groups)", GetOverflowRecoveryMode());',
    'handler.PSendSysMessage("Mang tax active for you: %s", IsMang(player) ? "YES. Donny noticed." : "no");':
        'handler.PSendSysMessage("Mang tax active for you: {}", IsMang(player) ? "YES. Donny noticed." : "no");',
    'handler->PSendSysMessage("Donny is dodging you for %u more seconds.", (lastSummon + cooldown) - now);':
        'handler->PSendSysMessage("Donny is dodging you for {} more seconds.", (lastSummon + cooldown) - now);',
    'handler->PSendSysMessage("Donny wants %s just to show up. You don\'t have it.", GoldPerks::MoneyString(finalCost).c_str());':
        'handler->PSendSysMessage("Donny wants {} just to show up. You don\'t have it.", GoldPerks::MoneyString(finalCost));',
    'handler->PSendSysMessage("Donny charged %s to show up.%s%s", GoldPerks::MoneyString(finalCost).c_str(), reason.empty() ? "" : " ", reason.c_str());':
        'handler->PSendSysMessage("Donny charged {} to show up.{}{}", GoldPerks::MoneyString(finalCost), reason.empty() ? "" : " ", reason);',
}
for old, new in fmt_replacements.items():
    cpp = replace_once(cpp, old, new, f'fmt replacement {old[:45]}')

# Runtime install/schema diagnostics. These are intentionally synchronous only on explicit Donny
# interactions/status checks, not on a hot update loop.
anchor = '''    static void DonnySay(Player* player, std::string const& text)\n    {\n        if (!player || !player->GetSession())\n            return;\n\n        ChatHandler(player->GetSession()).PSendSysMessage("|cff66ff66Donny the Dealer|r: {}", text);\n    }\n'''
insert = '''    struct DonnyTemplateState\n    {\n        bool exists = false;\n        bool scriptOk = false;\n        bool gossipFlag = false;\n        std::string scriptName;\n        uint64 npcFlags = 0;\n\n        bool Ready() const { return exists && scriptOk && gossipFlag; }\n    };\n\n    static DonnyTemplateState InspectDonnyTemplate(uint32 entry)\n    {\n        DonnyTemplateState state;\n        QueryResult result = WorldDatabase.Query(\n            "SELECT `ScriptName`, `npcflag` FROM `creature_template` WHERE `entry` = {}",\n            entry);\n        if (!result)\n            return state;\n\n        Field* fields = result->Fetch();\n        state.exists = true;\n        state.scriptName = fields[0].Get<std::string>();\n        state.npcFlags = fields[1].Get<uint64>();\n        state.scriptOk = state.scriptName == "npc_donny_the_dealer";\n        state.gossipFlag = (state.npcFlags & uint64(UNIT_NPC_FLAG_GOSSIP)) != 0;\n        return state;\n    }\n\n    static std::string DonnyTemplateProblem(DonnyTemplateState const& state, uint32 entry)\n    {\n        if (!state.exists)\n            return "creature_template entry " + std::to_string(entry) + " is missing; import sql/world/mod_gold_perks_world.sql";\n        if (!state.scriptOk)\n            return "creature_template ScriptName is '" + state.scriptName + "' instead of npc_donny_the_dealer; re-import the world SQL and restart worldserver";\n        if (!state.gossipFlag)\n            return "creature_template is missing UNIT_NPC_FLAG_GOSSIP; re-import the world SQL and restart worldserver";\n        return "ready";\n    }\n\n    static bool CharacterSchemaReady()\n    {\n        QueryResult result = CharacterDatabase.Query(\n            "SHOW COLUMNS FROM `mod_gold_perks_character` LIKE 'pocket_rank'");\n        return bool(result);\n    }\n\n    static void DonnySay(Player* player, std::string const& text)\n    {\n        if (!player || !player->GetSession())\n            return;\n\n        ChatHandler(player->GetSession()).PSendSysMessage("|cff66ff66Donny the Dealer|r: {}", text);\n    }\n'''
cpp = replace_once(cpp, anchor, insert, 'Donny runtime diagnostics')

# Make the permanent perk write verifiable before money is removed.
old_set = '''    static void SetPocketRank(Player* player, uint32 rank)\n    {\n        if (!player)\n            return;\n\n        EnsureCharacterRow(player);\n        CharacterDatabase.Execute(\n            "UPDATE `mod_gold_perks_character` SET `pocket_rank` = {} WHERE `guid` = {}",\n            rank,\n            player->GetGUID().GetCounter());\n    }\n'''
new_set = '''    static bool SetPocketRankVerified(Player* player, uint32 rank)\n    {\n        if (!player || !CharacterSchemaReady())\n            return false;\n\n        uint32 const guid = player->GetGUID().GetCounter();\n\n        // This is a rare explicit purchase, so make persistence deterministic instead of queuing an\n        // async INSERT/UPDATE and charging the player before we know the perk was stored.\n        CharacterDatabase.DirectExecute(\n            "INSERT IGNORE INTO `mod_gold_perks_character` (`guid`) VALUES ({})", guid);\n        CharacterDatabase.DirectExecute(\n            "UPDATE `mod_gold_perks_character` SET `pocket_rank` = {} WHERE `guid` = {}", rank, guid);\n\n        QueryResult result = CharacterDatabase.Query(\n            "SELECT `pocket_rank` FROM `mod_gold_perks_character` WHERE `guid` = {}", guid);\n        return result && result->Fetch()[0].Get<uint32>() == rank;\n    }\n'''
cpp = replace_once(cpp, old_set, new_set, 'verified pocket persistence')

# Overflow purchase: expose schema failure and persist first; only then take money. If the money check
# somehow changes between validation and deduction, roll the rank back.
old_purchase = '''        uint32 baseCost = OverflowPurchaseBaseCost();\n        std::string reason;\n        uint32 finalCost = ApplyServiceUpcharge(player, baseCost, &reason);\n        if (!TakeMoney(player, finalCost))\n        {\n            DonnySay(player, "Magical pockets cost money. Revolutionary concept. I need " + MoneyString(finalCost) + ".");\n            return false;\n        }\n\n        SetPocketRank(player, 1);\n        CharacterDatabase.Execute(\n            "INSERT INTO `mod_gold_perks_log` (`guid`, `action`, `donny_cut`) VALUES ({}, 'overflow_buy', {})",\n            player->GetGUID().GetCounter(), finalCost);\n'''
new_purchase = '''        if (!CharacterSchemaReady())\n        {\n            DonnySay(player, "My dimensional paperwork table is missing pocket_rank. Import sql/characters/mod_gold_perks_characters.sql before I take a single copper.");\n            return false;\n        }\n\n        uint32 baseCost = OverflowPurchaseBaseCost();\n        std::string reason;\n        uint32 finalCost = ApplyServiceUpcharge(player, baseCost, &reason);\n        if (player->GetMoney() < finalCost)\n        {\n            DonnySay(player, "Magical pockets cost money. Revolutionary concept. I need " + MoneyString(finalCost) + ".");\n            return false;\n        }\n\n        if (!SetPocketRankVerified(player, 1))\n        {\n            DonnySay(player, "The dimensional paperwork failed. I did NOT charge you. Check the characters database and worldserver log.");\n            return false;\n        }\n\n        if (!TakeMoney(player, finalCost))\n        {\n            (void)SetPocketRankVerified(player, 0);\n            DonnySay(player, "Your money changed while I was filing the paperwork. Deal cancelled; you were not charged.");\n            return false;\n        }\n\n        CharacterDatabase.Execute(\n            "INSERT INTO `mod_gold_perks_log` (`guid`, `action`, `donny_cut`) VALUES ({}, 'overflow_buy', {})",\n            player->GetGUID().GetCounter(), finalCost);\n'''
cpp = replace_once(cpp, old_purchase, new_purchase, 'overflow purchase safety')

# Status becomes an actual doctor: show template binding, schema and the exact recovery gate.
old_status_tail = '''        handler.PSendSysMessage("Full-inventory group-roll recovery mode: {} (2 = all groups)", GetOverflowRecoveryMode());\n        if (HasOverflowPerk(player) && !OverflowRecoveryReady())\n            handler.SendSysMessage("|cffff6060WARNING: Magical Overflow is purchased, but LFG.MailItemOnFullInventory is not set to 2.|r");\n        handler.PSendSysMessage("Mang tax active for you: {}", IsMang(player) ? "YES. Donny noticed." : "no");\n'''
new_status_tail = '''        handler.PSendSysMessage("Full-inventory group-roll recovery mode: {} (2 = all groups)", GetOverflowRecoveryMode());\n        if (!OverflowRecoveryReady())\n            handler.SendSysMessage("|cffff6060Magical Overflow purchase is blocked until LFG.MailItemOnFullInventory = 2 (unless RequireRecoveryEverywhere is disabled).|r");\n        handler.PSendSysMessage("Characters DB pocket_rank schema: {}", CharacterSchemaReady() ? "ready" : "MISSING/BROKEN");\n\n        uint32 const donnyEntry = sConfigMgr->GetOption<uint32>("GoldPerks.Donny.Entry", 900100);\n        DonnyTemplateState const templateState = InspectDonnyTemplate(donnyEntry);\n        handler.PSendSysMessage("Donny template entry {}: {}", donnyEntry, templateState.Ready() ? "ready" : "BROKEN");\n        if (!templateState.Ready())\n            handler.PSendSysMessage("Donny template problem: {}", DonnyTemplateProblem(templateState, donnyEntry));\n        else\n            handler.PSendSysMessage("Donny ScriptName: {} | npcflag: {}", templateState.scriptName, templateState.npcFlags);\n\n        handler.PSendSysMessage("Mang tax active for you: {}", IsMang(player) ? "YES. Donny noticed." : "no");\n'''
cpp = replace_once(cpp, old_status_tail, new_status_tail, 'status diagnostics')

# Do not charge a summon fee if the DB template cannot possibly dispatch the gossip script.
old_summon_cost = '''        uint32 baseCost = sConfigMgr->GetOption<uint32>("GoldPerks.Donny.Summon.CostCopper", 5000);\n        baseCost = uint32(float(baseCost) * GoldPerks::DungeonFeeMultiplier(player, false));\n\n        std::string reason;\n        uint32 finalCost = GoldPerks::ApplyServiceUpcharge(player, baseCost, &reason);\n'''
new_summon_cost = '''        uint32 const entry = sConfigMgr->GetOption<uint32>("GoldPerks.Donny.Entry", 900100);\n        GoldPerks::DonnyTemplateState const templateState = GoldPerks::InspectDonnyTemplate(entry);\n        if (!templateState.Ready())\n        {\n            handler->PSendSysMessage("Donny entry {} is not usable: {}. No summon fee charged.",\n                entry, GoldPerks::DonnyTemplateProblem(templateState, entry));\n            return true;\n        }\n\n        uint32 baseCost = sConfigMgr->GetOption<uint32>("GoldPerks.Donny.Summon.CostCopper", 5000);\n        baseCost = uint32(float(baseCost) * GoldPerks::DungeonFeeMultiplier(player, false));\n\n        std::string reason;\n        uint32 finalCost = GoldPerks::ApplyServiceUpcharge(player, baseCost, &reason);\n'''
cpp = replace_once(cpp, old_summon_cost, new_summon_cost, 'pre-charge template validation')
cpp = replace_once(cpp,
    '        uint32 entry = sConfigMgr->GetOption<uint32>("GoldPerks.Donny.Entry", 900100);\n        uint32 duration =',
    '        uint32 duration =',
    'remove duplicate summon entry')

# Make the gossip purchase itself explain the recovery prerequisite before the click, too.
old_buy_text = '''                std::string buyText = "Buy Magical Overflow (base " + GoldPerks::MoneyString(GoldPerks::OverflowPurchaseBaseCost()) + ")";\n                AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, buyText, GOSSIP_SENDER_MAIN, GoldPerks::ACTION_BUY_OVERFLOW);\n'''
new_buy_text = '''                bool const requireEverywhere = sConfigMgr->GetOption<bool>("GoldPerks.Overflow.RequireRecoveryEverywhere", true);\n                std::string buyText;\n                if (requireEverywhere && !GoldPerks::OverflowRecoveryReady())\n                    buyText = "Magical Overflow unavailable: set LFG.MailItemOnFullInventory = 2";\n                else\n                    buyText = "Buy Magical Overflow (base " + GoldPerks::MoneyString(GoldPerks::OverflowPurchaseBaseCost()) + ")";\n                AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, buyText, GOSSIP_SENDER_MAIN, GoldPerks::ACTION_BUY_OVERFLOW);\n'''
cpp = replace_once(cpp, old_buy_text, new_buy_text, 'gossip recovery prerequisite')

CPP.write_text(cpp)

# AzerothCore module configs require a worldserver section header.
conf = CONF.read_text()
if not conf.startswith('[worldserver]\n'):
    conf = '[worldserver]\n\n' + conf
CONF.write_text(conf)

# Add explicit runtime-repair guidance.
readme = README.read_text().rstrip()
section = '''\n\n## Runtime diagnostics / repair\n\nCurrent AzerothCore uses fmt-style `{}` placeholders for `PSendSysMessage`; this module now follows that API so Donny's feedback is visible instead of silently failing around old `%s`/`%u` formatting.\n\nThe canonical module config must begin with `[worldserver]`. Copying a `.conf.dist` without that section header into `configs/modules/` can make ConfigMgr ignore the GoldPerks settings even though the file exists.\n\nRun:\n\n```text\n.goldperks status\n```\n\nto see the effective overflow recovery mode, `pocket_rank` schema state, Donny creature-template binding, `ScriptName`, and gossip flag. `.goldperks summon` now refuses to take a summon fee if the configured creature entry is missing, lacks the gossip flag, or is not bound to `npc_donny_the_dealer`.\n\nMagical Overflow still intentionally requires `LFG.MailItemOnFullInventory = 2` by default. When that prerequisite is not satisfied, the gossip option and status output now say so explicitly. The purchase persists `pocket_rank` first and verifies it before deducting gold; persistence failure therefore cannot eat the purchase price.\n'''
if '## Runtime diagnostics / repair' not in readme:
    readme += section
README.write_text(readme.rstrip() + '\n')

# Make the world SQL self-documenting for the exact binding the runtime doctor checks.
world_sql = WORLD_SQL.read_text().rstrip()
extra = '''\n\n-- Runtime verification after import/restart:\n-- SELECT `entry`, `name`, `npcflag`, `ScriptName`\n-- FROM `creature_template`\n-- WHERE `entry` = 900100;\n-- Expected: npcflag includes GOSSIP (1), ScriptName = 'npc_donny_the_dealer'.\n'''
if 'Runtime verification after import/restart' not in world_sql:
    world_sql += extra
WORLD_SQL.write_text(world_sql.rstrip() + '\n')

print('Donny runtime repair applied')
