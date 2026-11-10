#pragma once
#include <string_view>

namespace dusklight_online::net {
// Lobby control remains on the authoritative relay stream. Peer-owned voice
// settings are checked against the authenticated room owner on receipt.
inline bool peer_delivery_type(std::string_view type) {
    constexpr std::string_view types[] = {
        "progression_state","sync_request","event_bit","tbox_bit","switch_bit","room_switch_bit","web_timer","room_actor_action","floor_switch_state","item_bit",
        "dungeon_item_bit","save_snapshot","key_num","light_drop_num","light_drop_get_flag",
        "max_life_update","bottle_slots","bomb_bag_slot","rupee_count","rupee_delta","poe_count",
        "malo_fundraising","charlo_offering","fish_record","collect_smell","item_get","trade_item","rando_item_get",
        "item_first_bit","collect_crystal","collect_mirror","dark_clear_lv","transform_lv","region_bit",
        "collect","visited_room","letter_get","pvp_hit","coop_presence","coop_settings","coop_enemy_damage","ganondorf_owner_claim","ganondorf_owner",
        "ganondorf_hit","ganondorf_reaction","ganondorf_player_damage","ganondorf_state","ooccoo_state",
        "chat","voice_settings","voice_settings_request",
        "save_recovery_setting","save_recovery_setting_request",
        "speedrun_check","speedrun_ready","speedrun_prompt","speedrun_player_ready",
        "speedrun_ready_count","speedrun_countdown","speedrun_cancel",
        "speedrun_start","speedrun_reset"};
    for(auto value:types) if(type==value) return true;
    return false;
}
}
