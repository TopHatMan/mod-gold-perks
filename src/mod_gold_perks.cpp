/*
 * mod-gold-perks
 * Stage 1: Donny the Dealer, a temporary summoned goblin broker.
 *
 * Features:
 * - .goldperks summon summons Donny temporarily only. No permanent creature spawn.
 * - Donny gossip sells gray/white/green clutter for vendor value minus his cut.
 * - Donny opens the player's bank for a fee.
 * - Donny lies/upcharges, and character name "Mang" pays extra if enabled.
 */

#include "ScriptMgr.h"
#include "Bag.h"
#include "Chat.h"
#include "Config.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Item.h"
#include "Map.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "Random.h"
#include "ScriptedGossip.h"
#include "SharedDefines.h"
#include "WorldSession.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace GoldPerks
{
    constexpr uint32 COPPER_PER_SILVER = 100;
    constexpr uint32 COPPER_PER_GOLD = 10000;

    enum DonnyActions : uint32
    {
        ACTION_STATUS = 100,
        ACTION_PREVIEW_SAFE = 101,
        ACTION_PREVIEW_FIELD = 102,
        ACTION_PREVIEW_LOW_GREENS = 103,
        ACTION_SELL_GRAY = 104,
        ACTION_SELL_GRAY_WHITE = 105,
        ACTION_SELL_GRAY_WHITE_LOW_GREENS = 106,
        ACTION_SELL_ALL_GREENS_DANGEROUS = 107,
        ACTION_OPEN_BANK = 108,
        ACTION_EXPLAIN_FEES = 109,
    };

    enum SellMask : uint32
    {
        SELL_GRAY = 0x01,
        SELL_WHITE = 0x02,
        SELL_LOW_GREENS = 0x04,
        SELL_ALL_GREENS = 0x08,
    };

    enum TimeColumn
    {
        TIME_LAST_DONNY_SUMMON,
        TIME_LAST_BANK_USE,
        TIME_LAST_SELL_USE
    };

    // These class values are stable in 3.3.5 item templates.
    enum ItemClassIds : uint32
    {
        ITEM_CLASS_CONSUMABLE_CUSTOM = 0,
        ITEM_CLASS_CONTAINER_CUSTOM = 1,
        ITEM_CLASS_WEAPON_CUSTOM = 2,
        ITEM_CLASS_ARMOR_CUSTOM = 4,
        ITEM_CLASS_REAGENT_CUSTOM = 5,
        ITEM_CLASS_PROJECTILE_CUSTOM = 6,
        ITEM_CLASS_TRADE_GOODS_CUSTOM = 7,
        ITEM_CLASS_RECIPE_CUSTOM = 9,
        ITEM_CLASS_QUIVER_CUSTOM = 11,
        ITEM_CLASS_QUEST_CUSTOM = 12,
        ITEM_CLASS_KEY_CUSTOM = 13,
        ITEM_CLASS_MISC_CUSTOM = 15,
    };

    struct SellCandidate
    {
        uint8 bag = 0;
        uint8 slot = 0;
        uint32 entry = 0;
        uint32 count = 0;
        uint32 quality = 0;
        uint32 vendorValue = 0;
    };

    struct SellSummary
    {
        uint32 gray = 0;
        uint32 white = 0;
        uint32 green = 0;
        uint32 vendorValue = 0;
        uint32 donnyCut = 0;
        uint32 playerReceived = 0;
        uint32 cutPct = 0;

        uint32 TotalItems() const { return gray + white + green; }
    };

    static bool Enabled()
    {
        return sConfigMgr->GetOption<bool>("GoldPerks.Enable", true);
    }

    static uint32 Now()
    {
        return uint32(GameTime::GetGameTime().count());
    }

    static std::string MoneyString(uint32 copper)
    {
        uint32 gold = copper / COPPER_PER_GOLD;
        copper %= COPPER_PER_GOLD;
        uint32 silver = copper / COPPER_PER_SILVER;
        copper %= COPPER_PER_SILVER;

        std::ostringstream ss;
        if (gold)
            ss << gold << "g ";
        if (silver || gold)
            ss << silver << "s ";
        ss << copper << "c";
        return ss.str();
    }

    static void DonnySay(Player* player, std::string const& text)
    {
        if (!player || !player->GetSession())
            return;

        ChatHandler(player->GetSession()).PSendSysMessage("|cff66ff66Donny the Dealer|r: %s", text.c_str());
    }

    static bool IsMang(Player* player)
    {
        if (!player || !sConfigMgr->GetOption<bool>("GoldPerks.Donny.MangTax.Enable", true))
            return false;

        std::string mangName = sConfigMgr->GetOption<std::string>("GoldPerks.Donny.MangTax.Name", "Mang");
        return player->GetName() == mangName;
    }

    static void AppendReason(std::string& reason, std::string const& text)
    {
        if (!reason.empty())
            reason += " ";
        reason += text;
    }

    static uint32 ApplyServiceUpcharge(Player* player, uint32 baseCost, std::string* reason = nullptr)
    {
        uint32 finalCost = baseCost;
        std::string localReason;

        if (sConfigMgr->GetOption<bool>("GoldPerks.Donny.Upcharge.Enable", true) && baseCost > 0)
        {
            uint32 chance = sConfigMgr->GetOption<uint32>("GoldPerks.Donny.Upcharge.ChancePct", 100);
            if (chance >= 100 || urand(1, 100) <= chance)
            {
                uint32 minPct = sConfigMgr->GetOption<uint32>("GoldPerks.Donny.Upcharge.MinPct", 5);
                uint32 maxPct = sConfigMgr->GetOption<uint32>("GoldPerks.Donny.Upcharge.MaxPct", 20);
                if (maxPct < minPct)
                    std::swap(minPct, maxPct);

                uint32 pct = urand(minPct, maxPct);
                finalCost += (baseCost * pct) / 100;
                AppendReason(localReason, "Donny invented a " + std::to_string(pct) + "% paperwork fee.");
            }
        }

        if (IsMang(player) && finalCost > 0)
        {
            uint32 pct = sConfigMgr->GetOption<uint32>("GoldPerks.Donny.MangTax.ExtraPct", 10);
            finalCost += (finalCost * pct) / 100;
            AppendReason(localReason, "Mang tax applied.");
        }

        if (reason)
            *reason = localReason;

        return finalCost;
    }

    static uint32 ApplySellCutUpcharge(Player* player, uint32 baseCutPct, std::string* reason = nullptr)
    {
        uint32 cutPct = baseCutPct;
        std::string localReason;

        if (sConfigMgr->GetOption<bool>("GoldPerks.Donny.Upcharge.Enable", true))
        {
            uint32 chance = sConfigMgr->GetOption<uint32>("GoldPerks.Donny.Upcharge.ChancePct", 100);
            if (chance >= 100 || urand(1, 100) <= chance)
            {
                uint32 minPct = sConfigMgr->GetOption<uint32>("GoldPerks.Donny.Upcharge.MinPct", 5);
                uint32 maxPct = sConfigMgr->GetOption<uint32>("GoldPerks.Donny.Upcharge.MaxPct", 20);
                if (maxPct < minPct)
                    std::swap(minPct, maxPct);

                uint32 pct = urand(minPct, maxPct);
                cutPct += pct;
                AppendReason(localReason, "Donny changed the market and added " + std::to_string(pct) + "% to his cut.");
            }
        }

        if (IsMang(player))
        {
            uint32 pct = sConfigMgr->GetOption<uint32>("GoldPerks.Donny.MangTax.ExtraPct", 10);
            cutPct += pct;
            AppendReason(localReason, "Mang tax applied.");
        }

        uint32 maxCut = sConfigMgr->GetOption<uint32>("GoldPerks.Sell.MaxCutPct", 90);
        cutPct = std::min(cutPct, maxCut);

        if (reason)
            *reason = localReason;

        return cutPct;
    }

    static void EnsureCharacterRow(Player* player)
    {
        if (!player)
            return;

        CharacterDatabase.Execute(
            "INSERT IGNORE INTO `mod_gold_perks_character` (`guid`) VALUES ({})",
            player->GetGUID().GetCounter());
    }

    static uint32 GetLastTime(Player* player, TimeColumn column)
    {
        EnsureCharacterRow(player);

        QueryResult result;
        switch (column)
        {
            case TIME_LAST_DONNY_SUMMON:
                result = CharacterDatabase.Query("SELECT `last_donny_summon` FROM `mod_gold_perks_character` WHERE `guid` = {}", player->GetGUID().GetCounter());
                break;
            case TIME_LAST_BANK_USE:
                result = CharacterDatabase.Query("SELECT `last_bank_use` FROM `mod_gold_perks_character` WHERE `guid` = {}", player->GetGUID().GetCounter());
                break;
            case TIME_LAST_SELL_USE:
                result = CharacterDatabase.Query("SELECT `last_sell_use` FROM `mod_gold_perks_character` WHERE `guid` = {}", player->GetGUID().GetCounter());
                break;
        }

        if (!result)
            return 0;

        Field* fields = result->Fetch();
        return fields[0].Get<uint32>();
    }

    static void SetLastTime(Player* player, TimeColumn column, uint32 value)
    {
        EnsureCharacterRow(player);

        switch (column)
        {
            case TIME_LAST_DONNY_SUMMON:
                CharacterDatabase.Execute("UPDATE `mod_gold_perks_character` SET `last_donny_summon` = {} WHERE `guid` = {}", value, player->GetGUID().GetCounter());
                break;
            case TIME_LAST_BANK_USE:
                CharacterDatabase.Execute("UPDATE `mod_gold_perks_character` SET `last_bank_use` = {} WHERE `guid` = {}", value, player->GetGUID().GetCounter());
                break;
            case TIME_LAST_SELL_USE:
                CharacterDatabase.Execute("UPDATE `mod_gold_perks_character` SET `last_sell_use` = {} WHERE `guid` = {}", value, player->GetGUID().GetCounter());
                break;
        }
    }

    static bool TakeMoney(Player* player, uint32 amount)
    {
        if (!player || amount == 0)
            return true;

        if (player->GetMoney() < amount)
            return false;

        player->ModifyMoney(-int32(amount));
        return true;
    }

    static bool IsForbiddenForSummon(Player* player)
    {
        if (!player)
            return true;

        if (!sConfigMgr->GetOption<bool>("GoldPerks.Donny.Summon.AllowInCombat", false) && player->IsInCombat())
            return true;

        if (!sConfigMgr->GetOption<bool>("GoldPerks.Donny.Summon.AllowInBattleground", false) && player->InBattleground())
            return true;

        if (!sConfigMgr->GetOption<bool>("GoldPerks.Donny.Summon.AllowInArena", false) && player->InArena())
            return true;

        Map* map = player->GetMap();
        if (map && map->IsDungeon())
        {
            if (map->IsRaid())
                return !sConfigMgr->GetOption<bool>("GoldPerks.Donny.Summon.AllowInRaid", false);

            return !sConfigMgr->GetOption<bool>("GoldPerks.Donny.Summon.AllowInDungeon", true);
        }

        return false;
    }

    static bool IsForbiddenForBank(Player* player)
    {
        if (!player)
            return true;

        if (!sConfigMgr->GetOption<bool>("GoldPerks.Bank.AllowInCombat", false) && player->IsInCombat())
            return true;

        if (!sConfigMgr->GetOption<bool>("GoldPerks.Bank.AllowInBattleground", false) && player->InBattleground())
            return true;

        if (!sConfigMgr->GetOption<bool>("GoldPerks.Bank.AllowInArena", false) && player->InArena())
            return true;

        Map* map = player->GetMap();
        if (map && map->IsDungeon())
        {
            if (map->IsRaid())
                return !sConfigMgr->GetOption<bool>("GoldPerks.Bank.AllowInRaid", false);

            return !sConfigMgr->GetOption<bool>("GoldPerks.Bank.AllowInDungeon", true);
        }

        return false;
    }

    static float DungeonFeeMultiplier(Player* player, bool forBank)
    {
        if (!player || !player->GetMap() || !player->GetMap()->IsDungeon())
            return 1.0f;

        return sConfigMgr->GetOption<float>(forBank ? "GoldPerks.Bank.DungeonFeeMultiplier" : "GoldPerks.Donny.Summon.DungeonFeeMultiplier", 3.0f);
    }

    static bool IsProtectedBag(Item* item)
    {
        if (!item)
            return false;

        if (!sConfigMgr->GetOption<bool>("GoldPerks.Sell.ProtectedBag.Enabled", true))
            return false;

        uint32 protectedBagNumber = sConfigMgr->GetOption<uint32>("GoldPerks.Sell.ProtectedBagSlot", 4);
        if (protectedBagNumber < 1 || protectedBagNumber > 4)
            return false;

        uint8 protectedBagSlot = uint8(INVENTORY_SLOT_BAG_START + (protectedBagNumber - 1));
        return item->GetBagSlot() == protectedBagSlot;
    }

    static bool IsNeverSellTemplate(ItemTemplate const* proto)
    {
        if (!proto)
            return true;

        if (proto->SellPrice == 0)
            return true;

        switch (proto->Class)
        {
            case ITEM_CLASS_CONTAINER_CUSTOM:
            case ITEM_CLASS_QUEST_CUSTOM:
            case ITEM_CLASS_KEY_CUSTOM:
            case ITEM_CLASS_RECIPE_CUSTOM:
            case ITEM_CLASS_QUIVER_CUSTOM:
                return true;
            default:
                break;
        }

        return false;
    }

    static bool ShouldSellWhite(ItemTemplate const* proto)
    {
        if (!proto || proto->Quality != ITEM_QUALITY_NORMAL)
            return false;

        if (proto->Class == ITEM_CLASS_MISC_CUSTOM)
            return true;

        if (sConfigMgr->GetOption<bool>("GoldPerks.Sell.White.AllowTradeGoods", false) && proto->Class == ITEM_CLASS_TRADE_GOODS_CUSTOM)
            return true;

        return false;
    }

    static bool ShouldSellLowGreen(Player* player, ItemTemplate const* proto)
    {
        if (!player || !proto || proto->Quality != ITEM_QUALITY_UNCOMMON)
            return false;

        uint32 delta = sConfigMgr->GetOption<uint32>("GoldPerks.Sell.Green.LowLevelDelta", 12);
        uint32 playerLevel = player->GetLevel();
        if (playerLevel <= delta)
            return false;

        return proto->ItemLevel + delta <= playerLevel;
    }

    static bool ShouldSellAllGreen(ItemTemplate const* proto)
    {
        return proto && proto->Quality == ITEM_QUALITY_UNCOMMON && sConfigMgr->GetOption<bool>("GoldPerks.Sell.Green.AllowDangerousAllGreens", true);
    }

    static void AddCandidate(Player* player, Item* item, uint32 mask, std::vector<SellCandidate>& out)
    {
        if (!player || !item || item->IsEquipped() || IsProtectedBag(item))
            return;

        ItemTemplate const* proto = item->GetTemplate();
        if (IsNeverSellTemplate(proto))
            return;

        bool sell = false;
        if ((mask & SELL_GRAY) && sConfigMgr->GetOption<bool>("GoldPerks.Sell.AllowGray", true) && proto->Quality == ITEM_QUALITY_POOR)
            sell = true;
        else if ((mask & SELL_WHITE) && sConfigMgr->GetOption<bool>("GoldPerks.Sell.AllowWhite", true) && ShouldSellWhite(proto))
            sell = true;
        else if ((mask & SELL_LOW_GREENS) && sConfigMgr->GetOption<bool>("GoldPerks.Sell.AllowGreen", true) && ShouldSellLowGreen(player, proto))
            sell = true;
        else if ((mask & SELL_ALL_GREENS) && sConfigMgr->GetOption<bool>("GoldPerks.Sell.AllowGreen", true) && ShouldSellAllGreen(proto))
            sell = true;

        if (!sell)
            return;

        SellCandidate candidate;
        candidate.bag = item->GetBagSlot();
        candidate.slot = item->GetSlot();
        candidate.entry = item->GetEntry();
        candidate.count = item->GetCount();
        candidate.quality = proto->Quality;
        candidate.vendorValue = proto->SellPrice * item->GetCount();
        out.push_back(candidate);
    }

    static std::vector<SellCandidate> BuildSellList(Player* player, uint32 mask)
    {
        std::vector<SellCandidate> items;
        if (!player)
            return items;

        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            AddCandidate(player, player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot), mask, items);

        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        {
            if (Bag* bag = player->GetBagByPos(bagSlot))
            {
                for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                    AddCandidate(player, player->GetItemByPos(bagSlot, slot), mask, items);
            }
        }

        return items;
    }

    static SellSummary BuildSummary(Player* player, uint32 mask, bool applyRandomCut)
    {
        SellSummary summary;
        std::vector<SellCandidate> items = BuildSellList(player, mask);

        for (SellCandidate const& candidate : items)
        {
            summary.vendorValue += candidate.vendorValue;
            if (candidate.quality == ITEM_QUALITY_POOR)
                ++summary.gray;
            else if (candidate.quality == ITEM_QUALITY_NORMAL)
                ++summary.white;
            else if (candidate.quality == ITEM_QUALITY_UNCOMMON)
                ++summary.green;
        }

        uint32 baseCut = 0;
        if (summary.green)
            baseCut = sConfigMgr->GetOption<uint32>("GoldPerks.Sell.Green.CutPct", 50);
        else if (summary.white)
            baseCut = sConfigMgr->GetOption<uint32>("GoldPerks.Sell.White.CutPct", 40);
        else
            baseCut = sConfigMgr->GetOption<uint32>("GoldPerks.Sell.Gray.CutPct", 35);

        summary.cutPct = applyRandomCut ? ApplySellCutUpcharge(player, baseCut) : baseCut;
        summary.donnyCut = (summary.vendorValue * summary.cutPct) / 100;
        summary.playerReceived = summary.vendorValue > summary.donnyCut ? summary.vendorValue - summary.donnyCut : 0;
        return summary;
    }

    static void ShowSellPreview(Player* player, uint32 mask)
    {
        SellSummary s = BuildSummary(player, mask, false);
        if (s.vendorValue == 0)
        {
            DonnySay(player, "Preview says you got nothing I want. That's bad for both of us. Mostly me.");
            return;
        }

        std::ostringstream ss;
        ss << "Preview: " << s.gray << " gray, " << s.white << " white, " << s.green << " green. "
           << "Vendor value: " << MoneyString(s.vendorValue) << ". Base cut before lies: " << s.cutPct << "%.";
        DonnySay(player, ss.str());
    }

    static SellSummary SellItems(Player* player, uint32 mask)
    {
        SellSummary summary;
        std::vector<SellCandidate> items = BuildSellList(player, mask);

        for (SellCandidate const& candidate : items)
        {
            summary.vendorValue += candidate.vendorValue;
            if (candidate.quality == ITEM_QUALITY_POOR)
                ++summary.gray;
            else if (candidate.quality == ITEM_QUALITY_NORMAL)
                ++summary.white;
            else if (candidate.quality == ITEM_QUALITY_UNCOMMON)
                ++summary.green;

            player->DestroyItem(candidate.bag, candidate.slot, true);
        }

        uint32 baseCut = 0;
        if (summary.green)
            baseCut = sConfigMgr->GetOption<uint32>("GoldPerks.Sell.Green.CutPct", 50);
        else if (summary.white)
            baseCut = sConfigMgr->GetOption<uint32>("GoldPerks.Sell.White.CutPct", 40);
        else
            baseCut = sConfigMgr->GetOption<uint32>("GoldPerks.Sell.Gray.CutPct", 35);

        std::string cutReason;
        summary.cutPct = ApplySellCutUpcharge(player, baseCut, &cutReason);
        summary.donnyCut = (summary.vendorValue * summary.cutPct) / 100;
        summary.playerReceived = summary.vendorValue > summary.donnyCut ? summary.vendorValue - summary.donnyCut : 0;

        if (summary.playerReceived)
            player->ModifyMoney(int32(summary.playerReceived));

        CharacterDatabase.Execute(
            "INSERT INTO `mod_gold_perks_log` (`guid`, `action`, `items_sold_gray`, `items_sold_white`, `items_sold_green`, `vendor_value`, `donny_cut`, `player_received`) VALUES ({}, 'sell', {}, {}, {}, {}, {}, {})",
            player->GetGUID().GetCounter(), summary.gray, summary.white, summary.green, summary.vendorValue, summary.donnyCut, summary.playerReceived);

        SetLastTime(player, TIME_LAST_SELL_USE, Now());

        if (!cutReason.empty())
            DonnySay(player, cutReason);

        return summary;
    }

    static bool PayBankFee(Player* player, Creature* creature)
    {
        if (!sConfigMgr->GetOption<bool>("GoldPerks.Bank.Enable", true))
        {
            DonnySay(player, "Banking's closed. Not my fault. Probably yours.");
            return false;
        }

        if (IsForbiddenForBank(player))
        {
            DonnySay(player, "No banking here. Too many witnesses, explosions, or both.");
            return false;
        }

        uint32 cooldown = sConfigMgr->GetOption<uint32>("GoldPerks.Bank.CooldownSeconds", 1800);
        uint32 lastUse = GetLastTime(player, TIME_LAST_BANK_USE);
        uint32 now = Now();
        if (lastUse && now < lastUse + cooldown)
        {
            DonnySay(player, "Your vault paperwork is still warm. Come back in " + std::to_string((lastUse + cooldown) - now) + " seconds.");
            return false;
        }

        uint32 baseCost = sConfigMgr->GetOption<uint32>("GoldPerks.Bank.BaseUseCostCopper", 15000);
        baseCost = uint32(float(baseCost) * DungeonFeeMultiplier(player, true));

        std::string reason;
        uint32 finalCost = ApplyServiceUpcharge(player, baseCost, &reason);

        if (!TakeMoney(player, finalCost))
        {
            DonnySay(player, "You want a bank with empty pockets? Bold strategy. Bad strategy. I need " + MoneyString(finalCost) + ".");
            return false;
        }

        SetLastTime(player, TIME_LAST_BANK_USE, now);
        CharacterDatabase.Execute(
            "INSERT INTO `mod_gold_perks_log` (`guid`, `action`, `donny_cut`) VALUES ({}, 'bank', {})",
            player->GetGUID().GetCounter(), finalCost);

        DonnySay(player, "Bank access costs " + MoneyString(finalCost) + ". Two to four hours, give or take. Mostly take." + (reason.empty() ? "" : " " + reason));
        player->GetSession()->SendShowBank(creature->GetGUID());
        return true;
    }

    static void ShowStatus(Player* player)
    {
        EnsureCharacterRow(player);
        ChatHandler handler(player->GetSession());
        handler.SendSysMessage("|cff66ff66Gold Perks|r");
        handler.PSendSysMessage("Use .goldperks summon to call Donny the Dealer.");
        handler.PSendSysMessage("Donny is temporary only. No permanent spawn is used.");
        handler.PSendSysMessage("Protected bag slot: %u", sConfigMgr->GetOption<uint32>("GoldPerks.Sell.ProtectedBagSlot", 4));
        handler.PSendSysMessage("Mang tax active for you: %s", IsMang(player) ? "YES. Donny noticed." : "no");
    }
}

class gold_perks_player_script : public PlayerScript
{
public:
    gold_perks_player_script() : PlayerScript("gold_perks_player_script") { }

    void OnPlayerLogin(Player* player) override
    {
        if (!GoldPerks::Enabled())
            return;

        GoldPerks::EnsureCharacterRow(player);

        if (sConfigMgr->GetOption<bool>("GoldPerks.AnnounceOnLogin", true))
            ChatHandler(player->GetSession()).SendSysMessage("|cff66ff66Gold Perks loaded.|r Use .goldperks summon to call Donny the Dealer.");
    }
};

class gold_perks_command_script : public CommandScript
{
public:
    gold_perks_command_script() : CommandScript("gold_perks_command_script") { }

    std::vector<ChatCommand> GetCommands() const override
    {
        static std::vector<ChatCommand> goldPerksCommandTable =
        {
            { "summon", SEC_PLAYER, false, &HandleSummonCommand, "" },
            { "status", SEC_PLAYER, false, &HandleStatusCommand, "" },
        };

        static std::vector<ChatCommand> commandTable =
        {
            { "goldperks", SEC_PLAYER, false, nullptr, "", goldPerksCommandTable },
        };

        return commandTable;
    }

    static bool HandleStatusCommand(ChatHandler* handler, char const* /*args*/)
    {
        Player* player = handler->GetSession()->GetPlayer();
        if (!player || !GoldPerks::Enabled())
            return false;

        GoldPerks::ShowStatus(player);
        return true;
    }

    static bool HandleSummonCommand(ChatHandler* handler, char const* /*args*/)
    {
        Player* player = handler->GetSession()->GetPlayer();
        if (!player || !GoldPerks::Enabled())
            return false;

        if (!sConfigMgr->GetOption<bool>("GoldPerks.Donny.Summon.Enable", true))
        {
            handler->SendSysMessage("Donny is not taking calls right now.");
            return true;
        }

        if (GoldPerks::IsForbiddenForSummon(player))
        {
            handler->SendSysMessage("Donny refuses to show up here.");
            return true;
        }

        uint32 now = GoldPerks::Now();
        uint32 cooldown = sConfigMgr->GetOption<uint32>("GoldPerks.Donny.Summon.CooldownSeconds", 300);
        uint32 lastSummon = GoldPerks::GetLastTime(player, GoldPerks::TIME_LAST_DONNY_SUMMON);
        if (lastSummon && now < lastSummon + cooldown)
        {
            handler->PSendSysMessage("Donny is dodging you for %u more seconds.", (lastSummon + cooldown) - now);
            return true;
        }

        uint32 baseCost = sConfigMgr->GetOption<uint32>("GoldPerks.Donny.Summon.CostCopper", 5000);
        baseCost = uint32(float(baseCost) * GoldPerks::DungeonFeeMultiplier(player, false));

        std::string reason;
        uint32 finalCost = GoldPerks::ApplyServiceUpcharge(player, baseCost, &reason);
        if (!GoldPerks::TakeMoney(player, finalCost))
        {
            handler->PSendSysMessage("Donny wants %s just to show up. You don't have it.", GoldPerks::MoneyString(finalCost).c_str());
            return true;
        }

        uint32 entry = sConfigMgr->GetOption<uint32>("GoldPerks.Donny.Entry", 900100);
        uint32 duration = sConfigMgr->GetOption<uint32>("GoldPerks.Donny.Summon.DurationSeconds", 60) * IN_MILLISECONDS;

        float x = player->GetPositionX() + 1.5f * std::cos(player->GetOrientation());
        float y = player->GetPositionY() + 1.5f * std::sin(player->GetOrientation());
        float z = player->GetPositionZ();
        float o = player->GetOrientation();

        // Donny is intentionally TEMPORARY. Do not insert him into the `creature` table.
        // This summon exists only for this call and despawns after GoldPerks.Donny.Summon.DurationSeconds.
        Creature* donny = player->SummonCreature(entry, x, y, z, o, TEMPSUMMON_TIMED_DESPAWN, duration);
        if (!donny)
        {
            player->ModifyMoney(int32(finalCost));
            handler->SendSysMessage("Donny failed to appear. Refunded summon fee.");
            return true;
        }

        GoldPerks::SetLastTime(player, GoldPerks::TIME_LAST_DONNY_SUMMON, now);
        CharacterDatabase.Execute(
            "INSERT INTO `mod_gold_perks_log` (`guid`, `action`, `donny_cut`) VALUES ({}, 'summon', {})",
            player->GetGUID().GetCounter(), finalCost);

        handler->PSendSysMessage("Donny charged %s to show up.%s%s", GoldPerks::MoneyString(finalCost).c_str(), reason.empty() ? "" : " ", reason.c_str());
        GoldPerks::DonnySay(player, "Alright, make it quick. All jobs take two to four hours, unless they don't. Donny ain't sure about this deal.");
        return true;
    }
};

class npc_donny_the_dealer : public CreatureScript
{
public:
    npc_donny_the_dealer() : CreatureScript("npc_donny_the_dealer") { }

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        if (!GoldPerks::Enabled())
            return false;

        ObjectGuid creator = creature->GetCreatorGUID();
        if (!creator.IsEmpty() && creator != player->GetGUID())
        {
            GoldPerks::DonnySay(player, "I ain't your goblin. Summon your own.");
            CloseGossipMenuFor(player);
            return true;
        }

        ClearGossipMenuFor(player);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Show my Gold Perks", GOSSIP_SENDER_MAIN, GoldPerks::ACTION_STATUS);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Preview safe sale: gray junk", GOSSIP_SENDER_MAIN, GoldPerks::ACTION_PREVIEW_SAFE);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Preview field cleanup: gray + white misc", GOSSIP_SENDER_MAIN, GoldPerks::ACTION_PREVIEW_FIELD);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Preview low-green cleanup", GOSSIP_SENDER_MAIN, GoldPerks::ACTION_PREVIEW_LOW_GREENS);
        AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, "Sell gray junk", GOSSIP_SENDER_MAIN, GoldPerks::ACTION_SELL_GRAY);
        AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, "Sell gray + white misc junk", GOSSIP_SENDER_MAIN, GoldPerks::ACTION_SELL_GRAY_WHITE);
        AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, "Sell gray + white misc + low greens", GOSSIP_SENDER_MAIN, GoldPerks::ACTION_SELL_GRAY_WHITE_LOW_GREENS);
        AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, "DANGEROUS: Sell all green items", GOSSIP_SENDER_MAIN, GoldPerks::ACTION_SELL_ALL_GREENS_DANGEROUS);
        AddGossipItemFor(player, GOSSIP_ICON_VENDOR, "Open my bank through Donny", GOSSIP_SENDER_MAIN, GoldPerks::ACTION_OPEN_BANK);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Explain your fees", GOSSIP_SENDER_MAIN, GoldPerks::ACTION_EXPLAIN_FEES);
        SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, creature->GetGUID());
        return true;
    }

    bool OnGossipSelect(Player* player, Creature* creature, uint32 /*sender*/, uint32 action) override
    {
        ClearGossipMenuFor(player);

        switch (action)
        {
            case GoldPerks::ACTION_STATUS:
                GoldPerks::ShowStatus(player);
                OnGossipHello(player, creature);
                return true;

            case GoldPerks::ACTION_PREVIEW_SAFE:
                GoldPerks::ShowSellPreview(player, GoldPerks::SELL_GRAY);
                OnGossipHello(player, creature);
                return true;

            case GoldPerks::ACTION_PREVIEW_FIELD:
                GoldPerks::ShowSellPreview(player, GoldPerks::SELL_GRAY | GoldPerks::SELL_WHITE);
                OnGossipHello(player, creature);
                return true;

            case GoldPerks::ACTION_PREVIEW_LOW_GREENS:
                GoldPerks::ShowSellPreview(player, GoldPerks::SELL_GRAY | GoldPerks::SELL_WHITE | GoldPerks::SELL_LOW_GREENS);
                OnGossipHello(player, creature);
                return true;

            case GoldPerks::ACTION_SELL_GRAY:
                DoSell(player, creature, GoldPerks::SELL_GRAY);
                return true;

            case GoldPerks::ACTION_SELL_GRAY_WHITE:
                DoSell(player, creature, GoldPerks::SELL_GRAY | GoldPerks::SELL_WHITE);
                return true;

            case GoldPerks::ACTION_SELL_GRAY_WHITE_LOW_GREENS:
                DoSell(player, creature, GoldPerks::SELL_GRAY | GoldPerks::SELL_WHITE | GoldPerks::SELL_LOW_GREENS);
                return true;

            case GoldPerks::ACTION_SELL_ALL_GREENS_DANGEROUS:
                DoSell(player, creature, GoldPerks::SELL_ALL_GREENS);
                return true;

            case GoldPerks::ACTION_OPEN_BANK:
                GoldPerks::PayBankFee(player, creature);
                CloseGossipMenuFor(player);
                creature->DespawnOrUnsummon(3000);
                return true;

            case GoldPerks::ACTION_EXPLAIN_FEES:
                GoldPerks::DonnySay(player, "Simple. I lie about the price, then I upcharge. If your name is Mang, that's another ten percent. All work takes two to four hours. Maybe.");
                OnGossipHello(player, creature);
                return true;

            default:
                CloseGossipMenuFor(player);
                return true;
        }
    }

private:
    static bool SellCooldownReady(Player* player)
    {
        uint32 cooldown = sConfigMgr->GetOption<uint32>("GoldPerks.Sell.CooldownSeconds", 0);
        if (!cooldown)
            return true;

        uint32 now = GoldPerks::Now();
        uint32 lastUse = GoldPerks::GetLastTime(player, GoldPerks::TIME_LAST_SELL_USE);
        if (lastUse && now < lastUse + cooldown)
        {
            GoldPerks::DonnySay(player, "Junk paperwork is still drying. Come back in " + std::to_string((lastUse + cooldown) - now) + " seconds.");
            return false;
        }

        return true;
    }

    static void DoSell(Player* player, Creature* creature, uint32 mask)
    {
        if (!sConfigMgr->GetOption<bool>("GoldPerks.Sell.Enable", true))
        {
            GoldPerks::DonnySay(player, "Buying junk is off. Regulations. Terrible things.");
            CloseGossipMenuFor(player);
            return;
        }

        if (!SellCooldownReady(player))
        {
            CloseGossipMenuFor(player);
            return;
        }

        if ((mask & GoldPerks::SELL_ALL_GREENS) && !sConfigMgr->GetOption<bool>("GoldPerks.Sell.Green.AllowDangerousAllGreens", true))
        {
            GoldPerks::DonnySay(player, "All-green buying is disabled. Donny's crooked, not suicidal.");
            CloseGossipMenuFor(player);
            return;
        }

        GoldPerks::SellSummary s = GoldPerks::SellItems(player, mask);
        if (s.vendorValue == 0)
        {
            GoldPerks::DonnySay(player, "You got nothing I want. Which is impressive, considering my standards.");
            CloseGossipMenuFor(player);
            return;
        }

        std::ostringstream ss;
        ss << "I took " << s.gray << " gray, " << s.white << " white, and " << s.green << " green items. "
           << "Vendor value was " << GoldPerks::MoneyString(s.vendorValue) << ". "
           << "My cut was " << s.cutPct << "% (" << GoldPerks::MoneyString(s.donnyCut) << "). "
           << "You get " << GoldPerks::MoneyString(s.playerReceived) << ". No refunds.";
        GoldPerks::DonnySay(player, ss.str());

        CloseGossipMenuFor(player);
        creature->DespawnOrUnsummon(3000);
    }
};

void Addmod_gold_perksScripts()
{
    new gold_perks_player_script();
    new gold_perks_command_script();
    new npc_donny_the_dealer();
}
