#ifndef D_A_REMOTE_LINK_H
#define D_A_REMOTE_LINK_H

#include "SSystem/SComponent/c_phase.h"
#include "d/d_resorce.h"
#include "d/d_cc_d.h"
#include "d/d_kankyo.h"
#include "dusk/multiplayer/multiplayer.hpp"
#include "dusklight_online/game/audio_lifetime.hpp"
#include "dusklight_online/game/wet_state.hpp"
#include "f_op/f_op_actor_mng.h"
#include "m_Do/m_Do_ext.h"
#include "Z2AudioLib/Z2SoundObject.h"

class mDoExt_bckAnm;
class JKRExpHeap;
class JKRMemArchive;
class J3DAnmTransform;
class J3DAnmTevRegKey;
class J3DAnmTextureSRTKey;
class J3DAnmTexPattern;
class J3DShape;

/**
 * Remote Link visual owner.
 *
 * This actor is intentionally not daAlink_c and does not participate in player
 * state. It owns the human Link visual model set and submits it through the
 * normal actor model lifecycle.
 */
class daRemoteLink_c : public fopAc_ac_c {
public:
    daRemoteLink_c();

    cPhs_Step create();
    int CreateHeap();
    int Execute();
    int Draw();
    int Delete();

    void setRemotePose(const cXyz& i_pos, s16 i_angleY, s8 i_roomNo);
    void setRemoteActionState(int i_procId, int i_procVar0, int i_procVar1, int i_procVar2,
                              int i_procVar3, int i_procVar5, f32 i_underFrame,
                              u16 i_underBck0, f32 i_underFrame0, f32 i_underRate0,
                              f32 i_underRatio0, u16 i_underBck1, f32 i_underFrame1,
                              f32 i_underRate1, f32 i_underRatio1, u16 i_underBck2,
                              f32 i_underFrame2, f32 i_underRate2, f32 i_underRatio2,
                              u16 i_upperBck0, f32 i_upperFrame0, f32 i_upperRate0,
                              f32 i_upperRatio0, u16 i_upperBck1, f32 i_upperFrame1,
                              f32 i_upperRate1, f32 i_upperRatio1, u16 i_upperBck2,
                              f32 i_upperFrame2, f32 i_upperRate2, f32 i_upperRatio2,
                              u16 i_equipItem, int i_swordVariant, int i_shieldVariant,
                              bool i_swordDraw, bool i_shieldDraw, bool i_shieldGuardActive,
                              bool i_swordHandAttached, bool i_shieldHandAttached,
                              int i_leftHandShape, int i_rightHandShape, bool i_swordOut,
                              bool i_heavyBoots, bool i_zoraMaskDraw,
                              bool i_magicArmorPowered, bool i_itemDraw, bool i_kanteraDraw,
                              bool i_midnaDraw, bool i_midnaMaskDraw, bool i_midnaHandDraw,
                              bool i_midnaHairDraw, bool i_midnaShadowForm, int i_itemActorKind,
                              int i_itemActorBombExTime, int i_itemActorBombFlash, int i_rideActorKind);
    void setRemoteAnimationSourceState(u16 i_underArc0, u16 i_underArc1, u16 i_underArc2,
                                       u16 i_upperArc0, u16 i_upperArc1, u16 i_upperArc2);
    void setRemoteFaceState(u16 i_bck, u16 i_bckArc, f32 i_bckFrame,
                            u16 i_btp, u16 i_btpArc, f32 i_btpFrame,
                            u16 i_btk, u16 i_btkArc, f32 i_btkFrame);
    void setRemoteMatrices(const dusk::multiplayer::RemoteLinkMatrixSnapshot& i_matrices);
    void setRemoteAttachmentMatrices(
        const dusk::multiplayer::RemoteLinkMatrixSnapshot& i_matrices);
    void setRemoteSpinnerVisualState(bool i_valid, bool i_linkAnchored,
                                     const cXyz& i_pos,
                                     s16 i_shapeX, s16 i_shapeY, s16 i_shapeZ,
                                     s16 i_rotY, f32 i_visualYOffset,
                                     u32 i_jumpEpoch);
    void setRemoteIronBallVisualState(bool i_valid, bool i_linkAnchored, const cXyz& i_pos,
                                      s16 i_angleX, s16 i_angleY, s16 i_angleZ,
                                      uint8_t i_chainCount,
                                      const std::array<cXyz, 100>& i_chainDirections,
                                      bool i_chainEndOffsetValid,
                                      const cXyz& i_chainEndOffset);
    void setRemoteHookshotVisualState(bool i_valid, bool i_left,
                                      s16 i_armAimX, s16 i_armAimY,
                                      bool i_topLinkAnchored,
                                      bool i_subTopLinkAnchored,
                                      const cXyz& i_top, const csXyz& i_topAngle,
                                      const cXyz& i_subTop, const csXyz& i_subTopAngle,
                                      s16 i_stopTime, f32 i_itemFrame,
                                      f32 i_tipFrame, f32 i_subTipFrame);
    void setRemoteBoomerangVisualState(bool i_valid, bool i_linkAnchored,
                                       const cXyz& i_pos, const csXyz& i_angle);
    void setRemoteCopyRodVisualState(bool i_valid, bool i_topUse);
    void setRemoteFishingRodVisualState(
        bool i_valid, bool i_lineValid, bool i_endValid,
        const std::array<cXyz, 15>& i_segmentDeltas,
        const std::array<cXyz,
            dusk::multiplayer::kFishingRodLineSampleCount>& i_lineOffsets,
        u8 i_action, u8 i_hookKind, u8 i_baitKind, s16 i_endRoll,
        const cXyz& i_bobberOffset,
        const std::array<s16, 6>& i_bobberAngles, s16 i_counter);
    void setRemoteBowVisualState(bool i_valid, bool i_grabLeft, u16 i_bck,
                                 f32 i_frame, bool i_arrowVisible,
                                 bool i_arrowBomb);
    void setRemoteLanternVisualState(bool i_valid, bool i_linkAnchored,
                                     bool i_handAttached, bool i_lit,
                                     const cXyz& i_pos,
                                     const csXyz& i_baseAngle,
                                     const csXyz& i_jointAngle);
    void setRemoteBottleVisualState(bool i_valid, bool i_oilRightAttached,
                                    bool i_jointRightAttached,
                                    bool i_drinkMaterialSet, int i_materialStage,
                                    f32 i_brkFrame, f32 i_btpFrame,
                                    f32 i_btkSwingFrame, f32 i_btkActionFrame,
                                    f32 i_btkFinishFrame, int i_contentKind,
                                    f32 i_contentFrame);
    void setRemotePresentedItem(u16 i_itemNo);
    void setRemotePresentationVisible(bool i_visible);
    bool isRemotePresentationVisible() const { return mRemotePresentationVisible; }
    void setRemoteTransformBridgeState(bool i_visible, bool i_senderWolf,
                                       const cXyz& i_pos, s16 i_shapeX,
                                       s16 i_shapeY, s16 i_shapeZ,
                                       int i_transformColor);
    void setRemoteTransformPresentationState(bool i_active, bool i_toWolf,
                                             bool i_senderWolf, int i_procVar1,
                                             int i_procVar3, int i_procVar5,
                                             f32 i_frame, const cXyz& i_pos);
    void setRemoteWaterDropState(const std::array<int64_t, 6>& state, uint32_t sequence, bool strong);
    void setRemotePowerUpState(u16 i_timer, f32 i_intensity);
    void handoffRemoteTransformEffectsTo(daRemoteLink_c* i_target);
    void setRemoteBombObjectState(const dusk::multiplayer::RemoteBombObjectSnapshot& i_bomb);
    void setRemoteHatState(const std::array<int16_t, 10>& i_rotA,
                           const std::array<int16_t, 10>& i_rotB,
                           const std::array<int16_t, 3>& i_swing, s16 i_shapeY);
    void setRemoteBodyState(s16 i_shapeX, s16 i_shapeZ, s16 i_bodyX, s16 i_bodyY,
                            s16 i_bodyZ, s16 i_twistY, s16 i_neckJointX,
                            s16 i_neckJointY, s16 i_neckJointZ, s16 i_lowerJointX,
                            s16 i_lowerJointZ, s16 i_rootJointX, s16 i_rootJointZ,
                            u8 i_blendMode, f32 i_upperSavedRatio, bool i_bodyRootValid,
                            const cXyz& i_bodyRoot,
                            const std::array<int16_t, 6>& i_legIkAngles,
                            const std::array<int16_t, 6>& i_armIkAngles,
                            const std::array<int16_t, 6>& i_armRotA,
                            const std::array<int16_t, 6>& i_armRotB,
                            const std::array<int16_t, 3>& i_fishingArm1Angle,
                            const std::array<int16_t, 3>& i_fishingArm2Angle);
    void applyRemoteBodyMatrixInterpolationForPresentation();
    bool getNameLabelPosition(cXyz* o_pos) const;
    void playRemoteSound(const dusk::multiplayer::RemoteAudioEvent& i_event);
    void syncRemoteActiveSounds(const std::vector<dusk::multiplayer::RemoteAudioEvent>& i_events);
    int headModelCallBack(int i_jointNo);
    int bodyModelCallBack(int i_jointNo);

    static int createHeapCallBack(fopAc_ac_c* i_this);
    static bool canReserveSlot();

private:
    class IronBallChainPacket : public J3DPacket {
    public:
        IronBallChainPacket() : mpOwner(NULL) {}
        void setOwner(daRemoteLink_c* i_owner) { mpOwner = i_owner; }
        virtual void draw();

    private:
        daRemoteLink_c* mpOwner;
    };

    class HookshotChainPacket : public J3DPacket {
    public:
        HookshotChainPacket() : mpOwner(NULL) {}
        void setOwner(daRemoteLink_c* i_owner) { mpOwner = i_owner; }
        virtual void draw();

    private:
        daRemoteLink_c* mpOwner;
    };

    enum VisualForm {
        FORM_HUMAN_KOKIRI,
        FORM_WOLF,
    };

    struct VisualState {
        VisualForm form;
    };

    struct BckCacheEntry {
        u16 resId;
        u16 arcNo;
        u8* buffer;
        J3DAnmTransform* bck;
    };

    struct OwnedResourceCacheEntry {
        s32 index;
        void* resource;
    };

    struct AramResourceCacheEntry {
        u16 resId;
        void* resource;
    };

    struct RemoteModelMatrixInterpState {
        dusk::multiplayer::RemoteModelMatrixSnapshot prev;
        dusk::multiplayer::RemoteModelMatrixSnapshot curr;
        bool prevValid;
        bool currValid;
    };

    struct HeldItemVisualDesc {
        u16 itemNo;
        u16 bmdResId;
        u32 bmdBufferSize;
        u32 modelFlags;
        u32 diffFlags;
        u16 brkResId;
    };

    struct ActiveRemoteSound {
        dusk::multiplayer::RemoteAudioEvent event;
        u8 ageTicks;
        bool active;
    };

    struct OwnedArchiveSlot {
        const char* arcName;
        JKRMemArchive* archive;
        s32 entry;
        bool mounted;
        OwnedResourceCacheEntry cache[16];
    };

    void setOriginalHeap(JKRExpHeap** i_ppheap, u32 i_size);
    J3DModel* initModel(J3DModelData* i_modelData, u32 i_mdlFlags, u32 i_diffFlags);
    J3DModel* initModel(J3DModelData* i_modelData, u32 i_diffFlags);
    void setupHumanKokiriModel();
    void setupWolfModel();
    void setupShadowMidnaModels();
    bool setupMagicArmorBrk();
    void setupEquipmentModels();
    void destroyEquipmentModels();
    const HeldItemVisualDesc* getHeldItemVisualDesc(u16 i_itemNo) const;
    void* loadAramResource(u16 i_resId, u32 i_bufSize, bool i_isModel);
    J3DModelData* loadAramBmd(u16 i_resId, u32 i_bufSize);
    J3DAnmTevRegKey* loadAramItemBrk(u16 i_resId, J3DModel* i_model);
    J3DAnmTextureSRTKey* loadAramItemBtk(u16 i_resId, J3DModel* i_model);
    J3DAnmTexPattern* loadAramItemBtp(u16 i_resId, J3DModel* i_model);
    void setupHeldItemModel();
    void setupFishingRodModels();
    void drawIronBallChain();
    void drawHookshotChains();
    void clearHeldItemExtras();
    void setupLinkedItemModels();
    void setupSwordMaterialAnm(J3DModel* i_model, int i_swordVariant);
    void applySwordShapeVisibility();
    void setupMotionAnimation();
    bool setupBlendAnimation();
    bool configureBlendSlot(int i_slot, u16 i_bck, u16 i_arcNo, f32 i_frame,
                            f32 i_rate, f32 i_ratio);
    void applyRemoteChangeBlendRate(int i_jointNo);
    void applyRemoteFootMatrix();
    void applyRemoteArmMatrix();
    J3DAnmTransform* loadMotionBck(u16 i_resId, u8** o_buffer);
    void* loadFaceAnimation(u16 i_resId, u16 i_arcNo, u8** o_buffer);
    void updateFaceAnimation();
    void releaseBckCacheEntry(BckCacheEntry& i_entry);
    J3DAnmTransform* getMotionBck(u16 i_resId);
    J3DAnmTransform* getBlendSlotBck(int i_slot, u16 i_resId, u16 i_arcNo);
    u16 selectActionBck(f32* o_speed);
    void updateMotionAnimation();
    bool setMotionBck(u16 i_resId, f32 i_speed);
    bool reserveSlot();
    void releaseSlot();
    bool mountOwnedArchive();
    bool mountArchiveSlot(OwnedArchiveSlot& i_slot);
    void initArchiveSlot(OwnedArchiveSlot& i_slot, const char* i_arcName);
    void deleteArchiveSlot(OwnedArchiveSlot& i_slot);
    const char* getCurrentArcName() const;
    const char* getBodyResName() const;
    const char* getHeadResName() const;
    const char* getFaceResName() const;
    const char* getHandResName() const;
    u32 getOwnedArchiveNodeType(u32 i_fileIndex) const;
    u32 getArchiveNodeType(JKRMemArchive* i_archive, u32 i_fileIndex) const;
    void* convertOwnedObjectRes(s32 i_index);
    void* convertArchiveObjectRes(OwnedArchiveSlot& i_slot, s32 i_index);
    void* getOwnedObjectRes(const char* i_resName);
    void* getArchiveObjectRes(OwnedArchiveSlot& i_slot, const char* i_resName);
    void* getArchiveObjectRes(OwnedArchiveSlot& i_slot, s32 i_index);
    bool copyRemoteModelMatrices(J3DModel* i_model,
                                 const dusk::multiplayer::RemoteModelMatrixSnapshot& i_source);
    void applyRemoteAttachmentMatrices();
    void applyHumanHandMatrices(bool i_presentation);
    void applyHumanEquipmentMatrices(bool i_presentation);
    void updateRemoteIronBallVisual(bool i_presentation);
    void updateRemoteHookshotVisual(bool i_presentation);
    void updateRemoteBoomerangVisual(bool i_presentation);
    void updateRemoteCopyRodVisual(bool i_presentation);
    void updateRemoteFishingRodVisual(bool i_presentation);
    void updateRemoteBowVisual(bool i_presentation);
    void updateRemoteLanternVisual(bool i_presentation);
    void updateRemoteSpinnerVisual(bool i_presentation);
    void updateRemoteSimpleHeldItemVisual(bool i_presentation);
    void updateRemoteLanternFlame(bool i_presentation, const cXyz& i_flamePos);
    void stopRemoteLanternFlame(bool i_release);
    void updateRemoteBottleVisual(bool i_presentation);
    void setupPresentedItemModel();
    void updateRemotePresentedItemVisual(bool i_presentation);
    void captureRemoteModelMatrixSnapshot(
        J3DModel* i_model, const dusk::multiplayer::RemoteModelMatrixSnapshot& i_source,
        RemoteModelMatrixInterpState& io_state);
    void clearRemoteModelMatrixInterpolation(RemoteModelMatrixInterpState& io_state);
    bool applyInterpolatedRemoteModelMatrices(
        J3DModel* i_model, RemoteModelMatrixInterpState& io_state, const char* i_label);
    void overrideRemoteModelMatrices(J3DModel* i_model);
    void applyInterpolatedRemoteAttachments();
    void captureRemoteBodyMatrixSnapshot(
        const dusk::multiplayer::RemoteModelMatrixSnapshot& i_source);
    void clearRemoteBodyMatrixInterpolation();
    f32 getRemoteMatrixInterpolationStep() const;
    void applyInterpolatedRemoteBodyMatrices();
    void setBaseMtx();
    void calcModels();
    void drawModel(J3DModel* i_model);
    void applyRemoteTransformMaterialColor(bool i_active);
    void updateKanteraGlowOcclusion();
    void resetKanteraGlowOcclusion();
    void drawLinkedItemActorModel();
    void drawRemoteTransformBridge();
    void drawRemoteTransformEffectModel();
    void updateRemoteTransformEffectModel(bool i_presentation);
    void updateRemoteTransformParticles();
    u32 setRemoteTransformEmitter(int i_slot, u16 i_effectId,
                                  const cXyz& i_pos);
    void drawShadowMidnaModels();
    void updateRemoteBombActor();
    void stopRemoteBombActor(bool i_explode);
    void maybeSpawnRemoteBombExplosion(int i_nextItemActorKind);
    void spawnRemoteBombExplosion();
    void initPvpTargetCollision();
    void updatePvpTargetCollision();
    void updatePvpAttentionTarget();
    void updatePvpMidnaBindEffect();
    void stopRemoteActiveSounds();
    void hideAllHandShapes();
    void setupDrawHands();
    void setupHeavyBootModels();
    void applyHeavyBootMatrices();
    void applyWolfEquipmentMatrices(bool i_presentation = false);
    void alignSemanticBodyRootToRemoteRoot();
    void alignSemanticBodyRootForPresentation();

    /* 0x568 */ request_of_phase_process_class mPhase;
    /* 0x570 */ JKRExpHeap* mpArcHeap;
    /* 0x574 */ JKRMemArchive* mpOwnedArchive;
    /* 0x578 */ OwnedResourceCacheEntry mOwnedResourceCache[16];
    /* 0x5F8 */ s32 mOwnedArchiveEntry;
    /* 0x5FC */ bool mOwnedArchiveMounted;
    /* 0x600 */ OwnedArchiveSlot mEquipmentArchives[5];
    OwnedArchiveSlot mPresentedItemArchive;
    /* 0x920 */ VisualState mVisualState;
    /* 0x924 */ void* mpWarpTexData;
    /* 0x928 */ J3DModel* mpBodyModel;
    /* 0x92C */ J3DModel* mpHeadModel;
    /* 0x930 */ J3DModel* mpHandModel;
    /* 0x934 */ J3DModel* mpFaceModel;
    /* 0x938 */ J3DModel* mpSwordModel;
    /* 0x93C */ J3DModel* mpSheathModel;
    /* 0x940 */ J3DModel* mpShieldModel;
    /* 0x944 */ J3DModel* mpHeldItemModel;
    J3DModel* mpFishingRodModels[15];
    J3DModel* mpFishingRodFloatModels[2];
    J3DModel* mpFishingRodHookModels[2];
    J3DModel* mpFishingRodBaitModels[2];
    /* 0x948 */ J3DModel* mpHookTipModel;
    /* 0x94C */ J3DModel* mpHookSubItemModel;
    /* 0x950 */ J3DModel* mpHookSubTipModel;
    /* 0x954 */ J3DModel* mpArrowModel;
    /* 0x958 */ J3DModel* mpKanteraModel;
    /* 0x95C */ J3DModel* mpKanteraGlowModel;
    /* 0x960 */ J3DModel* mpItemActorModel;
    /* 0x964 */ J3DModel* mpRideActorModel;
    J3DModel* mpPresentedItemModel;
    J3DModel* mpTransformBridgeModel;
    J3DModel* mpTransformEffectModel;
    mDoExt_bckAnm* mpSpinnerBck;
    mDoExt_bckAnm* mpHookshotItemBck;
    mDoExt_bckAnm* mpHookshotTipBck;
    mDoExt_bckAnm* mpBowBck;
    mDoExt_bckAnm* mpBottleContentBck;
    bool mBottleContentBckInitialized;
    mDoExt_bckAnm* mpTransformEffectBck;
    dKy_tevstr_c mRemoteMidnaTevStr;
    /* 0x968 */ J3DModel* mpMidnaModel;
    /* 0x96C */ J3DModel* mpMidnaMaskModel;
    /* 0x970 */ J3DModel* mpMidnaHandModel;
    /* 0x974 */ J3DModel* mpMidnaHairModel;
    /* 0x978 */ J3DModel* mpShadowMidnaModel;
    /* 0x97C */ J3DModel* mpShadowMidnaMaskModel;
    /* 0x980 */ J3DModel* mpShadowMidnaHandModel;
    /* 0x984 */ J3DModel* mpShadowMidnaHairModel;
    /* 0x988 */ J3DModel* mpMidnaGlowModel;
    /* 0x98C */ mDoExt_invisibleModel mShadowMidnaInvModel;
    /* 0x994 */ mDoExt_invisibleModel mShadowMidnaMaskInvModel;
    /* 0x99C */ mDoExt_invisibleModel mShadowMidnaHandInvModel;
    /* 0x9A4 */ mDoExt_invisibleModel mShadowMidnaHairInvModel;
    /* 0x9AC */ J3DModel* mpHeavyBootModels[2];
    /* 0x988 */ J3DAnmTevRegKey* mpHeldItemBrk;
    J3DAnmTexPattern* mpBottleBtp;
    J3DAnmTextureSRTKey* mpBottleBtkSwing;
    J3DAnmTextureSRTKey* mpBottleBtkAction;
    J3DAnmTextureSRTKey* mpBottleBtkFinish;
    /* 0x98C */ AramResourceCacheEntry mAramResourceCache[16];
    /* 0xA0C */ J3DAnmTevRegKey* mpMagicArmorBodyBrk;
    /* 0xA10 */ J3DAnmTevRegKey* mpMagicArmorHeadBrk;
    /* 0xA14 */ mDoExt_bckAnm* mpMotionBck;
    mDoExt_bckAnm* mpFaceBckAnm;
    mDoExt_btpAnm* mpFaceBtpAnm;
    mDoExt_btkAnm* mpFaceBtkAnm;
    u8* mpFaceBckBuffer;
    u8* mpFaceBtpBuffer;
    u8* mpFaceBtkBuffer;
    u16 mLoadedFaceBck;
    u16 mLoadedFaceBckArc;
    u16 mLoadedFaceBtp;
    u16 mLoadedFaceBtpArc;
    u16 mLoadedFaceBtk;
    u16 mLoadedFaceBtkArc;
    mDoExt_MtxCalcAnmBlendTbl* mpBlendMtxCalc;
    mDoExt_MtxCalcAnmBlendTbl* mpUpperBlendMtxCalc;
    mDoExt_MtxCalcOldFrame* mpOldFrameCalc;
    J3DTransformInfo mOldFrameTransInfo[40];
    Quaternion mOldFrameQuats[40];
    mDoExt_AnmRatioPack mBlendAnmPacks[6];
    u16 mBlendSlotBck[6];
    f32 mBlendSlotFrame[6];
    bool mBlendSlotFrameValid[6];
    f32 mBlendSlotPresentedRatio[6];
    bool mBlendSignatureValid;
    int mLastBlendProcId;
    u16 mLastBlendUnderBck[3];
    u16 mLastBlendUpperBck[3];
    /* 0xA18 */ BckCacheEntry mBckCache[48];
    BckCacheEntry mBlendBckCache[6];
    /* 0xB98 */ u16 mMotionBckResId;
    /* 0xB8C */ f32 mRemoteMoveSpeed;
    /* 0xB90 */ cXyz mLastRemotePos;
    /* 0xB9C */ int mRemoteProcId;
    /* 0xBA0 */ int mRemoteProcVar0;
    /* 0xBA4 */ int mRemoteProcVar1;
    /* 0xBA8 */ int mRemoteProcVar2;
    /* 0xBAC */ int mRemoteProcVar3;
    /* 0xBB0 */ int mRemoteProcVar5;
    /* 0xBB4 */ f32 mRemoteUnderFrame;
    /* 0xBB8 */ u16 mRemoteUnderBck0;
    u16 mRemoteUnderBckArc0;
    /* 0xBBC */ f32 mRemoteUnderFrame0;
    /* 0xBC0 */ f32 mRemoteUnderRate0;
    f32 mRemoteUnderRatio0;
    u16 mRemoteUnderBck1;
    u16 mRemoteUnderBckArc1;
    f32 mRemoteUnderFrame1;
    f32 mRemoteUnderRate1;
    f32 mRemoteUnderRatio1;
    u16 mRemoteUnderBck2;
    u16 mRemoteUnderBckArc2;
    f32 mRemoteUnderFrame2;
    f32 mRemoteUnderRate2;
    f32 mRemoteUnderRatio2;
    u16 mRemoteUpperBck0;
    u16 mRemoteUpperBckArc0;
    f32 mRemoteUpperFrame0;
    f32 mRemoteUpperRate0;
    f32 mRemoteUpperRatio0;
    u16 mRemoteUpperBck1;
    u16 mRemoteUpperBckArc1;
    f32 mRemoteUpperFrame1;
    f32 mRemoteUpperRate1;
    f32 mRemoteUpperRatio1;
    /* 0xBC4 */ u16 mRemoteUpperBck2;
    u16 mRemoteUpperBckArc2;
    /* 0xBC8 */ f32 mRemoteUpperFrame2;
    /* 0xBCC */ f32 mRemoteUpperRate2;
    f32 mRemoteUpperRatio2;
    u16 mRemoteFaceBck;
    u16 mRemoteFaceBckArc;
    f32 mRemoteFaceBckFrame;
    u16 mRemoteFaceBtp;
    u16 mRemoteFaceBtpArc;
    f32 mRemoteFaceBtpFrame;
    u16 mRemoteFaceBtk;
    u16 mRemoteFaceBtkArc;
    f32 mRemoteFaceBtkFrame;
    s16 mRemoteShapeX;
    s16 mRemoteShapeZ;
    s16 mRemoteBodyAngleX;
    s16 mRemoteBodyAngleY;
    s16 mRemoteBodyAngleZ;
    s16 mRemoteBodyTwistY;
    s16 mRemoteNeckJointX;
    s16 mRemoteNeckJointY;
    s16 mRemoteNeckJointZ;
    s16 mRemoteLowerJointX;
    s16 mRemoteLowerJointZ;
    s16 mRemoteRootJointX;
    s16 mRemoteRootJointZ;
    u8 mRemoteBlendMode;
    f32 mRemoteUpperSavedRatio;
    bool mPrevRemoteBodyRootValid;
    cXyz mPrevRemoteBodyRoot;
    bool mRemoteBodyRootValid;
    cXyz mRemoteBodyRoot;
    std::array<int16_t, 6> mRemoteLegIkAngles;
    std::array<int16_t, 6> mRemoteArmIkAngles;
    std::array<int16_t, 6> mRemoteArmRotA;
    std::array<int16_t, 6> mRemoteArmRotB;
    std::array<int16_t, 3> mRemoteFishingArm1Angle;
    std::array<int16_t, 3> mRemoteFishingArm2Angle;
    bool mRemoteAnimationFrameCorrectionPending;
    std::array<int16_t, 10> mRemoteHatRotA;
    std::array<int16_t, 10> mRemoteHatRotB;
    std::array<int16_t, 3> mRemoteHatSwing;
    s16 mRemoteHatShapeY;
    /* 0xBD0 */ f32 mRemoteTransformFrame;
    /* 0xBD4 */ bool mRemoteTransformFrameValid;
    /* 0xBD8 */ u16 mRemoteEquipItem;
    u16 mRemotePresentedItem;
    u16 mLoadedPresentedItem;
    s16 mPresentedItemAngle;
    /* 0xBDC */ int mRemoteSwordVariant;
    /* 0xBE0 */ int mRemoteShieldVariant;
    /* 0xBE4 */ int mLoadedSwordVariant;
    /* 0xBE8 */ int mLoadedShieldVariant;
    /* 0xBEC */ u16 mLoadedHeldItem;
    /* 0xBEE */ bool mRemoteSwordDraw;
    /* 0xBEF */ bool mRemoteShieldDraw;
    bool mRemoteShieldGuardActive;
    bool mRemoteSwordHandAttached;
    bool mRemoteShieldHandAttached;
    /* 0xBF0 */ bool mRemoteSwordOut;
    /* 0xBF1 */ bool mRemoteHeavyBoots;
    bool mRemoteZoraMaskDraw;
    bool mRemoteMagicArmorPowered;
    int mAppliedMagicArmorPowered;
    /* 0xBF2 */ bool mRemoteMidnaDraw;
    /* 0xBF3 */ bool mRemoteMidnaMaskDraw;
    /* 0xBF4 */ bool mRemoteMidnaHandDraw;
    /* 0xBF5 */ bool mRemoteMidnaHairDraw;
    /* 0xBF6 */ bool mRemoteMidnaShadowForm;
    /* 0xBF1 */ bool mHeldItemMatrixValid;
    /* 0xBF2 */ bool mHookTipMatrixValid;
    /* 0xBF3 */ bool mHookSubItemMatrixValid;
    /* 0xBF4 */ bool mHookSubTipMatrixValid;
    /* 0xBF5 */ bool mArrowMatrixValid;
    /* 0xBF6 */ bool mKanteraMatrixValid;
    /* 0xBF7 */ bool mKanteraGlowMatrixValid;
    u32 mKanteraGlowBufferZ;
    f32 mKanteraGlowDepth;
    f32 mKanteraGlowScale;
    /* 0xBF8 */ bool mItemActorMatrixValid;
    /* 0xBF9 */ bool mRideActorMatrixValid;
    /* 0xBFA */ bool mMidnaMatrixValid;
    /* 0xBFB */ bool mMidnaMaskMatrixValid;
    /* 0xBFC */ bool mMidnaHandMatrixValid;
    /* 0xBFD */ bool mMidnaHairMatrixValid;
    /* 0xBFE */ bool mMidnaGlowMatrixValid;
    /* 0xC04 */ bool mRemoteItemDraw;
    /* 0xC05 */ bool mRemoteKanteraDraw;
    /* 0xC06 */ bool mHasRemotePose;
    /* 0xC07 */ bool mHasRemoteMatrices;
    bool mHasRemoteAttachmentMatrices;
    dusk::multiplayer::RemoteLinkMatrixSnapshot mRemoteAttachmentMatrices;
    /* 0xC14 */ int mClothesVariant;
    /* 0xC08 */ J3DShape* mpLeftBodyHandShape;
    /* 0xC0C */ J3DShape* mpRightBodyHandShape;
    int mRemoteLeftHandShape;
    int mRemoteRightHandShape;
    /* 0xC10 */ int mRemoteItemActorKind;
    /* 0xC14 */ int mRemoteRideActorKind;
    /* 0xC18 */ int mLoadedItemActorKind;
    /* 0xC1C */ int mLoadedRideActorKind;
    bool mRemoteSpinnerVisualValid;
    bool mRemoteSpinnerLinkAnchored;
    bool mSpinnerBckInitialized;
    bool mHookshotItemBckInitialized;
    bool mHookshotTipBckInitialized;
    cXyz mRemoteSpinnerVisualPos;
    csXyz mRemoteSpinnerVisualShape;
    s16 mRemoteSpinnerVisualRotY;
    f32 mRemoteSpinnerVisualYOffset;
    cXyz mPreviousSpinnerBasePos;
    cXyz mCurrentSpinnerBasePos;
    bool mSpinnerBaseHistoryValid;
    u32 mRemoteSpinnerJumpEpoch;
    u32 mAppliedSpinnerJumpEpoch;
    bool mRemoteIronBallVisualValid;
    bool mRemoteIronBallLinkAnchored;
    cXyz mRemoteIronBallVisualPos;
    csXyz mRemoteIronBallVisualAngle;
    cXyz mRemoteIronBallPreviousVisualPos;
    csXyz mRemoteIronBallPreviousVisualAngle;
    bool mRemoteIronBallPreviousVisualValid;
    J3DModelData* mpIronBallChainModelData;
    IronBallChainPacket mIronBallChainPacket;
    uint8_t mRemoteIronBallChainCount;
    std::array<cXyz, 100> mRemoteIronBallChainDirections;
    std::array<cXyz, 100> mRemoteIronBallChainPreviousDirections;
    uint8_t mRemoteIronBallChainPreviousCount;
    bool mRemoteIronBallChainPreviousValid;
    bool mRemoteIronBallChainEndOffsetValid;
    bool mRemoteIronBallChainPreviousEndOffsetValid;
    cXyz mRemoteIronBallChainEndOffset;
    cXyz mRemoteIronBallChainPreviousEndOffset;
    cXyz mRemoteIronBallRenderedPos;
    csXyz mRemoteIronBallRenderedAngle;
    cXyz mRemoteIronBallHandRoot;
    bool mRemoteIronBallRenderReady;
    bool mRemoteHookshotVisualValid;
    bool mRemoteHookshotLeft;
    s16 mRemoteHookshotArmAimX;
    s16 mRemoteHookshotArmAimY;
    bool mRemoteHookshotTopLinkAnchored;
    bool mRemoteHookshotSubTopLinkAnchored;
    cXyz mRemoteHookshotTop;
    cXyz mRemoteHookshotPreviousTop;
    cXyz mRemoteHookshotSubTop;
    cXyz mRemoteHookshotPreviousSubTop;
    csXyz mRemoteHookshotTopAngle;
    csXyz mRemoteHookshotPreviousTopAngle;
    csXyz mRemoteHookshotSubTopAngle;
    csXyz mRemoteHookshotPreviousSubTopAngle;
    bool mRemoteHookshotPreviousValid;
    s16 mRemoteHookshotStopTime;
    f32 mRemoteHookshotItemFrame;
    f32 mRemoteHookshotPreviousTipFrame;
    f32 mRemoteHookshotTipFrame;
    f32 mRemoteHookshotPreviousSubTipFrame;
    f32 mRemoteHookshotSubTipFrame;
    cXyz mRemoteHookshotRenderedTop;
    cXyz mRemoteHookshotRenderedSubTop;
    cXyz mRemoteHookshotRoot;
    cXyz mRemoteHookshotSubRoot;
    bool mRemoteHookshotRenderReady;
    J3DModelData* mpHookshotChainModelData;
    HookshotChainPacket mHookshotChainPacket;
    bool mRemoteBoomerangVisualValid;
    bool mRemoteBoomerangLinkAnchored;
    cXyz mRemoteBoomerangVisualPos;
    cXyz mRemoteBoomerangPreviousVisualPos;
    csXyz mRemoteBoomerangVisualAngle;
    csXyz mRemoteBoomerangPreviousVisualAngle;
    bool mRemoteBoomerangPreviousVisualValid;
    cXyz mRemoteBoomerangRenderedPos;
    csXyz mRemoteBoomerangRenderedAngle;
    bool mRemoteCopyRodVisualValid;
    bool mRemoteCopyRodTopUse;
    bool mRemoteFishingRodVisualValid;
    bool mRemoteFishingRodLineValid;
    bool mRemoteFishingRodEndValid;
    bool mRemoteFishingRodPreviousValid;
    std::array<cXyz, 15> mRemoteFishingRodSegmentDeltas;
    std::array<cXyz, 15> mRemoteFishingRodPreviousSegmentDeltas;
    std::array<cXyz, dusk::multiplayer::kFishingRodLineSampleCount>
        mRemoteFishingRodLineOffsets;
    std::array<cXyz, dusk::multiplayer::kFishingRodLineSampleCount>
        mRemoteFishingRodPreviousLineOffsets;
    mDoExt_3DlineMat0_c mRemoteFishingRodLine;
    bool mRemoteFishingRodLineInitialized;
    u8 mRemoteFishingRodAction;
    u8 mRemoteFishingRodHookKind;
    u8 mRemoteFishingRodBaitKind;
    s16 mRemoteFishingRodEndRoll;
    s16 mRemoteFishingRodPreviousEndRoll;
    cXyz mRemoteFishingRodBobberOffset;
    cXyz mRemoteFishingRodPreviousBobberOffset;
    std::array<s16, 6> mRemoteFishingRodBobberAngles;
    std::array<s16, 6> mRemoteFishingRodPreviousBobberAngles;
    s16 mRemoteFishingRodCounter;
    bool mRemoteBowVisualValid;
    bool mRemoteBowGrabLeft;
    u16 mRemoteBowBck;
    u16 mLoadedBowBck;
    f32 mRemoteBowFrame;
    bool mRemoteBowArrowVisible;
    bool mRemoteBowArrowBomb;
    int mLoadedArrowBomb;
    bool mRemoteLanternVisualValid;
    bool mRemoteLanternLinkAnchored;
    bool mRemoteLanternHandAttached;
    bool mRemoteLanternLit;
    cXyz mRemoteLanternPos;
    cXyz mRemoteLanternPreviousPos;
    csXyz mRemoteLanternBaseAngle;
    csXyz mRemoteLanternPreviousBaseAngle;
    csXyz mRemoteLanternJointAngle;
    csXyz mRemoteLanternPreviousJointAngle;
    bool mRemoteLanternPreviousValid;
    cXyz mRemoteLanternRenderedPos;
    csXyz mRemoteLanternRenderedBaseAngle;
    u32 mRemoteLanternFlameEmitterId;
    cXyz mRemoteLanternFlamePos;
    bool mRemoteBottleVisualValid;
    bool mRemoteBottleOilRightAttached;
    bool mRemoteBottleJointRightAttached;
    bool mRemoteBottleDrinkMaterialSet;
    s8 mRemoteBottleMaterialStage;
    f32 mRemoteBottleBrkFrame;
    f32 mRemoteBottleBtpFrame;
    f32 mRemoteBottleBtkSwingFrame;
    f32 mRemoteBottleBtkActionFrame;
    f32 mRemoteBottleBtkFinishFrame;
    s8 mRemoteBottleContentKind;
    f32 mRemoteBottleContentFrame;
    int mLoadedBottleContentKind;
    /* 0xC20 */ int mRemoteBombFlashTicks;
    int mRemoteBombExTime;
    int mRemoteBombFlash;
    bool mRemoteBombLastPosValid;
    bool mRemoteBombWasWater;
    bool mRemoteBombExplosionSpawned;
    fpc_ProcID mRemoteBombActorId;
    int mRemoteBombActorKind;
    s16 mRemoteBombAngleY;
    cXyz mRemoteBombLastPos;
    dusk::multiplayer::RemoteModelMatrixSnapshot mPrevBodyMatrixSnapshot;
    dusk::multiplayer::RemoteModelMatrixSnapshot mCurrBodyMatrixSnapshot;
    bool mPrevBodyMatrixSnapshotValid;
    bool mCurrBodyMatrixSnapshotValid;
    u32 mRemoteMatrixTicksSinceCapture;
    u32 mRemoteMatrixBlendDurationTicks;
    RemoteModelMatrixInterpState mFaceMatrixInterp;
    RemoteModelMatrixInterpState mHandMatrixInterp;
    RemoteModelMatrixInterpState mSwordMatrixInterp;
    RemoteModelMatrixInterpState mSheathMatrixInterp;
    RemoteModelMatrixInterpState mShieldMatrixInterp;
    RemoteModelMatrixInterpState mHeldItemMatrixInterp;
    RemoteModelMatrixInterpState mHookTipMatrixInterp;
    RemoteModelMatrixInterpState mHookSubItemMatrixInterp;
    RemoteModelMatrixInterpState mHookSubTipMatrixInterp;
    RemoteModelMatrixInterpState mArrowMatrixInterp;
    RemoteModelMatrixInterpState mKanteraMatrixInterp;
    RemoteModelMatrixInterpState mKanteraGlowMatrixInterp;
    RemoteModelMatrixInterpState mItemActorMatrixInterp;
    RemoteModelMatrixInterpState mRideActorMatrixInterp;
    RemoteModelMatrixInterpState mMidnaMatrixInterp;
    RemoteModelMatrixInterpState mMidnaMaskMatrixInterp;
    RemoteModelMatrixInterpState mMidnaHandMatrixInterp;
    RemoteModelMatrixInterpState mMidnaHairMatrixInterp;
    RemoteModelMatrixInterpState mMidnaGlowMatrixInterp;
    dCcD_Stts mPvpTargetStts;
    dCcD_Cyl mPvpTargetCyl;
    bool mPvpTargetCollisionInitialized;
    bool mRemotePresentationVisible;
    bool mRemoteTransformBridgeVisible;
    bool mRemoteTransformBridgeSenderWolf;
    int mRemoteTransformBridgeColor;
    cXyz mRemoteTransformBridgePos;
    bool mRemoteTransformEffectActive;
    bool mRemoteTransformToWolf;
    bool mRemoteTransformSenderWolf;
    int mRemoteTransformEffectProcVar1;
    int mRemoteTransformEffectColor;
    std::array<int64_t, 6> mRemoteWaterDropAnchors{};
    std::array<dusklight_online::game::WetState, 2> mRemoteWetState{};
    bool mRemoteWaterDropStrong = false;
    std::array<u32, 2> mRemoteWaterDropEmitters{};
    u16 mRemotePowerUpTimer = 0;
    f32 mRemotePowerUpIntensity = 0.0f;
    void updateRemoteWaterDrops();
    void stopRemoteWaterDrops();

    int mRemoteTransformEffectProcVar5;
    f32 mRemoteTransformEffectFrame;
    cXyz mRemoteTransformEffectPos;
    u16 mTransformEffectBckResId;
    std::array<u32, 3> mRemoteTransformEmitterIds;
    s16 mPvpShieldFrontAngle;
    std::array<u32, 3> mPvpMidnaBindIds;
    bool mPvpMidnaBindActive;
    Z2SoundObjSimple mActiveSoundObj;
    std::array<ActiveRemoteSound, 8> mActiveSounds;
    dusklight_online::game::AudioLifetimeTracker mTrackedAudio;
    std::array<JAISoundHandle, 8> mTrackedSoundHandles;
    /* 0xC24 */ int mMidnaHairShape;
    /* 0xC28 */ bool mSlotReserved;
};

#endif /* D_A_REMOTE_LINK_H */
