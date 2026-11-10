#!/usr/bin/env python3
"""Source-level Ooccoo integration tripwires, not an engine/runtime test.

Run from any directory:
    python tests/ooccoo_source_audit.py --dusklight-dir ../dusklight-main
No external Python packages are needed. Optional vanilla-source checks verify
that the assumptions used by the model are still visible in the supplied tree.
"""
from __future__ import annotations

import argparse
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dusklight-dir", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    adapter = (root / "src/game/game_adapter.cpp").read_text(encoding="utf-8")
    model = (root / "include/dusklight_online/game/ooccoo_sync.hpp").read_text(encoding="utf-8")
    wire = (root / "include/dusklight_online/game/ooccoo_wire.hpp").read_text(encoding="utf-8")
    checks = 0

    def require(ok: bool, message: str) -> None:
        nonlocal checks
        checks += 1
        if not ok:
            raise SystemExit(f"FAILED: {message}")

    def section(start: str, end: str) -> str:
        first = adapter.index(start)
        return adapter[first:adapter.index(end, first + len(start))]

    catchup = section("void GameAdapter::flush_ooccoo_catchup(", "nlohmann::json GameAdapter::ooccoo_snapshot_state(")
    require('if (ooccooCatchupPending_) packet["request_state"] = true;' in catchup and
            "ooccooReplyPending_ = false;" in catchup, "one-shot catch-up replies do not request a reply")
    require("flush_ooccoo_catchup();" in section("void GameAdapter::update(", "void GameAdapter::capture_local_mutations_before_remote("),
            "catch-up flush is connected to the game tick")
    require("observe_local_ooccoo_state" not in adapter, "no slot-polling acquisition publisher")
    require("sharedOoccooState_" not in adapter, "no legacy single-owner state")
    for field in ("owner_stage", "return_stage", "return_room", "return_x", "return_y", "return_z",
                  "return_angle", "has_return_mark", "clear_stage"):
        require(f'"{field}"' not in adapter, f"no legacy wire field {field}")
    require("dComIfGs_setWarpItemData(name.c_str()" in adapter and
            "dComIfGs_setLastWarpAcceptStage(static_cast<s8>(stage))" in adapter,
            "remote Jr receives the same native mark fields as warpOutProc")
    require("DEFINE_HOOK(&dMeter2Info_c::warpOutProc, OoccooWarpOutHook)" in adapter and
            "mods::hook::add_post<OoccooWarpOutHook>(&ooccoo_warp_out_post)" in adapter and
            "ooccooState_.record_local(facts)" in adapter,
            "native warp-out publishes the validated Jr mark separately from pickup")
    key_items = section("bool is_synced_key_item(", "bool is_synced_item_first_bit(")
    require("dItemNo_TKS_LETTER_e" not in key_items, "Note excluded from generic item_get")
    bit_hook = section("void memory_dungeon_item_on_post(", "HookAction visited_room_on_pre(")
    require("if (kind == dSv_memBit_c::OOCCOO_NOTE) return;" in bit_hook,
            "all raw Ooccoo-bit publications blocked, not just boss side effects")
    require("kinds.push_back(6)" not in adapter, "snapshot does not publish raw warp bit")
    require("case 6: return ApplyResult::IgnoredByPolicy" in adapter and
            "case 6: break; // Legacy raw Ooccoo" in adapter,
            "live and snapshot raw warp-bit receivers are blocked")
    rando = section("ApplyResult GameAdapter::consume_randomizer(", "ApplyResult GameAdapter::consume_pvp_hit(")
    require("else if (remote_pickup_requires_grant(itemToApply))" in rando and
            rando.index("if (ooccooGrant) {") < rando.index("execItemGet(itemToApply"),
            "randomized Ooccoo cannot fall through to raw execItemGet")
    for start, end in (("void GameAdapter::notify_local_save_reset(", "void GameAdapter::notify_local_save_loaded("),
                       ("void GameAdapter::clear_disabled_sync_flags_state(", "void GameAdapter::update("),
                       ("void GameAdapter::reset_session(", "const std::string& GameAdapter::last_error(")):
        require("ooccooState_.reset()" in section(start, end), f"Ooccoo reset at {start}")
    apply = section("void GameAdapter::apply_shared_ooccoo_local_form(", "nlohmann::json GameAdapter::make_save_snapshot(")
    require("stableStageName_ != scene.name" in apply and "stableRoom_ != scene.room" in apply,
            "stable-tick count must belong to current scene")
    require("dMeter2Info_getWarpStatus() != 0" in apply and "dComIfGp_isPauseFlag()" in apply,
            "active warps/menus defer mutation")
    require("if (here == stage) applyFacts" in apply, "live bit writes require validated scene identity")
    require("bits.onStageBossEnemy()" not in apply and
            "(progress.completed & bit) == 0" in apply,
            "cached Ooccoo completion cannot restore a cleared boss flag")
    require("ooccooState_.observe_native_completion(nativeFacts.completed)" in apply and
            "progress_.completed = before.completed" in model,
            "Ooccoo decisions use native completion, not received completion")
    receive = section("bool GameAdapter::accept_ooccoo_state(",
                      "void GameAdapter::apply_shared_ooccoo_local_form(")
    require(receive.index("observe_native_completion(") < receive.index("merge_remote(*decoded)"),
            "native completion is refreshed before incoming receipts are merged")
    snapshot = section("nlohmann::json GameAdapter::ooccoo_snapshot_state(",
                       "bool GameAdapter::accept_ooccoo_state(")
    require("snapshot.completed = native_ooccoo_facts(false, stage_ready()).completed" in snapshot,
            "Ooccoo snapshots cannot republish stale completed bits")
    require("native_ooccoo_facts(false, true)" in apply, "reconcile cannot mint acquisitions from raw flags")
    require("newlyCompleted" in apply and '"type", "ooccoo_state"' in apply,
            "local boss completion publishes Ooccoo progress")
    require("ooccoo::valid_anchor(stage, anchor)" in apply,
            "untrusted return anchors are checked before writing native warp data")
    require("add_pre<OoccooReunionHook>" in adapter and "uninstall<OoccooReunionHook>" in adapter,
            "cave reunion hook installed and removed")
    require("add_pre<OoccooWarpActorHook>" in adapter and "uninstall<OoccooWarpActorHook>" in adapter,
            "warp scene-start helper also guarded, without deleting story actors")
    send = section("std::string GameAdapter::encode_manual_full_state(", "bool GameAdapter::apply_manual_full_state(")
    receive = section("bool GameAdapter::apply_manual_full_state(", "void GameAdapter::update_pending_sync_replies(")
    require("snapshotPlayer.getPlayerLastMarkInfo().init()" in send,
            "manual wire redacts donor mark")
    require("donorPlayer.getPlayerLastMarkInfo().init()" in receive and
            receive.index("donorPlayer.getPlayerLastMarkInfo().init()") < receive.index("clear_replaced_save_progression_state()"),
            "manual receiver sanitizes before copying/replay buffers")
    snapshots = section("ApplyResult GameAdapter::apply_save_snapshot(", "void GameAdapter::poll_local_state(")
    require(snapshots.index("ooccoo::decode") < snapshots.index("apply_manual_full_state("),
            "manual Ooccoo metadata validated before raw save replacement")
    require("ooccooState_.restore(*ooccooProgress, 0," in snapshots,
            "manual replacement also replaces ownership metadata")
    require('"ooccoo-progress-v2"' in adapter and "delete_blob(mod_ctx, kLegacyOoccooSaveBlob" in adapter,
            "versioned save companion replaces legacy detached owner")
    require('state.size() != (*version >= 3 ? 6 : 5)' in wire and
            'bounded_integer(state["acquired"], DungeonMask)' in wire,
            "wire format is strict and bounded")
    require("valid_anchor" in model and "choose_anchor" in model and
            "installReturnStage" in model,
            "bounded native return anchors converge and drive remote Jr projection")

    if args.dusklight_dir:
        vanilla = args.dusklight_dir.resolve()
        save = (vanilla / "include/d/d_save.h").read_text(encoding="utf-8")
        item = (vanilla / "src/d/d_item.cpp").read_text(encoding="utf-8")
        alink = (vanilla / "src/d/actor/d_a_alink.cpp").read_text(encoding="utf-8")
        demo = (vanilla / "src/d/actor/d_a_alink_demo.inc").read_text(encoding="utf-8")
        meter = (vanilla / "src/d/d_meter2_info.cpp").read_text(encoding="utf-8")
        maps = (vanilla / "src/dusk/map_loader_definitions.h").read_text(encoding="utf-8")
        boss = save[save.index("void onStageBossEnemy()"):save.index("void offStageBossEnemy()")]
        require("onDungeonItem(STAGE_BOSS_ENEMY)" in boss and "onDungeonItem(OOCCOO_NOTE)" in boss,
                "vanilla boss clear still sets Ooccoo bit as a side effect")
        city = item[item.index("void item_func_LV7_DUNGEON_EXIT()"):item.index("void item_func_LV7_DUNGEON_EXIT()") + 180]
        require("dItemNo_LV7_DUNGEON_EXIT_e" in city and "onDungeonItemWarp" not in city,
                "special City item does not acquire normal warp flag")
        require("!checkDungeon() && !checkBossRoom() && checkItemGet(dItemNo_DUNGEON_EXIT_e, 1)" in alink,
                "vanilla overworld conversion targets ordinary Sr only")
        require("!checkLv7DungeonShop()" in demo and 'checkStageName("D_MN07")' in demo,
                "vanilla has a City shop exception")
        warp = meter[meter.index("void dMeter2Info_c::warpOutProc()"):meter.index("void dMeter2Info_c::resetMeterString()")]
        require("dComIfGs_setWarpItemData" in warp and "dItemNo_DUNGEON_BACK_e" in warp,
                "vanilla warp out creates local mark and Jr")
        for name in ("D_MN05", "D_MN04", "D_MN01", "D_MN10", "D_MN11", "D_MN06", "D_MN07"):
            require(all(f'"{name}{suffix}"' in maps for suffix in ("", "A", "B")),
                    f"vanilla map definitions confirm {name} and both arenas")
    print(f"Ooccoo source-level audit: {checks} checks passed (not an engine execution test).")


if __name__ == "__main__":
    main()
