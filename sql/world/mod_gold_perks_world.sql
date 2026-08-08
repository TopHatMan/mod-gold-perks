-- Donny the Dealer, Gold Perks Broker
-- Entry is configurable, default 900100.
-- This file creates ONLY the creature_template/model. It does NOT insert into `creature`,
-- so Donny will not permanently spawn anywhere in the world.
-- Donny appears only through .goldperks summon as a temporary summon.

DELETE FROM `creature_template_model` WHERE `CreatureID` = 900100;
DELETE FROM `creature_template` WHERE `entry` = 900100;

INSERT INTO `creature_template`
(`entry`, `name`, `subname`, `minlevel`, `maxlevel`, `faction`, `npcflag`, `speed_walk`, `speed_run`, `scale`, `rank`, `unit_class`, `unit_flags`, `type`, `type_flags`, `AIName`, `ScriptName`)
VALUES
(900100, 'Donny the Dealer', 'Gold Perks Broker', 60, 60, 35, 1, 1.0, 1.14286, 1.08, 0, 1, 0, 7, 0, '', 'npc_donny_the_dealer');

-- Copy the exact goblin model from "Honest" Max <Slightly Used Flying Mounts>, entry 30464.
-- Run this first if you want to see his display ID:
-- SELECT `CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, `VerifiedBuild`
-- FROM `creature_template_model`
-- WHERE `CreatureID` = 30464;

INSERT INTO `creature_template_model`
(`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, `VerifiedBuild`)
SELECT
900100, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, `VerifiedBuild`
FROM `creature_template_model`
WHERE `CreatureID` = 30464;

-- Fallback check for older/custom schemas that still have modelid fields on creature_template:
-- SELECT `entry`, `name`, `modelid1`, `modelid2`, `modelid3`, `modelid4`
-- FROM `creature_template`
-- WHERE `entry` = 30464;

-- Runtime verification after import/restart:
-- SELECT `entry`, `name`, `npcflag`, `ScriptName`
-- FROM `creature_template`
-- WHERE `entry` = 900100;
-- Expected: npcflag includes GOSSIP (1), ScriptName = 'npc_donny_the_dealer'.
