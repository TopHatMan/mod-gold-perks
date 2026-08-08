#!/usr/bin/env python3
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
CPP = ROOT / 'src' / 'mod_gold_perks.cpp'
CONF = ROOT / 'conf' / 'mod_gold_perks.conf.dist'
WORLD_SQL = ROOT / 'sql' / 'world' / 'mod_gold_perks_world.sql'

GET_OPTION_RE = re.compile(r'GetOption\s*<[^>]+>\s*\(\s*"(GoldPerks\.[A-Za-z0-9_.]+)"')
CONFIG_RE = re.compile(r'^\s*(GoldPerks\.[A-Za-z0-9_.]+)\s*=', re.MULTILINE)
OLD_PSEND_RE = re.compile(r'PSendSysMessage\([^\n;]*%[sudi]')


def main() -> int:
    cpp = CPP.read_text(encoding='utf-8')
    conf = CONF.read_text(encoding='utf-8')
    world_sql = WORLD_SQL.read_text(encoding='utf-8')
    failed = False

    if not conf.startswith('[worldserver]\n'):
        print('ERROR: mod_gold_perks.conf.dist must start with [worldserver].')
        failed = True

    old_formats = OLD_PSEND_RE.findall(cpp)
    if old_formats:
        print('ERROR: printf-style PSendSysMessage formatting remains; current AzerothCore requires {} placeholders.')
        for match in old_formats:
            print('  -', match[:120])
        failed = True

    source_keys = set(GET_OPTION_RE.findall(cpp))
    config_keys = CONFIG_RE.findall(conf)
    duplicates = sorted({key for key in config_keys if config_keys.count(key) > 1})
    config_set = set(config_keys)

    if duplicates:
        print('ERROR: duplicate GoldPerks config keys:')
        for key in duplicates:
            print('  -', key)
        failed = True

    missing = sorted(source_keys - config_set)
    stale = sorted(config_set - source_keys)
    if missing:
        print('ERROR: source reads GoldPerks options absent from canonical config:')
        for key in missing:
            print('  -', key)
        failed = True
    if stale:
        print('ERROR: canonical config contains unused GoldPerks options:')
        for key in stale:
            print('  -', key)
        failed = True

    if "'npc_donny_the_dealer'" not in world_sql or '900100' not in world_sql:
        print('ERROR: world SQL no longer defines the canonical Donny script binding.')
        failed = True

    required_source_markers = [
        'InspectDonnyTemplate',
        'SetPocketRankVerified',
        'No summon fee charged',
        'Magical Overflow unavailable: set LFG.MailItemOnFullInventory = 2',
    ]
    for marker in required_source_markers:
        if marker not in cpp:
            print(f'ERROR: missing runtime-safety marker: {marker}')
            failed = True

    print(f'Gold Perks runtime contract: {len(config_set)} config keys, {len(source_keys)} source keys.')
    if failed:
        return 1
    print('Gold Perks runtime contract OK.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
