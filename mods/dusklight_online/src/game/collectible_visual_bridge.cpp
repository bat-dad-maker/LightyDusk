#include "dusklight_online/game/collectible_visual_bridge.hpp"

#include <unordered_set>

#include "d/dolzel.h"

// These fields are private only as a source-level encapsulation detail. Keep
// the chest visual repair isolated here without changing the SDK checkout or
// relying on a fixed byte offset.
#define private public
#include "d/actor/d_a_tbox.h"
#include "d/actor/d_a_door_mbossL1.h"
#include "d/actor/d_a_door_shutter.h"
#include "d/actor/d_a_obj_cblock.h"
#include "d/actor/d_a_obj_kshutter.h"
#include "d/actor/d_a_obj_lv4PoGate.h"
#include "d/actor/d_a_obj_scannon.h"
#include "d/actor/d_a_obj_bky_rock.h"
#include "d/actor/d_a_obj_bmWindow.h"
#include "d/actor/d_a_obj_hbombkoya.h"
#include "d/actor/d_a_obj_lv4digsand.h"
#include "d/actor/d_a_obj_lv5FloorBoard.h"
#include "d/actor/d_a_obj_lv5IceWall.h"
#include "d/actor/d_a_obj_lv5SwIce.h"
#include "d/actor/d_a_obj_movebox.h"
#include "d/actor/d_a_obj_picture.h"
#include "d/actor/d_a_obj_rstair.h"
#include "d/actor/d_a_obj_rfHole.h"
#include "d/actor/d_a_obj_well_cover.h"
#undef private

#include "d/actor/d_a_e_pz.h"
#include "d/actor/d_a_obj_carry.h"
#include "d/actor/d_a_obj_Lv5Key.h"
#include "d/actor/d_a_obj_iceblock.h"
#include "d/actor/d_a_obj_kgate.h"
#include "d/actor/d_a_obj_keyhole.h"
#include "d/actor/d_a_obj_drop.h"
#include "d/actor/d_a_obj_item.h"
#include "d/actor/d_a_obj_life_container.h"
#include "d/actor/d_a_obj_bbox.h"
#include "d/actor/d_a_obj_so.h"
#include "d/actor/d_a_obj_smallkey.h"
#include "d/actor/d_a_obj_sword.h"
#include "d/actor/d_a_obj_web0.h"
#include "d/actor/d_a_obj_web1.h"
#include "d/d_com_inf_game.h"
#include "d/d_door_param2.h"
#include "d/d_path.h"
#include "d/d_save.h"
#include "d/d_stage.h"
#include "d/d_tresure.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_name.h"

namespace dusklight_online::game {
namespace {

std::unordered_set<fpc_ProcID> sOpeningTboxes;

void* judge_tbox(void* actor, void* data) {
    if (actor == nullptr || fopAcM_GetName(actor) != fpcNm_TBOX_e) return nullptr;
    auto* tbox = static_cast<daTbox_c*>(actor);
    return tbox->getTboxNo() == *static_cast<int*>(data) ? actor : nullptr;
}

void* judge_key(void* actor, void* data) {
    if (actor == nullptr || fopAcM_GetName(actor) != fpcNm_Obj_SmallKey_e) return nullptr;
    auto* key = static_cast<daKey_c*>(actor);
    return key->getSaveBitNo() == *static_cast<int*>(data) ? actor : nullptr;
}

void* judge_drop(void* actor, void* data) {
    if (actor == nullptr || fopAcM_GetName(actor) != fpcNm_Obj_Drop_e) return nullptr;
    auto* drop = static_cast<daObjDrop_c*>(actor);
    return drop->getSave() == *static_cast<int*>(data) ? actor : nullptr;
}

void* judge_life(void* actor, void* data) {
    if (actor == nullptr || fopAcM_GetName(actor) != fpcNm_Obj_LifeContainer_e) return nullptr;
    auto* life = static_cast<daObjLife_c*>(actor);
    return life->getSaveBitNo() == *static_cast<int*>(data) ? actor : nullptr;
}

void* judge_item(void* actor, void* data) {
    if (actor == nullptr || fopAcM_GetName(actor) != fpcNm_ITEM_e) return nullptr;
    auto* item = static_cast<daItem_c*>(actor);
    return daItem_prm::getItemBitNo(item) == *static_cast<int*>(data) ? actor : nullptr;
}

void* judge_sword(void* actor, void* data) {
    if (actor == nullptr || fopAcM_GetName(actor) != fpcNm_Obj_Sword_e) return nullptr;
    auto* sword = static_cast<daObjSword_c*>(actor);
    return sword->getItemBit() == *static_cast<int*>(data) ? actor : nullptr;
}

void* judge_sewers_breakable_box(void* actor, void* data) {
    if (actor == nullptr || fopAcM_GetName(actor) != fpcNm_Obj_BBox_e) return nullptr;
    auto* box = static_cast<daObjBBox_c*>(actor);
    return box->getSwNo() == *static_cast<int*>(data) ? actor : nullptr;
}

void* repair_chain_block(void* actor, void* data) {
    if (actor == nullptr || fopAcM_GetName(actor) != fpcNm_Obj_ChainBlock_e) return nullptr;
    auto* block = static_cast<daObjCBlk_c*>(actor);
    const int flag = *static_cast<int*>(data);
    if (block->getSwNo() != flag || block->getPathID() == 0xFF ||
        !fopAcM_isSwitch(block, block->getSwNo())) {
        return nullptr;
    }
    dPath* path = dPath_GetRoomPath(block->getPathID(), fopAcM_GetHomeRoomNo(block));
    if (path == nullptr || path->m_num < 2) return nullptr;
    dPnt* point = dPath_GetPnt(path, 1);
    if (point == nullptr || block->current.pos.abs(point->m_position) <= 1.0f) return nullptr;
    block->current.pos = point->m_position;
    block->old.pos = block->current.pos;
    block->setBaseMtx();
    fopAcM_SetMtx(block, block->model1->getBaseTRMtx());
    return actor;
}

void* repair_jump_tbox(void* actor, void* data) {
    if (actor == nullptr || fopAcM_GetName(actor) != fpcNm_TBOX_e) return nullptr;
    auto* tbox = static_cast<daTbox_c*>(actor);
    const int flag = *static_cast<int*>(data);
    if (tbox->getSwNo() != flag ||
        !dComIfGs_isSwitch(tbox->getSwNo(), fopAcM_GetRoomNo(tbox))) {
        return nullptr;
    }
    const int funcType = tbox->getFuncType();
    if (!((funcType == 6 && tbox->getSwType() == 15) || funcType == 7)) return nullptr;
    dPath* path = dPath_GetRoomPath(tbox->getPathId(), -1);
    if (path == nullptr || path->m_num <= 0) return nullptr;
    dPnt* point = &path->m_points[path->m_num - 1];
    if (tbox->current.pos.abs(point->m_position) <= 1.0f) return nullptr;
    tbox->current.pos = point->m_position;
    tbox->home.pos = point->m_position;
    tbox->old.pos = tbox->current.pos;
    tbox->attention_info.position = tbox->current.pos;
    tbox->eyePos = tbox->current.pos;
    tbox->initBaseMtx();
    fopAcM_SetMtx(tbox, tbox->mpModel->getBaseTRMtx());
    dTres_c::setPosition(tbox->getTboxNo(), &tbox->current.pos);
    return actor;
}

void* repair_lv4_poe_gate(void* actor, void*) {
    if (actor == nullptr || fopAcM_GetName(actor) != fpcNm_Obj_Lv4PoGate_e) return nullptr;
    auto* gate = static_cast<daLv4PoGate_c*>(actor);
    const bool wasClosed = gate->mInitMove != 0 || gate->mMoveValue != gate->mMoveTarget ||
                           gate->mMode != daLv4PoGate_c::MODE_WAIT_e;
    if (gate->mSw != 0xFF) fopAcM_offSwitch(gate, gate->mSw);
    gate->mInitMove = 0;
    gate->mMoveValue = gate->mMoveTarget;
    gate->speedF = 0.0f;
    gate->init_modeWait();
    gate->setBaseMtx();
    fopAcM_SetMtx(gate, gate->mpModel->getBaseTRMtx());
    return wasClosed ? actor : nullptr;
}

void* judge_pz_mist(void* actor, void* data) {
    if (actor == nullptr || fopAcM_GetName(actor) != fpcNm_KYTAG12_e) return nullptr;
    return fopAcM_GetRoomNo(static_cast<fopAc_ac_c*>(actor)) == *static_cast<int*>(data)
               ? actor
               : nullptr;
}

void* repair_phantom_zant(void* actor, void* data) {
    if (actor == nullptr || fopAcM_GetName(actor) != fpcNm_E_PZ_e) return nullptr;
    auto* phantom = static_cast<daE_PZ_c*>(actor);
    const int flag = *static_cast<int*>(data);
    if (phantom->bitSw != flag || (phantom->arg0 != 0 && phantom->arg0 != 1) ||
        phantom->bitSw == 0xFF || !fopAcM_isSwitch(phantom, phantom->bitSw)) {
        return nullptr;
    }
    int room = fopAcM_GetRoomNo(phantom);
    if (fopAcIt_Judge(judge_pz_mist, &room) == nullptr) {
        cXyz pos(0.0f, 0.0f, -1300.0f);
        fopAcM_create(fpcNm_KYTAG12_e, 1, &pos, room, nullptr, nullptr, -1);
    }
    fopAcM_delete(phantom);
    return actor;
}

void* repair_sky_cannon(void* actor, void* data) {
    if (actor == nullptr || fopAcM_GetName(actor) != fpcNm_Obj_SCannon_e) return nullptr;
    auto* cannon = static_cast<daSCannon_c*>(actor);
    const int flag = *static_cast<int*>(data);
    if (cannon->mLayerNo != 1 || cannon->getSw1() != flag ||
        !fopAcM_isSwitch(cannon, cannon->getSw1())) {
        return nullptr;
    }
    if (cannon->mIsPortal == FALSE && cannon->mMode == daSCannon_c::MODE_END &&
        cannon->mDrawShadow == TRUE) {
        return nullptr;
    }
    cannon->mIsPortal = FALSE;
    cannon->mDrawShadow = TRUE;
    cannon->mMode = daSCannon_c::MODE_END;
    cannon->setModelMtx();
    fopAcM_SetMtx(cannon, cannon->mpModels[cannon->mIsRepaired]->getBaseTRMtx());
    return actor;
}

struct WebTimerSearch {
    int actorName;
    int room;
    uint32_t params;
    int timer;
};

void* repair_lakebed_rot_stair(void* actor, void*) {
    if (actor == nullptr || fopAcM_GetName(actor) != fpcNm_Obj_RotStair_e) return nullptr;
    auto* stair = static_cast<daObjRotStair_c*>(actor);
    if (fopAcM_GetRoomNo(stair) != 3 || stair->getSwNo() != 0) return nullptr;
    int target = -1;
    for (int i = 0; i < 4; ++i) {
        if (dComIfGs_isSwitch(stair->getSwNo() + i, fopAcM_GetRoomNo(stair))) {
            target = i;
            break;
        }
    }
    if (target < 0) return nullptr;
    for (int i = 0; i < 4; ++i) stair->mIsSw[i] = i == target;
    if (stair->mMode == daObjRotStair_c::MODE_WAIT) {
        static constexpr s16 kTargetAngles[] = {0x7FFF, 0x4000, 0x0000, -0x4000};
        stair->field_0x5e3 = static_cast<s8>(target);
        stair->mTargetAngle = kTargetAngles[target];
        stair->shape_angle.y = kTargetAngles[target];
        stair->field_0x5e2 = false;
        stair->init_modeWait();
    }
    return actor;
}

void* start_web_timer(void* actor, void* data) {
    if (actor == nullptr || data == nullptr) return nullptr;
    const auto& search = *static_cast<const WebTimerSearch*>(data);
    auto* web = static_cast<fopAc_ac_c*>(actor);
    if (fpcM_GetName(actor) != search.actorName || fopAcM_GetRoomNo(web) != search.room ||
        fopAcM_GetParam(actor) != search.params) {
        return nullptr;
    }

    if (search.actorName == fpcNm_OBJ_WEB0_e) {
        auto* wallWeb = static_cast<obj_web0_class*>(actor);
        if (wallWeb->mDeleteTimer != 0 || (search.timer != 1 && search.timer != 41)) {
            return nullptr;
        }
        wallWeb->mDeleteTimer = static_cast<u8>(search.timer);
    } else if (search.actorName == fpcNm_OBJ_WEB1_e) {
        auto* floorWeb = static_cast<obj_web1_class*>(actor);
        if (floorWeb->mDeleteTimer != 0 || search.timer != 1) return nullptr;
        floorWeb->mDeleteTimer = 1;
    } else {
        return nullptr;
    }
    return actor;
}

struct WebSwitchSearch {
    int actorName;
    int room;
    int flag;
    uint32_t params;
};

void* repair_web_switch(void* actor, void* data) {
    if (actor == nullptr || data == nullptr) return nullptr;
    const auto& search = *static_cast<const WebSwitchSearch*>(data);
    auto* web = static_cast<fopAc_ac_c*>(actor);
    if (fpcM_GetName(actor) != search.actorName || fopAcM_GetRoomNo(web) != search.room ||
        fopAcM_GetParam(actor) != search.params || search.flag == 0xFF ||
        static_cast<int>((search.params >> 24) & 0xFF) != search.flag ||
        !dComIfGs_isSwitch(search.flag, search.room)) {
        return nullptr;
    }

    // The timer message normally owns the live effect. Retain the completion
    // switch as the authoritative fallback for paths which skip that timer.
    fopAcM_delete(web);
    return actor;
}

struct RoomActorSearch {
    int actorName;
    int room;
    uint32_t params;
    RoomActorAction action;
    int actionArgument;
};

std::unordered_set<void*> sRemoteMoveboxes;
std::unordered_set<void*> sRemoteIceblocks;

bool room_actor_action_supported(int actorName, RoomActorAction action) {
    switch (actorName) {
    case fpcNm_BkyRock_e:
        return action == RoomActorAction::DamageStage || action == RoomActorAction::Break;
    case fpcNm_Obj_Lv4DigSand_e:
        return action == RoomActorAction::DigStart || action == RoomActorAction::Break;
    case fpcNm_Obj_IceWall_e:
        return action == RoomActorAction::PartialBreak || action == RoomActorAction::Break;
    case fpcNm_Obj_HBombkoya_e:
    case fpcNm_Obj_RfHole_e:
    case fpcNm_Obj_BmWindow_e:
    case fpcNm_Obj_WellCover_e:
    case fpcNm_Obj_BBox_e:
    case fpcNm_Obj_Lv5FBoard_e:
    case fpcNm_Obj_Picture_e:
    case fpcNm_Obj_Lv5SwIce_e:
        return action == RoomActorAction::Break;
    case fpcNm_Obj_Movebox_e:
        return action == RoomActorAction::MoveStep;
    case fpcNm_Obj_RotStair_e:
        return action == RoomActorAction::RotateTo;
    case fpcNm_Obj_IceBlock_e:
        return action == RoomActorAction::Slide;
    default:
        return false;
    }
}

int room_actor_switch_flag(int actorName, uint32_t params) {
    switch (actorName) {
    case fpcNm_BkyRock_e:
    case fpcNm_Obj_Picture_e:
        return static_cast<int>((params >> 4) & 0xFF);
    case fpcNm_Obj_HBombkoya_e:
        return static_cast<int>((params >> 8) & 0xFF);
    case fpcNm_Obj_RfHole_e:
    case fpcNm_Obj_BmWindow_e:
    case fpcNm_Obj_WellCover_e:
    case fpcNm_Obj_BBox_e:
    case fpcNm_Obj_Lv5FBoard_e:
    case fpcNm_Obj_Lv4DigSand_e:
    case fpcNm_Obj_IceWall_e:
    case fpcNm_Obj_Lv5SwIce_e:
        return static_cast<int>(params & 0xFF);
    default:
        return -1;
    }
}

bool apply_room_actor_action(void* actor, int actorName, RoomActorAction action,
                             int actionArgument) {
    static constexpr u16 kBkyFirstParticles[] = {0x89C4, 0x89C5, 0x89C6, 0x89C7};
    static constexpr u16 kBkySecondParticles[] = {0x89C2, 0x89C3, 0x89C4,
                                                  0x89C5, 0x89C6, 0x89C7};
    switch (actorName) {
    case fpcNm_BkyRock_e: {
        auto* rock = static_cast<daBkyRock_c*>(actor);
        if (action == RoomActorAction::DamageStage && rock->mMode == daBkyRock_c::MODE_0) {
            rock->initChangeModeBefore();
            rock->callBombEmt(4, kBkyFirstParticles);
            fopAcM_seStartCurrent(rock, Z2SE_OBJ_BOMB_ROCK_BRK_WTR_1, 0);
            rock->mMode = daBkyRock_c::MODE_1;
            rock->initChangeModeAfter();
            return true;
        }
        if (action != RoomActorAction::Break || rock->mMode == daBkyRock_c::MODE_2) return false;
        if (rock->mMode == daBkyRock_c::MODE_0) {
            apply_room_actor_action(actor, actorName, RoomActorAction::DamageStage, 0);
        }
        if (rock->mMode != daBkyRock_c::MODE_1) return false;
        rock->initChangeModeBefore();
        rock->callBombEmt(6, kBkySecondParticles);
        fopAcM_seStartCurrent(rock, Z2SE_OBJ_BOMB_ROCK_BRK_WTR_2, 0);
        rock->mMode = daBkyRock_c::MODE_2;
        fopAcM_onSwitch(rock, rock->getSwBit0());
        rock->initChangeModeAfter();
        return true;
    }
    case fpcNm_Obj_HBombkoya_e: {
        auto* house = static_cast<daObjHBombkoya_c*>(actor);
        if (action != RoomActorAction::Break) return false;
        // The house's own Execute observes this completion bit and performs
        // its normal final teardown on the following frame.
        fopAcM_onSwitch(house, house->getSw2No());
        return true;
    }
    case fpcNm_Obj_RfHole_e: {
        auto* roof = static_cast<daRfHole_c*>(actor);
        if (action != RoomActorAction::Break || roof->mMode != daRfHole_c::MODE_WAIT) return false;
        roof->init_modeBreak();
        return true;
    }
    case fpcNm_Obj_BmWindow_e: {
        auto* window = static_cast<daBmWindow_c*>(actor);
        if (action != RoomActorAction::Break || window->mMode != daBmWindow_c::WAIT) return false;
        window->init_modeBreak();
        return true;
    }
    case fpcNm_Obj_WellCover_e: {
        auto* cover = static_cast<daObjWCover_c*>(actor);
        if (action != RoomActorAction::Break || cover->field_0x5b0 != 0) return false;
        cover->init_modeBreak();
        return true;
    }
    case fpcNm_Obj_BBox_e: {
        auto* box = static_cast<daObjBBox_c*>(actor);
        if (action != RoomActorAction::Break ||
            dComIfGs_isSwitch(box->getSwNo(), fopAcM_GetRoomNo(box))) return false;
        static constexpr u16 particleIds[] = {0x83B0, 0x83B1, 0x83B2, 0x83B3, 0x83B4};
        for (u16 particleId : particleIds) {
            dComIfGp_particle_set(particleId, &box->current.pos, nullptr, &box->scale,
                                  0xff, nullptr, -1, nullptr, nullptr, nullptr);
        }
        fopAcM_seStart(box, Z2SE_OBJ_WOODBOX_BREAK, 0);
        fopAcM_onSwitch(box, box->getSwNo());
        fopAcM_delete(box);
        return true;
    }
    case fpcNm_Obj_Lv5FBoard_e: {
        auto* floor = static_cast<daFlorBoad_c*>(actor);
        if (action != RoomActorAction::Break || floor->mMode != daFlorBoad_c::MODE_WAIT) return false;
        floor->init_modeBreak();
        return true;
    }
    case fpcNm_Obj_Lv4DigSand_e: {
        auto* sand = static_cast<daObjL4DigSand_c*>(actor);
        if (action == RoomActorAction::DigStart && sand->mMode == 0) {
            sand->startDig();
            return true;
        }
        if (action != RoomActorAction::Break || sand->mMode == 2) return false;
        sand->mode_init_end();
        return true;
    }
    case fpcNm_Obj_Picture_e: {
        auto* picture = static_cast<daObjPicture_c*>(actor);
        if (action != RoomActorAction::Break || picture->field_0xd24 != 0 ||
            picture->field_0xd26 != 0) return false;
        fopAcM_onSwitch(picture, picture->getSW_0());
        mDoAud_seStart(Z2SE_OBJ_ROPE_PAINT_CUT, &picture->field_0xc88, 0, 0);
        picture->field_0xd24 = 1;
        picture->speed.set(0.0f, -1.0f, 0.0f);
        for (int i = 0; i < 10; ++i) {
            picture->field_0xd28[i].x = (i == 5 || i == 6) ? 12.0f
                                           : (i == 0 || i == 1) ? 4.0f : 7.0f;
            picture->field_0xd28[i].y = -1.0f;
            picture->field_0xd28[i].z = 0.0f;
        }
        return true;
    }
    case fpcNm_Obj_IceWall_e: {
        auto* wall = static_cast<daIceWall_c*>(actor);
        if (wall->mMode != daIceWall_c::MODE_WAIT) return false;
        if (action == RoomActorAction::PartialBreak) {
            if (wall->mIsBreaking != 0) return false;
            wall->mIsBreaking = 1;
            fopAcM_onSwitch(wall, wall->mIsBreakingSwBit);
            fopAcM_SetMtx(wall, wall->mpModel[1]->getBaseTRMtx());
            fopAcM_setCullSizeBox2(wall, wall->mpModel[1]->getModelData());
            return true;
        }
        if (action != RoomActorAction::Break) return false;
        wall->mIsBreaking = 1;
        wall->init_modeBreak();
        return true;
    }
    case fpcNm_Obj_Lv5SwIce_e: {
        auto* ice = static_cast<daLv5SwIce_c*>(actor);
        if (action != RoomActorAction::Break || ice->mMode != daLv5SwIce_c::MODE_WAIT) return false;
        ice->init_modeBreak();
        return true;
    }
    case fpcNm_Obj_Movebox_e: {
        auto* block = static_cast<daObjMovebox::Act_c*>(actor);
        const int direction = actionArgument & 0x3;
        const bool pull = (actionArgument & 0x4) != 0;
        const int duration = actionArgument >> 3;
        if (action != RoomActorAction::MoveStep || block->field_0x5ac != 0 ||
            direction < 0 || direction >= 4 || duration <= 0 || duration > 300) {
            return false;
        }
        block->field_0x8e8 = direction;
        block->mPPLabel = pull ? dBgW::PPLABEL_PULL : dBgW::PPLABEL_PUSH;
        block->eff_smoke_slip_start();
        block->mode_walk_init();
        block->field_0x8f8 = static_cast<s16>(duration);
        block->field_0x8e4 = 32768.0f / static_cast<f32>(duration);
        sRemoteMoveboxes.insert(block);
        return true;
    }
    case fpcNm_Obj_RotStair_e: {
        auto* stair = static_cast<daObjRotStair_c*>(actor);
        const int target = actionArgument;
        if (action != RoomActorAction::RotateTo || target < 0 || target >= 4 ||
            stair->mMode != daObjRotStair_c::MODE_WAIT) {
            return false;
        }
        static constexpr s16 kTargetAngles[] = {0x7FFF, 0x4000, 0x0000, -0x4000};
        const int baseSwitch = stair->getSwNo();
        for (int i = 0; i < 4; ++i) {
            if (i == target) {
                dComIfGs_onSwitch(baseSwitch + i, fopAcM_GetRoomNo(stair));
            } else {
                dComIfGs_offSwitch(baseSwitch + i, fopAcM_GetRoomNo(stair));
            }
            stair->mIsSw[i] = i == target;
        }
        stair->field_0x5e3 = static_cast<s8>(target);
        stair->mTargetAngle = kTargetAngles[target];
        stair->field_0x5e2 = false;
        stair->init_modeRotate();
        return true;
    }
    case fpcNm_Obj_IceBlock_e: {
        auto* block = static_cast<daObjIceBlk_c*>(actor);
        const int direction = actionArgument & 0x3;
        const int walkType = actionArgument >> 2;
        if (action != RoomActorAction::Slide ||
            block->mMode != daObjIceBlk_c::MODE_PROC_WAIT_e ||
            direction < 0 || direction >= 4 || walkType < 1 || walkType > 2) {
            return false;
        }
        static constexpr s16 kDirectionAngles[] = {0x0000, 0x4000, -0x8000, -0x4000};
        block->mMoveDir = direction;
        block->mWalkType = static_cast<u8>(walkType);
        block->current.angle.y = kDirectionAngles[direction];
        block->mode_init_walk();
        // The local push path uses this event to take over the camera. The
        // remote block keeps its native movement but must not control this
        // client's camera; restore ACTION_WAIT when the slide ends.
        block->setAction(daObjIceBlk_c::ACTION_DEAD_e);
        fopAcM_seStart(block, Z2SE_OBJ_IRONBLOCK_MOVE, 0);
        sRemoteIceblocks.insert(block);
        return true;
    }
    default:
        return false;
    }
}

void* apply_room_actor_action_judge(void* actor, void* data) {
    if (actor == nullptr || data == nullptr) return nullptr;
    const auto& search = *static_cast<const RoomActorSearch*>(data);
    auto* gameActor = static_cast<fopAc_ac_c*>(actor);
    if (fpcM_GetName(actor) != search.actorName ||
        fopAcM_GetRoomNo(gameActor) != search.room ||
        fopAcM_GetParam(actor) != search.params) {
        return nullptr;
    }
    return apply_room_actor_action(actor, search.actorName, search.action,
                                   search.actionArgument) ? actor : nullptr;
}

struct FaronCageSearch {
    int flag;
    bool repairBokoblin;
};

void* repair_faron_cage_actor(void* actor, void* data) {
    if (actor == nullptr || data == nullptr) return nullptr;
    const auto* search = static_cast<FaronCageSearch*>(data);
    if (search->repairBokoblin) {
        if (fopAcM_GetName(actor) != fpcNm_E_RD_e) return nullptr;
        const int actorFlag = (fopAcM_GetParam(actor) >> 24) & 0xFF;
        if (actorFlag != search->flag) return nullptr;
        auto* enemy = static_cast<fopAc_ac_c*>(actor);
        dComIfGs_onSwitch(actorFlag, fopAcM_GetRoomNo(enemy));
        fopAcM_createDisappear(enemy, &enemy->current.pos, 10, 0, 11);
        fopAcM_delete(enemy);
        return actor;
    }
    if (fopAcM_GetName(actor) != fpcNm_OBJ_SO_e) return nullptr;
    auto* cage = static_cast<obj_so_class*>(actor);
    const uint32_t params = fopAcM_GetParam(&cage->actor);
    const int highFlag = (params >> 24) & 0xFF;
    const int midFlag = (params >> 16) & 0xFF;
    if (highFlag != search->flag && midFlag != search->flag) return nullptr;
    cage->actor.health = 0;
    cage->field_0xdae = 1;
    cage->field_0xdb0 = 2;
    cage->field_0xdc8 = 0.0f;
    cage->field_0x1054 = 1;
    for (int i = 0; i < 8; ++i) {
        cage->field_0x1a98[i] = 2;
        cage->field_0x750[i + 2] = 0.0f;
    }
    dComIfGs_onSwitch(search->flag, fopAcM_GetRoomNo(&cage->actor));
    return actor;
}

void* repair_sewers_carry_box(void* actor, void*) {
    if (actor == nullptr || fopAcM_GetName(actor) != fpcNm_Obj_Carry_e) return nullptr;
    auto* box = static_cast<daObjCarry_c*>(actor);
    if (box->getType() != daObjCarry_c::TYPE_KIBAKO) return nullptr;
    box->obj_break(false, true, true);
    fopAcM_delete(box);
    return actor;
}

void* repair_faron_mist_gate(void* actor, void*) {
    constexpr int kGateFlag = 0x14;
    constexpr int kGateRoom = 5;
    if (actor == nullptr || fopAcM_GetName(actor) != fpcNm_Obj_KkrGate_e) return nullptr;
    auto* gate = static_cast<daObjKGate_c*>(actor);
    if (gate->getSwNo() != kGateFlag || fopAcM_GetRoomNo(gate) != kGateRoom) return nullptr;
    gate->setAction(2);
    return actor;
}

void repair_tbox_visual(int flag, bool animate = false) {
    auto* tbox = static_cast<daTbox_c*>(fopAcIt_Judge(judge_tbox, &flag));
    if (tbox == nullptr || tbox->eventInfo.checkCommandDemoAccrpt()) return;
    const auto id = fopAcM_GetID(tbox);
    if (sOpeningTboxes.contains(id)) return;
    if (tbox->mpAnm != nullptr) {
        if (animate && tbox->mpAnm->getFrame() < tbox->mpAnm->getEndFrame()) {
            tbox->mpAnm->setPlaySpeed(1.0f);
            sOpeningTboxes.insert(id);
        } else {
            tbox->mpAnm->setFrame(tbox->mpAnm->getEndFrame());
        }
    }
    tbox->setAction(&daTbox_c::actionWait);
    tbox->setDzb();
    dTres_c::offStatus(0, flag, 1);
}

bool repair_key_visual(int flag) {
    auto* key = static_cast<daKey_c*>(fopAcIt_Judge(judge_key, &flag));
    if (key == nullptr) return false;
    dTres_c::offStatus(flag, 1);
    fopAcM_delete(key);
    return true;
}

bool repair_drop_visual(int flag) {
    auto* drop = static_cast<daObjDrop_c*>(fopAcIt_Judge(judge_drop, &flag));
    if (drop == nullptr) return false;
    fopAcM_delete(drop);
    return true;
}

bool repair_life_visual(int globalBit) {
    auto* life = static_cast<daObjLife_c*>(fopAcIt_Judge(judge_life, &globalBit));
    if (life == nullptr) return false;
    fopAcM_delete(life);
    return true;
}

bool repair_item_visual(int flag) {
    auto* item = static_cast<daItem_c*>(fopAcIt_Judge(judge_item, &flag));
    if (item == nullptr) return false;
    fopAcM_delete(item);
    return true;
}

bool repair_sword_visual(int globalBit) {
    auto* sword = static_cast<daObjSword_c*>(fopAcIt_Judge(judge_sword, &globalBit));
    if (sword == nullptr) return false;
    fopAcM_delete(sword);
    return true;
}

int current_stage_table() {
    stage_stag_info_class* info = dComIfGp_getStageStagInfo();
    return info != nullptr ? dStage_stagInfo_GetSaveTbl(info) : -1;
}

}  // namespace

void repair_remote_tbox_collectible(int stage, int flag, bool newlySet, bool animate) {
    if (!newlySet || stage != current_stage_table()) return;
    repair_tbox_visual(flag, animate);
    if (!repair_key_visual(flag)) repair_drop_visual(flag);
}

void update_remote_tbox_animations(bool active) {
    if (!active) {
        sOpeningTboxes.clear();
        return;
    }
    if (dComIfGp_isPauseFlag()) return;
    for (auto it = sOpeningTboxes.begin(); it != sOpeningTboxes.end();) {
        auto* actor = fopAcM_SearchByID(*it);
        auto* tbox = actor != nullptr && fopAcM_GetName(actor) == fpcNm_TBOX_e
            ? static_cast<daTbox_c*>(actor) : nullptr;
        if (tbox == nullptr || tbox->mpAnm == nullptr ||
            tbox->eventInfo.checkCommandDemoAccrpt()) {
            it = sOpeningTboxes.erase(it);
        } else if (tbox->mpAnm->play()) {
            tbox->mpAnm->setFrame(tbox->mpAnm->getEndFrame());
            tbox->mpAnm->setPlaySpeed(0.0f);
            it = sOpeningTboxes.erase(it);
        } else {
            ++it;
        }
    }
}

void repair_remote_memory_item_collectible(int stage, int flag) {
    if (stage != current_stage_table()) return;
    const int globalBit = flag + dSv_info_c::MEMORY_ITEM;
    if (!repair_item_visual(globalBit) && !repair_life_visual(globalBit)) {
        repair_sword_visual(globalBit);
    }
}

bool repair_remote_switch_actors(int stage, int flag) {
    if (stage != current_stage_table()) return false;
    bool repaired = false;
    repaired = fopAcIt_Judge(repair_chain_block, &flag) != nullptr || repaired;
    repaired = fopAcIt_Judge(repair_jump_tbox, &flag) != nullptr || repaired;
    if (stage == 19 && flag == 0x26) {
        repaired = fopAcIt_Judge(repair_lv4_poe_gate, nullptr) != nullptr || repaired;
    }
    repaired = fopAcIt_Judge(repair_phantom_zant, &flag) != nullptr || repaired;
    repaired = fopAcIt_Judge(repair_sky_cannon, &flag) != nullptr || repaired;
    if (stage == dStage_SaveTbl_LV3 && flag >= 0 && flag <= 3 &&
        dComIfGp_roomControl_getStayNo() == 3) {
        repaired = fopAcIt_Judge(repair_lakebed_rot_stair, nullptr) != nullptr || repaired;
    }

    if (stage == dStage_SaveTbl_FARON) {
        constexpr int kBothBokoblins = 45;
        constexpr int kCageBroken = 46;
        constexpr int kLeftBokoblin = 47;
        constexpr int kRightBokoblin = 48;
        const auto repairBokoblin = [&](int bokoblinFlag) {
            FaronCageSearch search{bokoblinFlag, true};
            repaired = fopAcIt_Judge(repair_faron_cage_actor, &search) != nullptr || repaired;
        };
        if (flag == kLeftBokoblin || flag == kRightBokoblin) {
            repairBokoblin(flag);
        } else if (flag == kBothBokoblins) {
            repairBokoblin(kLeftBokoblin);
            repairBokoblin(kRightBokoblin);
        } else if (flag == kCageBroken) {
            FaronCageSearch search{flag, false};
            repaired = fopAcIt_Judge(repair_faron_cage_actor, &search) != nullptr || repaired;
        } else if (flag == 0x14) {
            constexpr int kGateRoom = 5;
            if (!dComIfGs_isSwitch(flag, kGateRoom)) {
                dComIfGs_onSwitch(flag, kGateRoom);
                repaired = true;
            }
            repaired = fopAcIt_Judge(repair_faron_mist_gate, nullptr) != nullptr || repaired;
        }
    } else if (stage == dStage_SaveTbl_PRISON) {
        if (flag == 10) {
            auto* box = static_cast<daObjBBox_c*>(
                fopAcIt_Judge(judge_sewers_breakable_box, &flag));
            if (box != nullptr) {
                static constexpr u16 particleIds[] = {0x83B0, 0x83B1, 0x83B2, 0x83B3,
                                                      0x83B4};
                for (u16 particleId : particleIds) {
                    dComIfGp_particle_set(particleId, &box->current.pos, nullptr, &box->scale,
                                          0xff, nullptr, -1, nullptr, nullptr, nullptr);
                }
                fopAcM_seStart(box, Z2SE_OBJ_WOODBOX_BREAK, 0);
                fopAcM_delete(box);
                repaired = true;
            }
        } else if (flag == 17) {
            repaired = fopAcIt_Judge(repair_sewers_carry_box, nullptr) != nullptr || repaired;
        }
    }
    return repaired;
}

int web_delete_timer(void* actor) {
    if (actor == nullptr || !fopAcM_IsActor(actor)) return -1;
    switch (fpcM_GetName(actor)) {
    case fpcNm_OBJ_WEB0_e:
        return static_cast<obj_web0_class*>(actor)->mDeleteTimer;
    case fpcNm_OBJ_WEB1_e:
        return static_cast<obj_web1_class*>(actor)->mDeleteTimer;
    default:
        return -1;
    }
}

bool apply_remote_web_timer(int actorName, int room, uint32_t params, int timer) {
    if ((actorName != fpcNm_OBJ_WEB0_e && actorName != fpcNm_OBJ_WEB1_e) ||
        room < 0 || room >= 64 || room != dComIfGp_roomControl_getStayNo()) {
        return false;
    }
    WebTimerSearch search{actorName, room, params, timer};
    return fopAcIt_Judge(start_web_timer, &search) != nullptr;
}

bool repair_remote_web_actor(int actorName, int room, int flag, uint32_t params) {
    if ((actorName != fpcNm_OBJ_WEB0_e && actorName != fpcNm_OBJ_WEB1_e) ||
        room < 0 || room >= 64 || room != dComIfGp_roomControl_getStayNo()) {
        return false;
    }
    WebSwitchSearch search{actorName, room, flag, params};
    return fopAcIt_Judge(repair_web_switch, &search) != nullptr;
}

int room_actor_action_state(void* actor) {
    if (actor == nullptr || !fopAcM_IsActor(actor)) return -1;
    switch (fpcM_GetName(actor)) {
    case fpcNm_BkyRock_e:
        return static_cast<daBkyRock_c*>(actor)->mMode;
    case fpcNm_Obj_Lv4DigSand_e:
        return static_cast<daObjL4DigSand_c*>(actor)->mMode;
    case fpcNm_Obj_Movebox_e:
        return static_cast<daObjMovebox::Act_c*>(actor)->field_0x5ac;
    case fpcNm_Obj_RotStair_e:
        return static_cast<daObjRotStair_c*>(actor)->mMode;
    case fpcNm_Obj_IceBlock_e:
        return static_cast<daObjIceBlk_c*>(actor)->mMode;
    default:
        return -1;
    }
}

int room_actor_action_argument(void* actor) {
    if (actor == nullptr || !fopAcM_IsActor(actor)) return -1;
    switch (fpcM_GetName(actor)) {
    case fpcNm_Obj_Movebox_e: {
        const auto* block = static_cast<const daObjMovebox::Act_c*>(actor);
        if (block->field_0x8e8 < 0 || block->field_0x8e8 >= 4 ||
            block->field_0x8f8 <= 0) return -1;
        const bool pull = cLib_checkBit<dBgW::PushPullLabel>(
            block->mPPLabel, dBgW::PPLABEL_PULL) != 0;
        return (block->field_0x8f8 << 3) | (pull ? 0x4 : 0) | block->field_0x8e8;
    }
    case fpcNm_Obj_RotStair_e:
        return static_cast<const daObjRotStair_c*>(actor)->field_0x5e3;
    case fpcNm_Obj_IceBlock_e: {
        const auto* block = static_cast<const daObjIceBlk_c*>(actor);
        if (block->mMoveDir < 0 || block->mMoveDir >= 4 ||
            block->mWalkType < 1 || block->mWalkType > 2) return -1;
        return (static_cast<int>(block->mWalkType) << 2) | block->mMoveDir;
    }
    default:
        return 0;
    }
}

RoomActorAction room_actor_switch_action(void* actor, bool wasSet) {
    if (actor != nullptr && fopAcM_IsActor(actor) &&
        fpcM_GetName(actor) == fpcNm_Obj_IceWall_e) {
        const auto* wall = static_cast<const daIceWall_c*>(actor);
        if (!wasSet && wall->mIsBreaking == 1) return RoomActorAction::PartialBreak;
    }
    return RoomActorAction::Break;
}

bool room_actor_persistent_switch(void* actor, int flag) {
    if (actor == nullptr || !fopAcM_IsActor(actor) || flag < 0 || flag >= 0xFF)
        return false;
    const int actorName = fpcM_GetName(actor);
    if (actorName == fpcNm_Obj_Movebox_e) {
        const auto* block = static_cast<const daObjMovebox::Act_c*>(actor);
        if (block->mpPath == nullptr || block->prmZ_get_pathId() == 0xFF) return false;
        return flag == block->prm_get_swSave1() || flag == block->prmZ_get_swSave2();
    }
    if (actorName == fpcNm_Obj_IceBlock_e) {
        const auto* block = static_cast<const daObjIceBlk_c*>(actor);
        if (block->mpPath == nullptr || block->mSwbit == 0xFF ||
            block->mMaxSwNum == 0 || block->mMaxSwNum > 8) return false;
        return flag >= block->mSwbit && flag < block->mSwbit + block->mMaxSwNum;
    }
    return room_actor_action_supported(actorName, RoomActorAction::Break) &&
           room_actor_switch_flag(actorName, fopAcM_GetParam(actor)) == flag;
}

bool apply_remote_room_actor_action(int actorName, int room, uint32_t params,
                                    RoomActorAction action, int actionArgument) {
    if (!room_actor_action_supported(actorName, action) || room < 0 || room >= 64 ||
        room != dComIfGp_roomControl_getStayNo()) {
        return false;
    }
    RoomActorSearch search{actorName, room, params, action, actionArgument};
    return fopAcIt_Judge(apply_room_actor_action_judge, &search) != nullptr;
}

bool remote_movebox_action_active(void* actor) {
    const auto it = sRemoteMoveboxes.find(actor);
    if (it == sRemoteMoveboxes.end()) return false;
    if (actor == nullptr || !fopAcM_IsActor(actor) ||
        fpcM_GetName(actor) != fpcNm_Obj_Movebox_e ||
        static_cast<daObjMovebox::Act_c*>(actor)->field_0x5ac != 1) {
        sRemoteMoveboxes.erase(it);
        return false;
    }
    return true;
}

bool remote_iceblock_action_active(void* actor) {
    return sRemoteIceblocks.contains(actor);
}

void finish_remote_iceblock_action(void* actor) {
    const auto it = sRemoteIceblocks.find(actor);
    if (it == sRemoteIceblocks.end()) return;
    if (actor == nullptr || !fopAcM_IsActor(actor) ||
        fpcM_GetName(actor) != fpcNm_Obj_IceBlock_e) {
        sRemoteIceblocks.erase(it);
        return;
    }
    auto* block = static_cast<daObjIceBlk_c*>(actor);
    if (block->mMode == daObjIceBlk_c::MODE_PROC_WAIT_e) {
        block->setAction(daObjIceBlk_c::ACTION_WAIT_e);
        sRemoteIceblocks.erase(it);
    }
}

bool apply_remote_room_actor_switch(int actorName, int room, int flag, uint32_t params,
                                    RoomActorAction fallbackAction) {
    if (room < 0 || room >= 64 || flag < 0 || flag >= 0xFF ||
        room != dComIfGp_roomControl_getStayNo() ||
        room_actor_switch_flag(actorName, params) != flag ||
        !room_actor_action_supported(actorName, fallbackAction)) {
        return false;
    }

    RoomActorSearch search{actorName, room, params, fallbackAction, 0};
    fopAcIt_Judge(apply_room_actor_action_judge, &search);
    dComIfGs_onSwitch(flag, room);
    return true;
}

void repair_current_stage_collectibles() {
    const int stage = current_stage_table();
    if (stage < 0) return;
    for (int flag = 0; flag < 64; ++flag) {
        if (dComIfGs_isStageTbox(stage, flag)) repair_tbox_visual(flag);
    }
    for (int flag = 0; flag < dSv_info_c::DAN_ITEM; ++flag) {
        // Current-stage memory is authoritative while an item cutscene is in
        // flight; Savedata can lag it until the stage/save commit.
        if (g_dComIfG_gameInfo.info.getMemory().getBit().isItem(flag)) {
            const int globalBit = flag + dSv_info_c::MEMORY_ITEM;
            if (!repair_item_visual(globalBit) && !repair_life_visual(globalBit)) {
                repair_sword_visual(globalBit);
            }
        }
    }
    constexpr int kFirstSewersBoxSwitch = 10;
    if (stage == dStage_SaveTbl_PRISON &&
        dComIfGs_isStageSwitch(stage, kFirstSewersBoxSwitch)) {
        repair_remote_switch_actors(stage, kFirstSewersBoxSwitch);
    }
}

void repair_remote_key_door_actor(void* process) {
    if (process == nullptr || !fopAcM_IsActor(process)) return;
    switch (fpcM_GetName(process)) {
    case fpcNm_DOOR20_e:
    {
        auto* door = static_cast<daDoor20_c*>(process);
        const int swBit = door_param2_c::getSwbit(door);
        if (!door->field_0x5f0 || door->field_0x5ec == fpcM_ERROR_PROCESS_ID_e ||
            swBit == 0xFF || !dComIfGs_isSwitch(swBit, 0xFFFFFFFF) ||
            (door_param2_c::getFrontOption(door) != 2 &&
             door_param2_c::getBackOption(door) != 2)) {
            return;
        }
        if (door->field_0x673 == 1) {
            auto* key = static_cast<daObjLv5Key_c*>(fopAcM_SearchByID(door->field_0x5ec));
            if (key == nullptr) return;
            key->keylock_open_start();
        } else {
            auto* keyhole = reinterpret_cast<obj_keyhole_class*>(
                fopAcM_SearchByID(door->field_0x5ec));
            if (keyhole == nullptr) return;
            keyhole->setOpen();
        }
        door->field_0x5f0 = false;
        return;
    }
    case fpcNm_L1MBOSS_DOOR_e:
    {
        auto* door = static_cast<daMBdoorL1_c*>(process);
        const u8 swBit = door_param2_c::getSwbit(door);
        if (door->mKeyHoleId == fpcM_ERROR_PROCESS_ID_e ||
            door_param2_c::getFrontOption(door) != 2 || swBit == 0xFF ||
            !fopAcM_isSwitch(door, swBit)) {
            return;
        }
        auto* keyhole = reinterpret_cast<obj_keyhole_class*>(
            fopAcM_SearchByID(door->mKeyHoleId));
        if (keyhole != nullptr && !keyhole->checkOpen() && !keyhole->checkOpenEnd()) {
            keyhole->setOpen();
        }
        return;
    }
    case fpcNm_Obj_Kshutter_e:
    {
        auto* door = static_cast<daObjKshtr_c*>(process);
        if (!door->mIsCheckKey || door->mKeyHoleId == fpcM_ERROR_PROCESS_ID_e ||
            door->mSwNo == 0xFF || !fopAcM_isSwitch(door, door->mSwNo)) {
            return;
        }
        auto* keyhole = reinterpret_cast<obj_keyhole_class*>(
            fopAcM_SearchByID(door->mKeyHoleId));
        if (keyhole != nullptr && !keyhole->checkOpen() && !keyhole->checkOpenEnd()) {
            keyhole->setOpen();
        }
        return;
    }
    default:
        return;
    }
}

}  // namespace dusklight_online::game
