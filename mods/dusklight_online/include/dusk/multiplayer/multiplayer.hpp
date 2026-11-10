#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class dCcD_GObjInf;
class fopAc_ac_c;

namespace dusk::multiplayer {

// Remote Midna streaming is disabled because its matrix stream is not sent.
inline constexpr bool kRemoteMidnaStreamingEnabled = false;
inline constexpr size_t kFishingRodLineSampleCount = 21;

struct RemoteModelMatrixSnapshot {
    bool valid = false;
    uint16_t jointCount = 0;
    uint16_t weightCount = 0;
    bool weightsOmitted = false;
    std::array<float, 12> base{};
    std::vector<float> joints;
    std::vector<float> weights;
};

struct RemoteLinkMatrixSnapshot {
    bool valid = false;
    RemoteModelMatrixSnapshot body;
    RemoteModelMatrixSnapshot hat;
    RemoteModelMatrixSnapshot face;
    RemoteModelMatrixSnapshot hand;
    RemoteModelMatrixSnapshot sword;
    RemoteModelMatrixSnapshot sheath;
    RemoteModelMatrixSnapshot shield;
    RemoteModelMatrixSnapshot heldItem;
    RemoteModelMatrixSnapshot hookTip;
    RemoteModelMatrixSnapshot hookSubItem;
    RemoteModelMatrixSnapshot hookSubTip;
    RemoteModelMatrixSnapshot arrow;
    RemoteModelMatrixSnapshot kantera;
    RemoteModelMatrixSnapshot kanteraGlow;
    RemoteModelMatrixSnapshot itemActor;
    RemoteModelMatrixSnapshot rideActor;
    RemoteModelMatrixSnapshot midna;
    RemoteModelMatrixSnapshot midnaMask;
    RemoteModelMatrixSnapshot midnaHand;
    RemoteModelMatrixSnapshot midnaHair;
    RemoteModelMatrixSnapshot midnaGlow;
    int midnaHairShape = 0;
};

struct RemoteAudioEvent {
    uint32_t sequence = 0;
    uint32_t soundId = 0;
    uint32_t mapInfo = 0;
    int8_t reverb = -1;
    uint8_t sourceKind = 0;
    bool level = false;
    bool tracked = false; // One start per sequence; active snapshot owns its lifetime.
};

enum RemoteObjectKind : uint8_t {
    REMOTE_OBJECT_NONE = 0,
    REMOTE_OBJECT_BOMB = 1,
};

enum RemoteItemActorKind : uint8_t {
    REMOTE_ITEM_ACTOR_NONE = 0,
    REMOTE_ITEM_ACTOR_BOOMERANG = 1,
    REMOTE_ITEM_ACTOR_BOMB_NORMAL = 2,
    REMOTE_ITEM_ACTOR_BOMB_WATER = 3,
    REMOTE_ITEM_ACTOR_BOMB_INSECT = 4,
};

enum RemoteRideActorKind : uint8_t {
    REMOTE_RIDE_ACTOR_NONE = 0,
    REMOTE_RIDE_ACTOR_SPINNER = 1,
};

struct RemoteObjectSnapshot {
    bool valid = false;
    std::string peerId;
    uint8_t objectKind = REMOTE_OBJECT_NONE;
    int32_t objectId = -1;
    uint32_t sequence = 0;
    uint32_t ageTicks = 0;
    std::string stage;
    int room = -1;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    int angleY = 0;
    int kind = 0;
    int exTime = -1;
    bool active = false;
    bool exploding = false;
};

using RemoteBombObjectSnapshot = RemoteObjectSnapshot;

enum RemoteAudioSourceKind : uint8_t {
    REMOTE_AUDIO_SOURCE_GENERIC = 0,
    REMOTE_AUDIO_SOURCE_LINK_SOUND = 1,
    REMOTE_AUDIO_SOURCE_LINK_SOUND_LEVEL = 2,
    REMOTE_AUDIO_SOURCE_LINK_VOICE = 3,
    REMOTE_AUDIO_SOURCE_LINK_VOICE_LEVEL = 4,
    REMOTE_AUDIO_SOURCE_LINK_SWORD = 5,
    REMOTE_AUDIO_SOURCE_LINK_COLLISION = 6,
    REMOTE_AUDIO_SOURCE_LINK_HIT_ITEM = 7,
};

struct PeerPoseSnapshot {
    bool valid = false;
    // Receiver-only clock; never serialized or used for network acknowledgement.
    uint32_t presentationSequence = 0;
    std::string peerId;
    uint32_t sequence = 0;
    uint32_t ageTicks = 0;
    // Sender-selected presentation intent. Older packets decode as Unknown and
    // retain the receiver's conservative semantic-support checks.
    enum class VisualMode : uint8_t {
        Unknown = 0,
        SemanticGameplay = 1,
        HiddenUnsupported = 2,
    };
    VisualMode visualMode = VisualMode::Unknown;
    uint32_t visualUnsupportedReasons = 0;
    std::string stage;
    int room = -1;
    int layer = -1;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    int angleY = 0;
    bool finalGanondorfReady = false;
    int procId = 0;
    int procVar0 = 0;
    int procVar1 = 0;
    int procVar2 = 0;
    int procVar3 = 0;
    int procVar5 = 0;
    int cutType = 0;
    int cutCount = 0;
    bool jumpCancelTurn = false;
    bool manualSyncReady = false;
    float underFrame = 0.0f;
    int underBck0 = 0;
    int underBckArc0 = 0xFFFF;
    float underFrame0 = 0.0f;
    float underRate0 = 1.0f;
    float underRatio0 = 1.0f;
    int underBck1 = 0;
    int underBckArc1 = 0xFFFF;
    float underFrame1 = 0.0f;
    float underRate1 = 1.0f;
    float underRatio1 = 0.0f;
    int underBck2 = 0;
    int underBckArc2 = 0xFFFF;
    float underFrame2 = 0.0f;
    float underRate2 = 1.0f;
    float underRatio2 = 0.0f;
    int upperBck0 = 0;
    int upperBckArc0 = 0xFFFF;
    float upperFrame0 = 0.0f;
    float upperRate0 = 1.0f;
    float upperRatio0 = 1.0f;
    int upperBck1 = 0;
    int upperBckArc1 = 0xFFFF;
    float upperFrame1 = 0.0f;
    float upperRate1 = 1.0f;
    float upperRatio1 = 0.0f;
    int upperBck2 = 0;
    int upperBckArc2 = 0xFFFF;
    float upperFrame2 = 0.0f;
    float upperRate2 = 1.0f;
    float upperRatio2 = 0.0f;
    int faceBck = 0;
    int faceBckArc = 0xFFFF;
    float faceBckFrame = 0.0f;
    int faceBtp = 0;
    int faceBtpArc = 0xFFFF;
    float faceBtpFrame = 0.0f;
    int faceBtk = 0;
    int faceBtkArc = 0xFFFF;
    float faceBtkFrame = 0.0f;
    std::array<int16_t, 10> hatRotA{};
    std::array<int16_t, 10> hatRotB{};
    std::array<int16_t, 3> hatSwing{};
    int hatShapeY = 0;
    int shapeAngleX = 0;
    int shapeAngleZ = 0;
    int bodyAngleX = 0;
    int bodyAngleY = 0;
    int bodyAngleZ = 0;
    int bodyTwistY = 0;
    int neckJointX = 0;
    int neckJointY = 0;
    int neckJointZ = 0;
    int lowerJointX = 0;
    int lowerJointZ = 0;
    int rootJointX = 0;
    int rootJointZ = 0;
    int blendMode = 0;
    float upperSavedRatio = 0.0f;
    bool bodyRootValid = false;
    float bodyRootX = 0.0f;
    float bodyRootY = 0.0f;
    float bodyRootZ = 0.0f;
    std::array<int16_t, 6> legIkAngles{};
    std::array<int16_t, 6> armIkAngles{};
    std::array<int16_t, 6> armRotA{};
    std::array<int16_t, 6> armRotB{};
    std::array<int16_t, 3> fishingArm1Angle{};
    std::array<int16_t, 3> fishingArm2Angle{};
    // Upper/lower anchors: pose sequence, native timer, initial fade.
    std::array<int64_t, 6> waterDropState{};
    bool waterDropStrong = false;
    uint16_t powerUpTimer = 0;
    float powerUpIntensity = 0.0f;
    bool isWolf = false;
    bool isTransforming = false;
    bool transformFromWolf = false;
    bool transformToWolf = false;
    int transformProcVar0 = 0;
    int transformProcVar5 = 0;
    int transformClothesWait = 0;
    float transformFrame = 0.0f;
    int transformProcVar2 = 0;
    int transformProcVar3 = 0;
    int transformShapeX = 0;
    uint16_t equipItem = 0xFFFF;
    uint16_t presentedItem = 0xFFFF;
    int swordVariant = 0;
    int shieldVariant = 0;
    int clothesVariant = 0;
    bool swordDraw = false;
    bool shieldDraw = false;
    bool shieldGuardActive = false;
    bool swordHandAttached = false;
    bool shieldHandAttached = false;
    int leftHandShape = -1;
    int rightHandShape = -1;
    bool swordOut = false;
    bool midnaDraw = false;
    bool midnaMaskDraw = false;
    bool midnaHandDraw = false;
    bool midnaHairDraw = false;
    bool midnaShadowForm = false;
    bool heavyBoots = false;
    bool zoraMaskDraw = false;
    bool magicArmorPowered = true;
    bool itemDraw = false;
    bool kanteraDraw = false;
    int itemActorKind = 0;
    bool boomerangVisualValid = false;
    bool boomerangLinkAnchored = false;
    float boomerangX = 0.0f;
    float boomerangY = 0.0f;
    float boomerangZ = 0.0f;
    int boomerangAngleX = 0;
    int boomerangAngleY = 0;
    int boomerangAngleZ = 0;
    int itemActorBombExTime = -1;
    int itemActorBombFlash = -1;
    bool ironBallVisualValid = false;
    bool ironBallLinkAnchored = false;
    float ironBallX = 0.0f;
    float ironBallY = 0.0f;
    float ironBallZ = 0.0f;
    int ironBallAngleX = 0;
    int ironBallAngleY = 0;
    int ironBallAngleZ = 0;
    uint8_t ironBallChainCount = 0;
    std::vector<uint8_t> ironBallChainDirections;
    bool ironBallChainEndOffsetValid = false;
    float ironBallChainEndOffsetX = 0.0f;
    float ironBallChainEndOffsetY = 0.0f;
    float ironBallChainEndOffsetZ = 0.0f;
    bool hookshotVisualValid = false;
    bool hookshotLeft = true;
    int hookshotArmAimX = 0;
    int hookshotArmAimY = 0;
    bool hookshotTopLinkAnchored = false;
    bool hookshotSubTopLinkAnchored = false;
    float hookshotTopX = 0.0f;
    float hookshotTopY = 0.0f;
    float hookshotTopZ = 0.0f;
    int hookshotTopAngleX = 0;
    int hookshotTopAngleY = 0;
    int hookshotTopAngleZ = 0;
    float hookshotSubTopX = 0.0f;
    float hookshotSubTopY = 0.0f;
    float hookshotSubTopZ = 0.0f;
    int hookshotSubTopAngleX = 0;
    int hookshotSubTopAngleY = 0;
    int hookshotSubTopAngleZ = 0;
    int hookshotStopTime = 0;
    float hookshotItemFrame = 0.0f;
    float hookshotTipFrame = 0.0f;
    float hookshotSubTipFrame = 0.0f;
    bool copyRodVisualValid = false;
    bool copyRodTopUse = false;
    bool fishingRodVisualValid = false;
    bool fishingRodLineValid = false;
    std::array<float, 45> fishingRodSegmentDeltas{};
    std::array<float, kFishingRodLineSampleCount * 3> fishingRodLineOffsets{};
    bool fishingRodEndValid = false;
    uint8_t fishingRodAction = 0;
    uint8_t fishingRodHookKind = 0;
    uint8_t fishingRodBaitKind = 0;
    int fishingRodEndRoll = 0;
    std::array<float, 3> fishingRodBobberOffset{};
    std::array<int16_t, 6> fishingRodBobberAngles{};
    int fishingRodCounter = 0;
    bool bowVisualValid = false;
    bool bowGrabLeft = false;
    int bowBck = 0;
    float bowFrame = 0.0f;
    bool bowArrowVisible = false;
    bool bowArrowBomb = false;
    bool lanternVisualValid = false;
    bool lanternLinkAnchored = false;
    bool lanternHandAttached = false;
    bool lanternLit = false;
    float lanternX = 0.0f;
    float lanternY = 0.0f;
    float lanternZ = 0.0f;
    int lanternBaseAngleX = 0;
    int lanternBaseAngleY = 0;
    int lanternBaseAngleZ = 0;
    int lanternJointAngleX = 0;
    int lanternJointAngleY = 0;
    int lanternJointAngleZ = 0;
    bool bottleVisualValid = false;
    bool bottleOilRightAttached = false;
    bool bottleJointRightAttached = false;
    bool bottleDrinkMaterialSet = false;
    int bottleMaterialStage = 0;
    float bottleBrkFrame = 0.0f;
    float bottleBtpFrame = 0.0f;
    float bottleBtkSwingFrame = 0.0f;
    float bottleBtkActionFrame = 0.0f;
    float bottleBtkFinishFrame = 0.0f;
    int bottleContentKind = 0;
    float bottleContentFrame = 0.0f;
    int rideActorKind = 0;
    bool spinnerVisualValid = false;
    bool spinnerLinkAnchored = false;
    float spinnerX = 0.0f;
    float spinnerY = 0.0f;
    float spinnerZ = 0.0f;
    int spinnerShapeX = 0;
    int spinnerShapeY = 0;
    int spinnerShapeZ = 0;
    int spinnerRotY = 0;
    float spinnerVisualYOffset = 90.0f;
    uint32_t spinnerJumpEpoch = 0;
    RemoteLinkMatrixSnapshot linkMatrices;
    bool linkMatricesFresh = false;
    std::vector<RemoteAudioEvent> audioEvents;
    std::vector<RemoteAudioEvent> activeAudioEvents;
};

using RemotePvpHitCallback = void (*)(fopAc_ac_c*, fopAc_ac_c*, dCcD_GObjInf*);
using RemoteBombActorCallback = void (*)(int32_t);

void configure_remote_actor_bridge(bool pvpEnabled, RemotePvpHitCallback pvpHit,
                                   RemoteBombActorCallback bombCreated);
void set_remote_pvp_hit_callback(RemotePvpHitCallback callback);
void set_remote_bomb_actor_callback(RemoteBombActorCallback callback);
void set_remote_actor_options(bool displayMidna, bool syncWorld,
                              bool remoteCollision, bool pvpEnabled,
                              bool semanticRenderingEnabled);
void set_remote_bomb_object(const RemoteBombObjectSnapshot& object);
void erase_remote_actor_peer(const std::string& peerId);
void reset_remote_actor_bridge();
bool pvp_enabled();
bool display_remote_midna_enabled();
bool remote_collision_enabled();
bool semantic_rendering_enabled();
bool get_remote_bomb_object_for_peer(const std::string& peerId,
                                     RemoteBombObjectSnapshot* out);
void report_remote_link_pvp_target_hit(fopAc_ac_c* remoteLinkActor,
                                       fopAc_ac_c* attackActor,
                                       dCcD_GObjInf* attackInfo);
void register_remote_bomb_actor_id(int32_t actorId);

}  // namespace dusk::multiplayer
