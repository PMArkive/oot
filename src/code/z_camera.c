#include "ultra64.h"
#include "global.h"
#include "quake.h"
#include "terminal.h"
#include "overlays/actors/ovl_En_Horse/z_en_horse.h"

s16 Camera_ChangeSettingFlags(Camera* camera, s16 setting, s16 flags);
s32 Camera_ChangeModeFlags(Camera* camera, s16 mode, u8 flags);
s32 Camera_QRegInit(void);
s32 Camera_UpdateWater(Camera* camera);

// Camera will reload its paramData. Usually that means setting the read-only data from what is stored in
// CameraModeValue arrays. Although sometimes some read-write data is reset as well
#define RELOAD_PARAMS(camera) (camera->animState == 0 || camera->animState == 10 || camera->animState == 20)

/**
 * Camera data is stored in both read-only data and OREG as s16, and then converted to the appropriate type during
 * runtime. If a small f32 is being stored as an s16, it is common to store that value 100 times larger than the
 * original value. This is then scaled back down during runtime with the CAM_DATA_SCALED macro.
 */
#define CAM_DATA_SCALED(x) ((x)*0.01f)

// Load the next value from camera read-only data stored in CameraModeValue
#define GET_NEXT_RO_DATA(values) ((values++)->val)
// Load the next value and scale down from camera read-only data stored in CameraModeValue
#define GET_NEXT_SCALED_RO_DATA(values) CAM_DATA_SCALED(GET_NEXT_RO_DATA(values))

#define FLG_ADJSLOPE (1 << 0)
#define FLG_OFFGROUND (1 << 7)

#define DISTORTION_HOT_ROOM (1 << 0)
#define DISTORTION_UNDERWATER_WEAK (1 << 1)
#define DISTORTION_UNDERWATER_MEDIUM (1 << 2)
#define DISTORTION_UNDERWATER_STRONG (1 << 3)
#define DISTORTION_UNDERWATER_FISHING (1 << 4)

#include "z_camera_data.inc.c"

/*===============================================================*/

/**
 * Interpolates along a curve between 0 and 1 with a period of
 * -a <= p <= a at time `b`
 */
f32 Camera_InterpolateCurve(f32 a, f32 b) {
    f32 ret;
    f32 absB;
    f32 t = 0.4f;
    f32 t2;
    f32 t3;
    f32 t4;

    absB = fabsf(b);
    if (a < absB) {
        ret = 1.0f;
    } else {
        t2 = 1.0f - t;
        if ((a * t2) > absB) {
            t3 = SQ(b) * (1.0f - t);
            t4 = SQ(a * t2);
            ret = t3 / t4;
        } else {
            t3 = SQ(a - absB) * t;
            t4 = SQ(0.4f * a);
            ret = 1.0f - (t3 / t4);
        }
    }
    return ret;
}

/*
 * Performs linear interpolation between `cur` and `target`.  If `cur` is within
 * `minDiff` units, the result is rounded up to `target`
 */
f32 Camera_LERPCeilF(f32 target, f32 cur, f32 stepScale, f32 minDiff) {
    f32 diff = target - cur;
    f32 step;
    f32 ret;

    if (fabsf(diff) >= minDiff) {
        step = diff * stepScale;
        ret = cur + step;
    } else {
        ret = target;
    }

    return ret;
}

/*
 * Performs linear interpolation between `cur` and `target`.  If `cur` is within
 * `minDiff` units, the result is rounded down to `cur`
 */
f32 Camera_LERPFloorF(f32 target, f32 cur, f32 stepScale, f32 minDiff) {
    f32 diff = target - cur;
    f32 step;
    f32 ret;

    if (fabsf(diff) >= minDiff) {
        step = diff * stepScale;
        ret = cur + step;
    } else {
        ret = cur;
    }

    return ret;
}

/*
 * Performs linear interpolation between `cur` and `target`.  If `cur` is within
 * `minDiff` units, the result is rounded up to `target`
 */
s16 Camera_LERPCeilS(s16 target, s16 cur, f32 stepScale, s16 minDiff) {
    s16 diff = target - cur;
    s16 step;
    s32 ret;

    if (ABS(diff) >= minDiff) {
        step = diff * stepScale + 0.5f;
        ret = cur + step;
    } else {
        ret = target;
    }

    return ret;
}

/*
 * Performs linear interpolation between `cur` and `target`.  If `cur` is within
 * `minDiff` units, the result is rounded down to `cur`
 */
s16 Camera_LERPFloorS(s16 target, s16 cur, f32 stepScale, s16 minDiff) {
    s16 diff = target - cur;
    s16 step;
    s32 ret;

    if (ABS(diff) >= minDiff) {
        step = diff * stepScale + 0.5f;
        ret = cur + step;
    } else {
        ret = cur;
    }

    return ret;
}

/*
 * Performs linear interpolation between `cur` and `target`.  If `cur` is within
 * `minDiff` units, the result is rounded up to `target`
 */
void Camera_LERPCeilVec3f(Vec3f* target, Vec3f* cur, f32 yStepScale, f32 xzStepScale, f32 minDiff) {
    cur->x = Camera_LERPCeilF(target->x, cur->x, xzStepScale, minDiff);
    cur->y = Camera_LERPCeilF(target->y, cur->y, yStepScale, minDiff);
    cur->z = Camera_LERPCeilF(target->z, cur->z, xzStepScale, minDiff);
}

void func_80043ABC(Camera* camera) {
    camera->yawUpdateRateInv = 100.0f;
    camera->pitchUpdateRateInv = R_CAM_PITCH_UPDATE_RATE_INV;
    camera->rUpdateRateInv = R_CAM_R_UPDATE_RATE_INV;
    camera->xzOffsetUpdateRate = CAM_DATA_SCALED(R_CAM_XZ_OFFSET_UPDATE_RATE);
    camera->yOffsetUpdateRate = CAM_DATA_SCALED(R_CAM_Y_OFFSET_UPDATE_RATE);
    camera->fovUpdateRate = CAM_DATA_SCALED(R_CAM_FOV_UPDATE_RATE);
}

void func_80043B60(Camera* camera) {
    camera->rUpdateRateInv = OREG(27);
    camera->yawUpdateRateInv = OREG(27);
    camera->pitchUpdateRateInv = OREG(27);
    camera->xzOffsetUpdateRate = 0.001f;
    camera->yOffsetUpdateRate = 0.001f;
    camera->fovUpdateRate = 0.001f;
}

Vec3f* Camera_Vec3sToVec3f(Vec3f* dest, Vec3s* src) {
    Vec3f copy;

    copy.x = src->x;
    copy.y = src->y;
    copy.z = src->z;

    *dest = copy;
    return dest;
}

Vec3f* Camera_AddVecGeoToVec3f(Vec3f* dest, Vec3f* a, VecGeo* geo) {
    Vec3f sum;
    Vec3f b;

    OLib_VecGeoToVec3f(&b, geo);

    sum.x = a->x + b.x;
    sum.y = a->y + b.y;
    sum.z = a->z + b.z;

    *dest = sum;

    return dest;
}

Vec3f* Camera_Vec3fTranslateByUnitVector(Vec3f* dest, Vec3f* src, Vec3f* unitVector, f32 uvScale) {
    Vec3f copy;

    copy.x = src->x + (unitVector->x * uvScale);
    copy.y = src->y + (unitVector->y * uvScale);
    copy.z = src->z + (unitVector->z * uvScale);

    *dest = copy;
    return dest;
}

/**
 * Detects the collision poly between `from` and `to`, places collision info in `to`
 */
s32 Camera_BGCheckInfo(Camera* camera, Vec3f* from, CamColChk* to) {
    CollisionContext* colCtx = &camera->play->colCtx;
    Vec3f toNewPos;
    Vec3f toPoint;
    Vec3f fromToNorm;
    f32 floorPolyY;
    CollisionPoly* floorPoly;
    s32 floorBgId;
    VecGeo fromToOffset;

    OLib_Vec3fDiffToVecGeo(&fromToOffset, from, &to->pos);
    fromToOffset.r += 8.0f;
    Camera_AddVecGeoToVec3f(&toPoint, from, &fromToOffset);

    if (!BgCheck_CameraLineTest1(colCtx, from, &toPoint, &toNewPos, &to->poly, 1, 1, 1, -1, &to->bgId)) {
        // no poly in path.
        OLib_Vec3fDistNormalize(&fromToNorm, from, &to->pos);

        to->norm.x = -fromToNorm.x;
        to->norm.y = -fromToNorm.y;
        to->norm.z = -fromToNorm.z;

        toNewPos = to->pos;
        toNewPos.y += 5.0f;
        floorPolyY = BgCheck_CameraRaycastDown2(colCtx, &floorPoly, &floorBgId, &toNewPos);

        if ((to->pos.y - floorPolyY) > 5.0f) {
            // if the y distance from the check point to the floor is more than 5 units
            // the point is not colliding with any collision.
            to->pos.x += to->norm.x;
            to->pos.y += to->norm.y;
            to->pos.z += to->norm.z;
            return 0;
        }

        to->poly = floorPoly;
        toNewPos.y = floorPolyY + 1.0f;
        to->bgId = floorBgId;
    }

    to->norm.x = COLPOLY_GET_NORMAL(to->poly->normal.x);
    to->norm.y = COLPOLY_GET_NORMAL(to->poly->normal.y);
    to->norm.z = COLPOLY_GET_NORMAL(to->poly->normal.z);
    to->pos.x = to->norm.x + toNewPos.x;
    to->pos.y = to->norm.y + toNewPos.y;
    to->pos.z = to->norm.z + toNewPos.z;

    return floorBgId + 1;
}

/**
 * Detects if there is collision between `from` and `to`
 */
s32 Camera_BGCheck(Camera* camera, Vec3f* from, Vec3f* to) {
    CamColChk toCol;
    s32 bgId;

    toCol.pos = *to;
    bgId = Camera_BGCheckInfo(camera, from, &toCol);
    *to = toCol.pos;
    return bgId;
}

s32 func_80043F94(Camera* camera, Vec3f* from, CamColChk* to) {
    CollisionContext* colCtx = &camera->play->colCtx;
    Vec3f toNewPos;
    Vec3f toPos;
    Vec3f fromToNorm;
    Vec3f playerFloorNormF;
    f32 floorY;
    CollisionPoly* floorPoly;
    s32 bgId;
    VecGeo fromToGeo;

    OLib_Vec3fDiffToVecGeo(&fromToGeo, from, &to->pos);
    fromToGeo.r += 8.0f;
    Camera_AddVecGeoToVec3f(&toPos, from, &fromToGeo);
    if (!BgCheck_CameraLineTest1(colCtx, from, &toPos, &toNewPos, &to->poly, 1, 1, 1, -1, &to->bgId)) {
        OLib_Vec3fDistNormalize(&fromToNorm, from, &to->pos);
        to->norm.x = -fromToNorm.x;
        to->norm.y = -fromToNorm.y;
        to->norm.z = -fromToNorm.z;
        toNewPos = to->pos;
        toNewPos.y += 5.0f;
        floorY = BgCheck_CameraRaycastDown2(colCtx, &floorPoly, &bgId, &toNewPos);
        if ((to->pos.y - floorY) > 5.0f) {
            // to is not on the ground or below it.
            to->pos.x += to->norm.x;
            to->pos.y += to->norm.y;
            to->pos.z += to->norm.z;
            return 0;
        }
        // to is touching the ground, move it up 1 unit.
        to->poly = floorPoly;
        toNewPos.y = floorY + 1.0f;
        to->bgId = bgId;
    }
    to->norm.x = COLPOLY_GET_NORMAL(to->poly->normal.x);
    to->norm.y = COLPOLY_GET_NORMAL(to->poly->normal.y);
    to->norm.z = COLPOLY_GET_NORMAL(to->poly->normal.z);
    if ((to->norm.y > 0.5f) || (to->norm.y < -0.8f)) {
        to->pos.x = to->norm.x + toNewPos.x;
        to->pos.y = to->norm.y + toNewPos.y;
        to->pos.z = to->norm.z + toNewPos.z;
    } else if (playerFloorPoly != NULL) {
        playerFloorNormF.x = COLPOLY_GET_NORMAL(playerFloorPoly->normal.x);
        playerFloorNormF.y = COLPOLY_GET_NORMAL(playerFloorPoly->normal.y);
        playerFloorNormF.z = COLPOLY_GET_NORMAL(playerFloorPoly->normal.z);
        if (Math3D_LineSegVsPlane(playerFloorNormF.x, playerFloorNormF.y, playerFloorNormF.z, playerFloorPoly->dist,
                                  from, &toPos, &toNewPos, 1)) {
            // line is from->to is touching the poly the player is on.
            to->norm = playerFloorNormF;
            to->poly = playerFloorPoly;
            to->bgId = camera->bgId;
            to->pos.x = to->norm.x + toNewPos.x;
            to->pos.y = to->norm.y + toNewPos.y;
            to->pos.z = to->norm.z + toNewPos.z;
        } else {
            OLib_Vec3fDistNormalize(&fromToNorm, from, &to->pos);
            to->norm.x = -fromToNorm.x;
            to->norm.y = -fromToNorm.y;
            to->norm.z = -fromToNorm.z;
            to->pos.x += to->norm.x;
            to->pos.y += to->norm.y;
            to->pos.z += to->norm.z;
            return 0;
        }
    }
    return 1;
}

void func_80044340(Camera* camera, Vec3f* arg1, Vec3f* arg2) {
    CamColChk sp20;
    Vec3s unused;

    sp20.pos = *arg2;
    func_80043F94(camera, arg1, &sp20);
    *arg2 = sp20.pos;
}

/**
 * Checks if `from` to `to` is looking from the outside of a poly towards the front
 */
s32 Camera_CheckOOB(Camera* camera, Vec3f* from, Vec3f* to) {
    s32 pad;
    Vec3f intersect;
    s32 pad2;
    s32 bgId;
    CollisionPoly* poly;
    CollisionContext* colCtx = &camera->play->colCtx;

    poly = NULL;
    if (BgCheck_CameraLineTest1(colCtx, from, to, &intersect, &poly, 1, 1, 1, 0, &bgId) &&
        (CollisionPoly_GetPointDistanceFromPlane(poly, from) < 0.0f)) {
        // if there is a poly between `from` and `to` and the `from` is behind the poly.
        return true;
    }

    return false;
}

/**
 * Gets the floor position underneath `chkPos`, and returns the normal of the floor to `floorNorm`,
 * and bgId to `bgId`.  If no floor is found, then the normal is a flat surface pointing upwards.
 */
f32 Camera_GetFloorYNorm(Camera* camera, Vec3f* floorNorm, Vec3f* chkPos, s32* bgId) {
    s32 pad;
    CollisionPoly* floorPoly;
    f32 floorY = BgCheck_EntityRaycastDown3(&camera->play->colCtx, &floorPoly, bgId, chkPos);

    if (floorY == BGCHECK_Y_MIN) {
        // no floor
        floorNorm->x = 0.0f;
        floorNorm->y = 1.0f;
        floorNorm->z = 0.0f;
    } else {
        floorNorm->x = COLPOLY_GET_NORMAL(floorPoly->normal.x);
        floorNorm->y = COLPOLY_GET_NORMAL(floorPoly->normal.y);
        floorNorm->z = COLPOLY_GET_NORMAL(floorPoly->normal.z);
    }

    return floorY;
}

/**
 * Gets the position of the floor from `pos`
 */
f32 Camera_GetFloorY(Camera* camera, Vec3f* pos) {
    Vec3f posCheck;
    Vec3f floorNorm;
    s32 bgId;

    posCheck = *pos;
    posCheck.y += 80.0f;

    return Camera_GetFloorYNorm(camera, &floorNorm, &posCheck, &bgId);
}

/**
 * Gets the position of the floor from `pos`, and if the floor is considered not solid,
 * it checks the next floor below that up to 3 times.  Returns the normal of the floor into `norm`
 */
f32 Camera_GetFloorYLayer(Camera* camera, Vec3f* norm, Vec3f* pos, s32* bgId) {
    CollisionPoly* floorPoly;
    CollisionContext* colCtx = &camera->play->colCtx;
    f32 floorY;
    s32 i;

    for (i = 3; i > 0; i--) {
        floorY = BgCheck_CameraRaycastDown2(colCtx, &floorPoly, bgId, pos);
        if (floorY == BGCHECK_Y_MIN ||
            (camera->playerGroundY < floorY && !(COLPOLY_GET_NORMAL(floorPoly->normal.y) > 0.5f))) {
            // no floor, or player is below the floor and floor is not considered steep
            norm->x = 0.0f;
            norm->y = 1.0f;
            norm->z = 0.0f;
            floorY = BGCHECK_Y_MIN;
            break;
        } else if (SurfaceType_GetFloorType(colCtx, floorPoly, *bgId) == FLOOR_TYPE_1) {
            // floor is not solid, check below that floor.
            pos->y = floorY - 10.0f;
            continue;
        } else {
            norm->x = COLPOLY_GET_NORMAL(floorPoly->normal.x);
            norm->y = COLPOLY_GET_NORMAL(floorPoly->normal.y);
            norm->z = COLPOLY_GET_NORMAL(floorPoly->normal.z);
            break;
        }
    }
    if (i == 0) {
        osSyncPrintf(VT_COL(YELLOW, BLACK) "camera: foward check: too many layer!\n" VT_RST);
    }
    return floorY;
}

/**
 * Returns the CameraSettingType of the camera at index `bgCamIndex`
 */
s16 Camera_GetBgCamSetting(Camera* camera, s32 bgCamIndex) {
    return BgCheck_GetBgCamSettingImpl(&camera->play->colCtx, bgCamIndex, BGCHECK_SCENE);
}

/**
 * Returns the bgCamFuncData using the current bgCam index
 */
Vec3s* Camera_GetBgCamFuncData(Camera* camera) {
    return BgCheck_GetBgCamFuncDataImpl(&camera->play->colCtx, camera->bgCamIndex, BGCHECK_SCENE);
}

/**
 * Gets the bgCam index for the poly `poly`, returns -1 if
 * there is no camera data for that poly.
 */
s32 Camera_GetBgCamIndex(Camera* camera, s32* bgId, CollisionPoly* poly) {
    s32 bgCamIndex;
    PosRot playerPosRot;
    s32 ret;

    Actor_GetWorldPosShapeRot(&playerPosRot, &camera->player->actor); // unused.
    bgCamIndex = SurfaceType_GetBgCamIndex(&camera->play->colCtx, poly, *bgId);

    if (BgCheck_GetBgCamSettingImpl(&camera->play->colCtx, bgCamIndex, *bgId) == CAM_SET_NONE) {
        ret = -1;
    } else {
        ret = bgCamIndex;
    }
    return ret;
}

/**
 * Returns the bgCamFuncData for the floor under the player.
 * Also returns the number of pieces of data there are in `bgCamCount`.
 * If there is no floor, then return NULL
 */
Vec3s* Camera_GetBgCamFuncDataUnderPlayer(Camera* camera, u16* bgCamCount) {
    CollisionPoly* floorPoly;
    s32 pad;
    s32 bgId;
    PosRot playerPosShape;

    Actor_GetWorldPosShapeRot(&playerPosShape, &camera->player->actor);
    playerPosShape.pos.y += Player_GetHeight(camera->player);

    if (BgCheck_EntityRaycastDown3(&camera->play->colCtx, &floorPoly, &bgId, &playerPosShape.pos) == BGCHECK_Y_MIN) {
        // no floor
        return NULL;
    }

    *bgCamCount = BgCheck_GetBgCamCount(&camera->play->colCtx, floorPoly, bgId);
    return BgCheck_GetBgCamFuncData(&camera->play->colCtx, floorPoly, bgId);
}

/**
 * Gets the Camera information for the water box the player is in.
 * Returns -1 if the player is not in a water box, or does not have a swimming state.
 * Returns -2 if there is no camera index for the water box.
 * Returns the camera data index otherwise.
 */
s32 Camera_GetWaterBoxBgCamIndex(Camera* camera, f32* waterY) {
    PosRot playerPosShape;
    WaterBox* waterBox;
    s32 bgCamIndex;

    Actor_GetWorldPosShapeRot(&playerPosShape, &camera->player->actor);
    *waterY = playerPosShape.pos.y;

    if (!WaterBox_GetSurface1(camera->play, &camera->play->colCtx, playerPosShape.pos.x, playerPosShape.pos.z, waterY,
                              &waterBox)) {
        // player's position is not in a water box.
        *waterY = BGCHECK_Y_MIN;
        return -1;
    }

    if (!(camera->player->stateFlags1 & PLAYER_STATE1_27)) {
        // player is not swimming
        *waterY = BGCHECK_Y_MIN;
        return -1;
    }

    bgCamIndex = WaterBox_GetBgCamIndex(&camera->play->colCtx, waterBox);

    //! @bug bgCamIndex = 0 is a valid index, should be (bgCamIndex < 0)
    if ((bgCamIndex <= 0) || (WaterBox_GetBgCamSetting(&camera->play->colCtx, waterBox) <= CAM_SET_NONE)) {
        // no camera data index, or no CameraSettingType
        return -2;
    }

    return bgCamIndex;
}

/**
 * Checks if `chkPos` is inside a waterbox.
 * If there is no water box below `chkPos` or if `chkPos` is above the water surface, return BGCHECK_Y_MIN.
 * If `chkPos` is inside the waterbox, output light index to `lightIndex`.
 */
f32 Camera_GetWaterSurface(Camera* camera, Vec3f* chkPos, s32* lightIndex) {
    PosRot playerPosRot;
    f32 waterY;
    WaterBox* waterBox;

    Actor_GetWorldPosShapeRot(&playerPosRot, &camera->player->actor);
    waterY = playerPosRot.pos.y;

    if (!WaterBox_GetSurface1(camera->play, &camera->play->colCtx, chkPos->x, chkPos->z, &waterY, &waterBox)) {
        // chkPos is not within the x/z boundaries of a water box.
        return BGCHECK_Y_MIN;
    }

    if (waterY < chkPos->y) {
        // the water's y position is below the check position
        // meaning the position is NOT in the water.
        return BGCHECK_Y_MIN;
    }

    *lightIndex = WaterBox_GetLightIndex(&camera->play->colCtx, waterBox);
    return waterY;
}

/**
 * Calculates the angle between points `from` and `to`
 */
s16 Camera_XZAngle(Vec3f* to, Vec3f* from) {
    return CAM_DEG_TO_BINANG(RAD_TO_DEG(Math_FAtan2F(from->x - to->x, from->z - to->z)));
}

s16 Camera_GetPitchAdjFromFloorHeightDiffs(Camera* camera, s16 viewYaw, s16 initAndReturnZero) {
    static f32 sFloorYNear;
    static f32 sFloorYFar;
    static CamColChk sFarColChk;
    Vec3f playerPos;
    Vec3f nearPos;
    Vec3f floorNorm;
    f32 checkOffsetY;
    s16 pitchNear;
    s16 pitchFar;
    f32 floorYDiffFar;
    f32 viewForwardsUnitX;
    f32 viewForwardsUnitZ;
    s32 bgId;
    f32 nearDist;
    f32 farDist;
    f32 floorYDiffNear;
    f32 playerHeight;

    viewForwardsUnitX = Math_SinS(viewYaw);
    viewForwardsUnitZ = Math_CosS(viewYaw);

    playerHeight = Player_GetHeight(camera->player);
    checkOffsetY = CAM_DATA_SCALED(R_CAM_PITCH_FLOOR_CHECK_OFFSET_Y_FAC) * playerHeight;
    nearDist = CAM_DATA_SCALED(R_CAM_PITCH_FLOOR_CHECK_NEAR_DIST_FAC) * playerHeight;
    farDist = CAM_DATA_SCALED(R_CAM_PITCH_FLOOR_CHECK_FAR_DIST_FAC) * playerHeight;

    playerPos.x = camera->playerPosRot.pos.x;
    playerPos.y = camera->playerGroundY + checkOffsetY;
    playerPos.z = camera->playerPosRot.pos.z;

    nearPos.x = playerPos.x + (nearDist * viewForwardsUnitX);
    nearPos.y = playerPos.y;
    nearPos.z = playerPos.z + (nearDist * viewForwardsUnitZ);

    if (initAndReturnZero || (camera->play->state.frames % 2) == 0) {
        sFarColChk.pos.x = playerPos.x + (farDist * viewForwardsUnitX);
        sFarColChk.pos.y = playerPos.y;
        sFarColChk.pos.z = playerPos.z + (farDist * viewForwardsUnitZ);

        Camera_BGCheckInfo(camera, &playerPos, &sFarColChk);

        if (initAndReturnZero) {
            sFloorYNear = sFloorYFar = camera->playerGroundY;
        }
    } else {
        farDist = OLib_Vec3fDistXZ(&playerPos, &sFarColChk.pos);

        sFarColChk.pos.x += sFarColChk.norm.x * 5.0f;
        sFarColChk.pos.y += sFarColChk.norm.y * 5.0f;
        sFarColChk.pos.z += sFarColChk.norm.z * 5.0f;

        if (nearDist > farDist) {
            nearDist = farDist;
            sFloorYNear = sFloorYFar = Camera_GetFloorYLayer(camera, &floorNorm, &sFarColChk.pos, &bgId);
        } else {
            sFloorYNear = Camera_GetFloorYLayer(camera, &floorNorm, &nearPos, &bgId);
            sFloorYFar = Camera_GetFloorYLayer(camera, &floorNorm, &sFarColChk.pos, &bgId);
        }

        if (sFloorYNear == BGCHECK_Y_MIN) {
            sFloorYNear = camera->playerGroundY;
        }

        if (sFloorYFar == BGCHECK_Y_MIN) {
            sFloorYFar = sFloorYNear;
        }
    }

    floorYDiffNear = CAM_DATA_SCALED(R_CAM_PITCH_FLOOR_CHECK_NEAR_WEIGHT) * (sFloorYNear - camera->playerGroundY);
    floorYDiffFar =
        (1.0f - CAM_DATA_SCALED(R_CAM_PITCH_FLOOR_CHECK_NEAR_WEIGHT)) * (sFloorYFar - camera->playerGroundY);

    pitchNear = CAM_DEG_TO_BINANG(RAD_TO_DEG(Math_FAtan2F(floorYDiffNear, nearDist)));
    pitchFar = CAM_DEG_TO_BINANG(RAD_TO_DEG(Math_FAtan2F(floorYDiffFar, farDist)));

    return pitchNear + pitchFar;
}

/**
 * Calculates a new Up vector from the pitch, yaw, roll
 */
Vec3f* Camera_CalcUpFromPitchYawRoll(Vec3f* viewUp, s16 pitch, s16 yaw, s16 roll) {
    f32 sinP = Math_SinS(pitch);
    f32 cosP = Math_CosS(pitch);
    f32 sinY = Math_SinS(yaw);
    f32 cosY = Math_CosS(yaw);
    f32 sinR = Math_SinS(-roll);
    f32 cosR = Math_CosS(-roll);
    Vec3f up;
    Vec3f baseUp;
    Vec3f u;
    Vec3f rollMtxRow1;
    Vec3f rollMtxRow2;
    Vec3f rollMtxRow3;
    f32 pad;

    // Axis to roll around
    u.x = cosP * sinY;
    u.y = sinP;
    u.z = cosP * cosY;

    // Matrix to apply the roll to the Up vector without roll
    rollMtxRow1.x = ((1.0f - SQ(u.x)) * cosR) + SQ(u.x);
    rollMtxRow1.y = ((u.x * u.y) * (1.0f - cosR)) - (u.z * sinR);
    rollMtxRow1.z = ((u.z * u.x) * (1.0f - cosR)) + (u.y * sinR);

    rollMtxRow2.x = ((u.x * u.y) * (1.0f - cosR)) + (u.z * sinR);
    rollMtxRow2.y = ((1.0f - SQ(u.y)) * cosR) + SQ(u.y);
    rollMtxRow2.z = ((u.y * u.z) * (1.0f - cosR)) - (u.x * sinR);

    rollMtxRow3.x = ((u.z * u.x) * (1.0f - cosR)) - (u.y * sinR);
    rollMtxRow3.y = ((u.y * u.z) * (1.0f - cosR)) + (u.x * sinR);
    rollMtxRow3.z = ((1.0f - SQ(u.z)) * cosR) + SQ(u.z);

    // Up without roll
    baseUp.x = -sinP * sinY;
    baseUp.y = cosP;
    baseUp.z = -sinP * cosY;

    // rollMtx * baseUp
    up.x = DOTXYZ(baseUp, rollMtxRow1);
    up.y = DOTXYZ(baseUp, rollMtxRow2);
    up.z = DOTXYZ(baseUp, rollMtxRow3);

    *viewUp = up;

    return viewUp;
}

f32 Camera_ClampLERPScale(Camera* camera, f32 maxLERPScale) {
    f32 ret;

    if (camera->atLERPStepScale < CAM_DATA_SCALED(R_CAM_AT_LERP_STEP_SCALE_MIN)) {
        ret = CAM_DATA_SCALED(R_CAM_AT_LERP_STEP_SCALE_MIN);
    } else if (camera->atLERPStepScale >= maxLERPScale) {
        ret = maxLERPScale;
    } else {
        ret = CAM_DATA_SCALED(R_CAM_AT_LERP_STEP_SCALE_FAC) * camera->atLERPStepScale;
    }

    return ret;
}

void Camera_CopyDataToRegs(Camera* camera, s16 mode) {
    CameraModeValue* values;
    CameraModeValue* valueP;
    s32 i;

    if (PREG(82)) {
        osSyncPrintf("camera: res: stat (%d/%d/%d)\n", camera->camId, camera->setting, mode);
    }

    values = sCameraSettings[camera->setting].cameraModes[mode].values;

    for (i = 0; i < sCameraSettings[camera->setting].cameraModes[mode].valueCnt; i++) {
        valueP = &values[i];
        PREG(valueP->dataType) = valueP->val;
        if (PREG(82)) {
            osSyncPrintf("camera: res: PREG(%02d) = %d\n", valueP->dataType, valueP->val);
        }
    }
    camera->animState = 0;
}

s32 Camera_CopyPREGToModeValues(Camera* camera) {
    CameraModeValue* values = sCameraSettings[camera->setting].cameraModes[camera->mode].values;
    CameraModeValue* valueP;
    s32 i;

    for (i = 0; i < sCameraSettings[camera->setting].cameraModes[camera->mode].valueCnt; i++) {
        valueP = &values[i];
        valueP->val = R_CAM_DATA(valueP->dataType);
        if (PREG(82)) {
            osSyncPrintf("camera: res: %d = PREG(%02d)\n", valueP->val, valueP->dataType);
        }
    }
    return true;
}

void Camera_UpdateInterface(s16 interfaceField) {
    s16 hudVisibilityMode;

    if ((interfaceField & CAM_LETTERBOX_MASK) != CAM_LETTERBOX_IGNORE) {
        switch (interfaceField & CAM_LETTERBOX_SIZE_MASK) {
            case CAM_LETTERBOX_SMALL:
                sCameraLetterboxSize = 26;
                break;

            case CAM_LETTERBOX_MEDIUM:
                sCameraLetterboxSize = 27;
                break;

            case CAM_LETTERBOX_LARGE:
                sCameraLetterboxSize = 32;
                break;

            default:
                sCameraLetterboxSize = 0;
                break;
        }

        if (interfaceField & CAM_LETTERBOX_INSTANT) {
            Letterbox_SetSize(sCameraLetterboxSize);
        } else {
            Letterbox_SetSizeTarget(sCameraLetterboxSize);
        }
    }

    if ((interfaceField & CAM_HUD_VISIBILITY_MASK) != CAM_HUD_VISIBILITY_IGNORE) {
        hudVisibilityMode = (interfaceField & CAM_HUD_VISIBILITY_MASK) >> CAM_HUD_VISIBILITY_SHIFT;
        if (hudVisibilityMode == (CAM_HUD_VISIBILITY_ALL >> CAM_HUD_VISIBILITY_SHIFT)) {
            hudVisibilityMode = HUD_VISIBILITY_ALL;
        }
        if (sCameraHudVisibilityMode != hudVisibilityMode) {
            sCameraHudVisibilityMode = hudVisibilityMode;
            Interface_ChangeHudVisibilityMode(sCameraHudVisibilityMode);
        }
    }
}

Vec3f* Camera_BGCheckCorner(Vec3f* dst, Vec3f* linePointA, Vec3f* linePointB, CamColChk* pointAColChk,
                            CamColChk* pointBColChk) {
    Vec3f closestPoint;

    if (!func_800427B4(pointAColChk->poly, pointBColChk->poly, linePointA, linePointB, &closestPoint)) {
        osSyncPrintf(VT_COL(YELLOW, BLACK) "camera: corner check no cross point %x %x\n" VT_RST, pointAColChk,
                     pointBColChk);
        *dst = pointAColChk->pos;
        return dst;
    }

    *dst = closestPoint;
    return dst;
}

/**
 * Checks collision between at and eyeNext, if `checkEye` is set, if there is no collision between
 * eyeNext->at, then eye->at is also checked.
 * Returns:
 * 0 if no collision is found between at->eyeNext
 * 2 if the angle between the polys is between 60 degrees and 120 degrees
 * 3 ?
 * 6 if the angle between the polys is greater than 120 degrees
 */
s32 func_80045508(Camera* camera, VecGeo* diffGeo, CamColChk* eyeChk, CamColChk* atChk, s16 checkEye) {
    Vec3f* at = &camera->at;
    Vec3f* eye = &camera->eye;
    Vec3f* eyeNext = &camera->eyeNext;
    Vec3f eyePos;
    s32 atEyeBgId;
    s32 eyeAtBgId;
    s32 ret;
    f32 cosEyeAt;

    eyeChk->pos = camera->eyeNext;

    ret = 0;

    atEyeBgId = Camera_BGCheckInfo(camera, at, eyeChk);
    if (atEyeBgId != 0) {
        // collision found between at->eye
        atChk->pos = camera->at;

        OLib_Vec3fToVecGeo(&eyeChk->geoNorm, &eyeChk->norm);

        if (eyeChk->geoNorm.pitch >= 0x2EE1) {
            eyeChk->geoNorm.yaw = diffGeo->yaw;
        }

        eyeAtBgId = Camera_BGCheckInfo(camera, eyeNext, atChk);

        if (eyeAtBgId == 0) {
            // no collision from eyeNext->at
            if (checkEye & 1) {

                atChk->pos = *at;
                eyePos = *eye;

                if (Camera_BGCheckInfo(camera, &eyePos, atChk) == 0) {
                    // no collision from eye->at
                    return 3;
                } else if (eyeChk->poly == atChk->poly) {
                    // at->eye and eye->at is the same poly
                    return 3;
                }
            } else {
                return 3;
            }
        } else if (eyeChk->poly == atChk->poly) {
            // at->eyeNext and eyeNext->at is the same poly
            return 3;
        }

        OLib_Vec3fToVecGeo(&atChk->geoNorm, &atChk->norm);

        if (atChk->geoNorm.pitch >= 0x2EE1) {
            atChk->geoNorm.yaw = diffGeo->yaw - 0x7FFF;
        }

        if (atEyeBgId != eyeAtBgId) {
            // different bgIds for at->eye[Next] and eye[Next]->at
            ret = 3;
        } else {
            cosEyeAt = Math3D_Cos(&eyeChk->norm, &atChk->norm);
            if (cosEyeAt < -0.5f) {
                ret = 6;
            } else if (cosEyeAt > 0.5f) {
                ret = 3;
            } else {
                ret = 2;
            }
        }
    }
    return ret;
}

/**
 * Calculates how much to adjust the camera at's y value when on a slope.
 */
f32 Camera_CalcSlopeYAdj(Vec3f* floorNorm, s16 playerYRot, s16 eyeAtYaw, f32 adjAmt) {
    f32 tmp;
    VecGeo floorNormGeo;

    OLib_Vec3fToVecGeo(&floorNormGeo, floorNorm);

    tmp = Math_CosS(floorNormGeo.pitch) * Math_CosS(playerYRot - floorNormGeo.yaw);
    return (fabsf(tmp) * adjAmt) * Math_CosS(playerYRot - eyeAtYaw);
}

/**
 * Calculates new at vector for the camera pointing in `eyeAtDir`
 */
s32 Camera_CalcAtDefault(Camera* camera, VecGeo* eyeAtDir, f32 extraYOffset, s16 calcSlope) {
    Vec3f* at = &camera->at;
    Vec3f posOffsetTarget;
    Vec3f atTarget;
    s32 pad2;
    PosRot* playerPosRot = &camera->playerPosRot;
    f32 yOffset;

    yOffset = Player_GetHeight(camera->player);

    posOffsetTarget.x = 0.f;
    posOffsetTarget.y = yOffset + extraYOffset;
    posOffsetTarget.z = 0.f;

    if (calcSlope) {
        posOffsetTarget.y -= OLib_ClampMaxDist(
            Camera_CalcSlopeYAdj(&camera->floorNorm, playerPosRot->rot.y, eyeAtDir->yaw, R_CAM_SLOPE_Y_ADJ_AMOUNT),
            yOffset);
    }

    Camera_LERPCeilVec3f(&posOffsetTarget, &camera->posOffset, camera->yOffsetUpdateRate, camera->xzOffsetUpdateRate,
                         0.1f);

    atTarget.x = playerPosRot->pos.x + camera->posOffset.x;
    atTarget.y = playerPosRot->pos.y + camera->posOffset.y;
    atTarget.z = playerPosRot->pos.z + camera->posOffset.z;

    Camera_LERPCeilVec3f(&atTarget, at, camera->atLERPStepScale, camera->atLERPStepScale, 0.2f);

    return true;
}

s32 func_800458D4(Camera* camera, VecGeo* eyeAtDir, f32 arg2, f32* arg3, s16 arg4) {
    f32 phi_f2;
    Vec3f posOffsetTarget;
    Vec3f atTarget;
    f32 eyeAtAngle;
    PosRot* playerPosRot = &camera->playerPosRot;
    f32 deltaY;
    s32 pad[2];

    posOffsetTarget.y = Player_GetHeight(camera->player) + arg2;
    posOffsetTarget.x = 0.0f;
    posOffsetTarget.z = 0.0f;

    if (arg4) {
        posOffsetTarget.y -=
            Camera_CalcSlopeYAdj(&camera->floorNorm, playerPosRot->rot.y, eyeAtDir->yaw, R_CAM_SLOPE_Y_ADJ_AMOUNT);
    }

    deltaY = playerPosRot->pos.y - *arg3;
    eyeAtAngle = Math_FAtan2F(deltaY, OLib_Vec3fDistXZ(&camera->at, &camera->eye));

    if (eyeAtAngle > DEG_TO_RAD(OREG(32))) {
        if (1) {}
        phi_f2 = 1.0f - sinf(eyeAtAngle - DEG_TO_RAD(OREG(32)));
    } else if (eyeAtAngle < DEG_TO_RAD(OREG(33))) {
        phi_f2 = 1.0f - sinf(DEG_TO_RAD(OREG(33)) - eyeAtAngle);
    } else {
        phi_f2 = 1.0f;
    }

    posOffsetTarget.y -= deltaY * phi_f2;
    Camera_LERPCeilVec3f(&posOffsetTarget, &camera->posOffset, CAM_DATA_SCALED(OREG(29)), CAM_DATA_SCALED(OREG(30)),
                         0.1f);

    atTarget.x = playerPosRot->pos.x + camera->posOffset.x;
    atTarget.y = playerPosRot->pos.y + camera->posOffset.y;
    atTarget.z = playerPosRot->pos.z + camera->posOffset.z;

    Camera_LERPCeilVec3f(&atTarget, &camera->at, camera->atLERPStepScale, camera->atLERPStepScale, 0.2f);

    return 1;
}

s32 func_80045B08(Camera* camera, VecGeo* eyeAtDir, f32 yExtra, s16 arg3) {
    f32 phi_f2;
    Vec3f posOffsetTarget;
    Vec3f atTarget;
    f32 pad;
    f32 temp_ret;
    PosRot* playerPosRot = &camera->playerPosRot;

    posOffsetTarget.y = Player_GetHeight(camera->player) + yExtra;
    posOffsetTarget.x = 0.0f;
    posOffsetTarget.z = 0.0f;

    temp_ret = Math_SinS(arg3);

    if (temp_ret < 0.0f) {
        phi_f2 = Math_CosS(playerPosRot->rot.y - eyeAtDir->yaw);
    } else {
        phi_f2 = -Math_CosS(playerPosRot->rot.y - eyeAtDir->yaw);
    }

    posOffsetTarget.y -= temp_ret * phi_f2 * R_CAM_SLOPE_Y_ADJ_AMOUNT;
    Camera_LERPCeilVec3f(&posOffsetTarget, &camera->posOffset, camera->yOffsetUpdateRate, camera->xzOffsetUpdateRate,
                         0.1f);

    atTarget.x = playerPosRot->pos.x + camera->posOffset.x;
    atTarget.y = playerPosRot->pos.y + camera->posOffset.y;
    atTarget.z = playerPosRot->pos.z + camera->posOffset.z;
    Camera_LERPCeilVec3f(&atTarget, &camera->at, camera->atLERPStepScale, camera->atLERPStepScale, 0.2f);

    return 1;
}

/**
 * Adjusts the camera's at position for Camera_Parallel1
 */
s32 Camera_CalcAtForParallel(Camera* camera, VecGeo* arg1, f32 yOffset, f32* arg3, s16 arg4) {
    Vec3f* at = &camera->at;
    Vec3f posOffsetTarget;
    Vec3f atTarget;
    Vec3f* eye = &camera->eye;
    PosRot* playerPosRot = &camera->playerPosRot;
    f32 temp_f2;
    f32 phi_f16;
    f32 eyeAtDistXZ;
    f32 phi_f20;
    f32 temp_f0_4;

    temp_f0_4 = Player_GetHeight(camera->player);
    posOffsetTarget.x = 0.0f;
    posOffsetTarget.y = temp_f0_4 + yOffset;
    posOffsetTarget.z = 0.0f;

    if (PREG(76) && arg4) {
        posOffsetTarget.y -=
            Camera_CalcSlopeYAdj(&camera->floorNorm, playerPosRot->rot.y, arg1->yaw, R_CAM_SLOPE_Y_ADJ_AMOUNT);
    }

    if (camera->playerGroundY == camera->playerPosRot.pos.y || camera->player->actor.gravity > -0.1f ||
        camera->player->stateFlags1 & PLAYER_STATE1_21) {
        *arg3 = Camera_LERPCeilF(playerPosRot->pos.y, *arg3, CAM_DATA_SCALED(OREG(43)), 0.1f);
        phi_f20 = playerPosRot->pos.y - *arg3;
        posOffsetTarget.y -= phi_f20;
        Camera_LERPCeilVec3f(&posOffsetTarget, &camera->posOffset, camera->yOffsetUpdateRate,
                             camera->xzOffsetUpdateRate, 0.1f);
    } else {
        if (!PREG(75)) {
            phi_f20 = playerPosRot->pos.y - *arg3;
            eyeAtDistXZ = OLib_Vec3fDistXZ(at, &camera->eye);
            phi_f16 = eyeAtDistXZ;
            Math_FAtan2F(phi_f20, eyeAtDistXZ);
            temp_f2 = Math_FTanF(DEG_TO_RAD(camera->fov * 0.4f)) * phi_f16;
            if (temp_f2 < phi_f20) {
                *arg3 += phi_f20 - temp_f2;
                phi_f20 = temp_f2;
            } else if (phi_f20 < -temp_f2) {
                *arg3 += phi_f20 + temp_f2;
                phi_f20 = -temp_f2;
            }
            posOffsetTarget.y -= phi_f20;
        } else {
            phi_f20 = playerPosRot->pos.y - *arg3;
            temp_f2 = Math_FAtan2F(phi_f20, OLib_Vec3fDistXZ(at, eye));
            if (DEG_TO_RAD(OREG(32)) < temp_f2) {
                phi_f16 = 1 - sinf(temp_f2 - DEG_TO_RAD(OREG(32)));
            } else if (temp_f2 < DEG_TO_RAD(OREG(33))) {
                phi_f16 = 1 - sinf(DEG_TO_RAD(OREG(33)) - temp_f2);
            } else {
                phi_f16 = 1;
            }
            posOffsetTarget.y -= phi_f20 * phi_f16;
        }
        Camera_LERPCeilVec3f(&posOffsetTarget, &camera->posOffset, CAM_DATA_SCALED(OREG(29)), CAM_DATA_SCALED(OREG(30)),
                             0.1f);
        camera->yOffsetUpdateRate = CAM_DATA_SCALED(OREG(29));
        camera->xzOffsetUpdateRate = CAM_DATA_SCALED(OREG(30));
    }
    atTarget.x = playerPosRot->pos.x + camera->posOffset.x;
    atTarget.y = playerPosRot->pos.y + camera->posOffset.y;
    atTarget.z = playerPosRot->pos.z + camera->posOffset.z;
    Camera_LERPCeilVec3f(&atTarget, at, camera->atLERPStepScale, camera->atLERPStepScale, 0.2f);
    return 1;
}

/**
 * Adjusts at position for Camera_Battle1 and Camera_KeepOn1
 */
s32 Camera_CalcAtForLockOn(Camera* camera, VecGeo* eyeAtDir, Vec3f* targetPos, f32 yOffset, f32 distance,
                           f32* yPosOffset, VecGeo* outPlayerToTargetDir, s16 flags) {
    Vec3f* at = &camera->at;
    Vec3f tmpPos0;
    Vec3f tmpPos1;
    Vec3f lookFromOffset;
    Vec3f* floorNorm = &camera->floorNorm;
    VecGeo playerToTargetDir;
    PosRot* playerPosRot = &camera->playerPosRot;
    f32 yPosDelta;
    f32 phi_f16;
    f32 eyeAtDistXZ;
    f32 temp_f0_2;
    f32 playerHeight;

    playerHeight = Player_GetHeight(camera->player);
    tmpPos0.x = 0.0f;
    tmpPos0.y = playerHeight + yOffset;
    tmpPos0.z = 0.0f;
    if (PREG(76) && (flags & FLG_ADJSLOPE)) {
        tmpPos0.y -= Camera_CalcSlopeYAdj(floorNorm, playerPosRot->rot.y, eyeAtDir->yaw, R_CAM_SLOPE_Y_ADJ_AMOUNT);
    }

    // tmpPos1 is player's head
    tmpPos1 = playerPosRot->pos;
    tmpPos1.y += playerHeight;
    OLib_Vec3fDiffToVecGeo(outPlayerToTargetDir, &tmpPos1, targetPos);
    playerToTargetDir = *outPlayerToTargetDir;
    if (distance < playerToTargetDir.r) {
        playerToTargetDir.r = playerToTargetDir.r * CAM_DATA_SCALED(OREG(38));
    } else {
        // ratio of player's height off ground to player's height.
        temp_f0_2 = OLib_ClampMaxDist((playerPosRot->pos.y - camera->playerGroundY) / playerHeight, 1.0f);
        playerToTargetDir.r = (playerToTargetDir.r * CAM_DATA_SCALED(OREG(39))) -
                              (((CAM_DATA_SCALED(OREG(39)) - CAM_DATA_SCALED(OREG(38))) * playerToTargetDir.r) *
                               (playerToTargetDir.r / distance));
        playerToTargetDir.r = playerToTargetDir.r - (playerToTargetDir.r * temp_f0_2) * temp_f0_2;
    }

    if (flags & FLG_OFFGROUND) {
        playerToTargetDir.r *= 0.2f;
        camera->xzOffsetUpdateRate = camera->yOffsetUpdateRate = .01f;
    }

    OLib_VecGeoToVec3f(&lookFromOffset, &playerToTargetDir);

    if (PREG(89)) {
        osSyncPrintf("%f (%f %f %f) %f\n", playerToTargetDir.r / distance, lookFromOffset.x, lookFromOffset.y,
                     lookFromOffset.z, camera->atLERPStepScale);
    }

    tmpPos0.x = tmpPos0.x + lookFromOffset.x;
    tmpPos0.y = tmpPos0.y + lookFromOffset.y;
    tmpPos0.z = tmpPos0.z + lookFromOffset.z;

    if (camera->playerGroundY == camera->playerPosRot.pos.y || camera->player->actor.gravity > -0.1f ||
        camera->player->stateFlags1 & PLAYER_STATE1_21) {
        *yPosOffset = Camera_LERPCeilF(playerPosRot->pos.y, *yPosOffset, CAM_DATA_SCALED(OREG(43)), 0.1f);
        yPosDelta = playerPosRot->pos.y - *yPosOffset;
        tmpPos0.y -= yPosDelta;
        Camera_LERPCeilVec3f(&tmpPos0, &camera->posOffset, camera->yOffsetUpdateRate, camera->xzOffsetUpdateRate, 0.1f);
    } else {
        if (!(flags & FLG_OFFGROUND)) {
            yPosDelta = playerPosRot->pos.y - *yPosOffset;
            eyeAtDistXZ = OLib_Vec3fDistXZ(at, &camera->eye);
            phi_f16 = eyeAtDistXZ;
            Math_FAtan2F(yPosDelta, eyeAtDistXZ);
            temp_f0_2 = Math_FTanF(DEG_TO_RAD(camera->fov * 0.4f)) * phi_f16;
            if (temp_f0_2 < yPosDelta) {
                *yPosOffset = *yPosOffset + (yPosDelta - temp_f0_2);
                yPosDelta = temp_f0_2;
            } else if (yPosDelta < -temp_f0_2) {
                *yPosOffset = *yPosOffset + (yPosDelta + temp_f0_2);
                yPosDelta = -temp_f0_2;
            }
            tmpPos0.y = tmpPos0.y - yPosDelta;
        } else {
            yPosDelta = playerPosRot->pos.y - *yPosOffset;
            temp_f0_2 = Math_FAtan2F(yPosDelta, OLib_Vec3fDistXZ(at, &camera->eye));

            if (temp_f0_2 > DEG_TO_RAD(OREG(32))) {
                phi_f16 = 1.0f - sinf(temp_f0_2 - DEG_TO_RAD(OREG(32)));
            } else if (temp_f0_2 < DEG_TO_RAD(OREG(33))) {
                phi_f16 = 1.0f - sinf(DEG_TO_RAD(OREG(33)) - temp_f0_2);
            } else {
                phi_f16 = 1.0f;
            }
            tmpPos0.y -= (yPosDelta * phi_f16);
        }

        Camera_LERPCeilVec3f(&tmpPos0, &camera->posOffset, CAM_DATA_SCALED(OREG(29)), CAM_DATA_SCALED(OREG(30)), 0.1f);
        camera->yOffsetUpdateRate = CAM_DATA_SCALED(OREG(29));
        camera->xzOffsetUpdateRate = CAM_DATA_SCALED(OREG(30));
    }

    tmpPos1.x = playerPosRot->pos.x + camera->posOffset.x;
    tmpPos1.y = playerPosRot->pos.y + camera->posOffset.y;
    tmpPos1.z = playerPosRot->pos.z + camera->posOffset.z;
    Camera_LERPCeilVec3f(&tmpPos1, at, camera->atLERPStepScale, camera->atLERPStepScale, 0.2f);
    return 1;
}

s32 Camera_CalcAtForHorse(Camera* camera, VecGeo* eyeAtDir, f32 yOffset, f32* yPosOffset, s16 calcSlope) {
    Vec3f* at = &camera->at;
    Vec3f posOffsetTarget;
    Vec3f atTarget;
    s32 pad;
    s32 pad2;
    f32 playerHeight;
    Player* player;
    PosRot horsePosRot;

    playerHeight = Player_GetHeight(camera->player);
    player = camera->player;
    Actor_GetWorldPosShapeRot(&horsePosRot, player->rideActor);

    if (EN_HORSE_CHECK_JUMPING((EnHorse*)player->rideActor)) {
        horsePosRot.pos.y -= 49.f;
        *yPosOffset = Camera_LERPCeilF(horsePosRot.pos.y, *yPosOffset, 0.1f, 0.2f);
        camera->atLERPStepScale = Camera_LERPCeilF(0.4f, camera->atLERPStepScale, 0.2f, 0.02f);
    } else {
        *yPosOffset = Camera_LERPCeilF(horsePosRot.pos.y, *yPosOffset, 0.5f, 0.2f);
    }

    posOffsetTarget.x = 0.0f;
    posOffsetTarget.y = playerHeight + yOffset;
    posOffsetTarget.z = 0.0f;

    if (calcSlope != 0) {
        posOffsetTarget.y -= Camera_CalcSlopeYAdj(&camera->floorNorm, camera->playerPosRot.rot.y, eyeAtDir->yaw,
                                                  R_CAM_SLOPE_Y_ADJ_AMOUNT);
    }

    Camera_LERPCeilVec3f(&posOffsetTarget, &camera->posOffset, camera->yOffsetUpdateRate, camera->xzOffsetUpdateRate,
                         0.1f);

    atTarget.x = camera->posOffset.x + horsePosRot.pos.x;
    atTarget.y = camera->posOffset.y + horsePosRot.pos.y;
    atTarget.z = camera->posOffset.z + horsePosRot.pos.z;
    Camera_LERPCeilVec3f(&atTarget, at, camera->atLERPStepScale, camera->atLERPStepScale, 0.2f);

    return 1;
}

f32 Camera_LERPClampDist(Camera* camera, f32 dist, f32 min, f32 max) {
    f32 distTarget;
    f32 rUpdateRateInvTarget;

    if (dist < min) {
        distTarget = min;
        rUpdateRateInvTarget = R_CAM_R_UPDATE_RATE_INV;
    } else if (dist > max) {
        distTarget = max;
        rUpdateRateInvTarget = R_CAM_R_UPDATE_RATE_INV;
    } else {
        distTarget = dist;
        rUpdateRateInvTarget = 1.0f;
    }

    camera->rUpdateRateInv = Camera_LERPCeilF(rUpdateRateInvTarget, camera->rUpdateRateInv,
                                              CAM_DATA_SCALED(R_CAM_UPDATE_RATE_STEP_SCALE_XZ), 0.1f);
    return Camera_LERPCeilF(distTarget, camera->dist, 1.0f / camera->rUpdateRateInv, 0.2f);
}

f32 Camera_ClampDist(Camera* camera, f32 dist, f32 minDist, f32 maxDist, s16 timer) {
    f32 distTarget;
    f32 rUpdateRateInvTarget;

    if (dist < minDist) {
        distTarget = minDist;

        rUpdateRateInvTarget = timer != 0 ? R_CAM_R_UPDATE_RATE_INV * 0.5f : R_CAM_R_UPDATE_RATE_INV;
    } else if (maxDist < dist) {
        distTarget = maxDist;

        rUpdateRateInvTarget = timer != 0 ? R_CAM_R_UPDATE_RATE_INV * 0.5f : R_CAM_R_UPDATE_RATE_INV;
    } else {
        distTarget = dist;

        rUpdateRateInvTarget = timer != 0 ? R_CAM_R_UPDATE_RATE_INV : 1.0f;
    }

    camera->rUpdateRateInv = Camera_LERPCeilF(rUpdateRateInvTarget, camera->rUpdateRateInv,
                                              CAM_DATA_SCALED(R_CAM_UPDATE_RATE_STEP_SCALE_XZ), 0.1f);
    return Camera_LERPCeilF(distTarget, camera->dist, 1.0f / camera->rUpdateRateInv, 0.2f);
}

s16 Camera_CalcDefaultPitch(Camera* camera, s16 arg1, s16 arg2, s16 arg3) {
    f32 pad;
    f32 stepScale;
    f32 t;
    s16 phi_v0;
    s16 absCur;
    s16 target;

    absCur = ABS(arg1);
    phi_v0 = arg3 > 0 ? (s16)(Math_CosS(arg3) * arg3) : arg3;
    target = arg2 - phi_v0;

    if (ABS(target) < absCur) {
        stepScale = (1.0f / camera->pitchUpdateRateInv) * 3.0f;
    } else {
        t = absCur * (1.0f / R_CAM_MAX_PITCH);
        pad = Camera_InterpolateCurve(0.8f, 1.0f - t);
        stepScale = (1.0f / camera->pitchUpdateRateInv) * pad;
    }
    return Camera_LERPCeilS(target, arg1, stepScale, 0xA);
}

s16 Camera_CalcDefaultYaw(Camera* camera, s16 cur, s16 target, f32 arg3, f32 accel) {
    f32 velocity;
    s16 angDelta;
    f32 updSpeed;
    f32 speedT;
    f32 velFactor;
    f32 yawUpdRate;

    if (camera->xzSpeed > 0.001f) {
        angDelta = target - (s16)(cur - 0x7FFF);
        speedT = COLPOLY_GET_NORMAL((s16)(angDelta - 0x7FFF));
    } else {
        angDelta = target - (s16)(cur - 0x7FFF);
        speedT = CAM_DATA_SCALED(OREG(48));
    }

    updSpeed = Camera_InterpolateCurve(arg3, speedT);

    velocity = updSpeed + (1.0f - updSpeed) * accel;

    if (velocity < 0.0f) {
        velocity = 0.0f;
    }

    velFactor = Camera_InterpolateCurve(0.5f, camera->speedRatio);
    yawUpdRate = 1.0f / camera->yawUpdateRateInv;
    return cur + (s16)(angDelta * velocity * velFactor * yawUpdRate);
}

void func_80046E20(Camera* camera, VecGeo* eyeAdjustment, f32 minDist, f32 arg3, f32* arg4, SwingAnimation* anim) {
    static CamColChk atEyeColChk;
    static CamColChk eyeAtColChk;
    static CamColChk newEyeColChk;
    Vec3f* eye = &camera->eye;
    s32 temp_v0;
    Vec3f* at = &camera->at;
    Vec3f peekAroundPoint;
    Vec3f* eyeNext = &camera->eyeNext;
    f32 temp_f0;
    VecGeo newEyeAdjustment;
    VecGeo sp40;

    temp_v0 = func_80045508(camera, eyeAdjustment, &atEyeColChk, &eyeAtColChk, !anim->unk_18);

    switch (temp_v0) {
        case 1:
        case 2:
            // angle between polys is between 60 and 120 degrees.
            Camera_BGCheckCorner(&anim->collisionClosePoint, at, eyeNext, &atEyeColChk, &eyeAtColChk);
            peekAroundPoint.x = anim->collisionClosePoint.x + (atEyeColChk.norm.x + eyeAtColChk.norm.x);
            peekAroundPoint.y = anim->collisionClosePoint.y + (atEyeColChk.norm.y + eyeAtColChk.norm.y);
            peekAroundPoint.z = anim->collisionClosePoint.z + (atEyeColChk.norm.z + eyeAtColChk.norm.z);

            temp_f0 = OLib_Vec3fDist(at, &atEyeColChk.pos);
            *arg4 = temp_f0 > minDist ? 1.0f : temp_f0 / minDist;

            anim->swingUpdateRate = CAM_DATA_SCALED(OREG(10));
            anim->unk_18 = 1;
            anim->atEyePoly = eyeAtColChk.poly;
            OLib_Vec3fDiffToVecGeo(&newEyeAdjustment, at, &peekAroundPoint);

            newEyeAdjustment.r = eyeAdjustment->r;
            Camera_AddVecGeoToVec3f(eye, at, &newEyeAdjustment);
            newEyeColChk.pos = *eye;
            if (Camera_BGCheckInfo(camera, at, &newEyeColChk) == 0) {
                // no collision found between at->newEyePos
                newEyeAdjustment.yaw += (s16)(eyeAdjustment->yaw - newEyeAdjustment.yaw) >> 1;
                newEyeAdjustment.pitch += (s16)(eyeAdjustment->pitch - newEyeAdjustment.pitch) >> 1;
                Camera_AddVecGeoToVec3f(eye, at, &newEyeAdjustment);
                if (atEyeColChk.geoNorm.pitch < 0x2AA8) {
                    // ~ 60 degrees
                    anim->unk_16 = newEyeAdjustment.yaw;
                    anim->unk_14 = newEyeAdjustment.pitch;
                } else {
                    anim->unk_16 = eyeAdjustment->yaw;
                    anim->unk_14 = eyeAdjustment->pitch;
                }
                peekAroundPoint.x = anim->collisionClosePoint.x - (atEyeColChk.norm.x + eyeAtColChk.norm.x);
                peekAroundPoint.y = anim->collisionClosePoint.y - (atEyeColChk.norm.y + eyeAtColChk.norm.y);
                peekAroundPoint.z = anim->collisionClosePoint.z - (atEyeColChk.norm.z + eyeAtColChk.norm.z);
                OLib_Vec3fDiffToVecGeo(&newEyeAdjustment, at, &peekAroundPoint);
                newEyeAdjustment.r = eyeAdjustment->r;
                Camera_AddVecGeoToVec3f(eyeNext, at, &newEyeAdjustment);
                break;
            }

            camera->eye = newEyeColChk.pos;
            atEyeColChk = newEyeColChk;
            FALLTHROUGH;
        case 3:
        case 6:
            if (anim->unk_18 != 0) {
                anim->swingUpdateRateTimer = OREG(52);
                anim->unk_18 = 0;
                *eyeNext = *eye;
            }

            temp_f0 = OLib_Vec3fDist(at, &atEyeColChk.pos);
            *arg4 = temp_f0 > minDist ? 1.0f : temp_f0 / minDist;

            anim->swingUpdateRate = *arg4 * arg3;

            Camera_Vec3fTranslateByUnitVector(eye, &atEyeColChk.pos, &atEyeColChk.norm, 1.0f);
            anim->atEyePoly = NULL;
            if (temp_f0 < OREG(21)) {
                sp40.yaw = eyeAdjustment->yaw;
                sp40.pitch = Math_SinS(atEyeColChk.geoNorm.pitch + 0x3FFF) * 16380.0f;
                sp40.r = (OREG(21) - temp_f0) * CAM_DATA_SCALED(OREG(22));
                Camera_AddVecGeoToVec3f(eye, eye, &sp40);
            }
            break;
        default:
            if (anim->unk_18 != 0) {
                anim->swingUpdateRateTimer = OREG(52);
                *eyeNext = *eye;
                anim->unk_18 = 0;
            }
            anim->swingUpdateRate = arg3;
            anim->atEyePoly = NULL;
            eye->x = atEyeColChk.pos.x + atEyeColChk.norm.x;
            eye->y = atEyeColChk.pos.y + atEyeColChk.norm.y;
            eye->z = atEyeColChk.pos.z + atEyeColChk.norm.z;
            break;
    }
}

s32 Camera_Noop(Camera* camera) {
    return true;
}

s32 Camera_Normal1(Camera* camera) {
    Vec3f* eye = &camera->eye;
    Vec3f* at = &camera->at;
    Vec3f* eyeNext = &camera->eyeNext;
    f32 spA0;
    f32 sp9C;
    f32 sp98;
    f32 sp94;
    Vec3f sp88;
    s16 wiggleAdj;
    s16 t;
    VecGeo eyeAdjustment;
    VecGeo atEyeGeo;
    VecGeo atEyeNextGeo;
    PosRot* playerPosRot = &camera->playerPosRot;
    Normal1ReadOnlyData* roData = &camera->paramData.norm1.roData;
    Normal1ReadWriteData* rwData = &camera->paramData.norm1.rwData;
    f32 playerHeight;
    f32 rate = 0.1f;

    playerHeight = Player_GetHeight(camera->player);
    if (RELOAD_PARAMS(camera) || R_RELOAD_CAM_PARAMS) {
        CameraModeValue* values = sCameraSettings[camera->setting].cameraModes[camera->mode].values;
        f32 yNormal =
            (1.0f + CAM_DATA_SCALED(R_CAM_YOFFSET_NORM) - CAM_DATA_SCALED(R_CAM_YOFFSET_NORM) * (68.0f / playerHeight));

        sp94 = yNormal * CAM_DATA_SCALED(playerHeight);

        roData->yOffset = GET_NEXT_RO_DATA(values) * sp94;
        roData->distMin = GET_NEXT_RO_DATA(values) * sp94;
        roData->distMax = GET_NEXT_RO_DATA(values) * sp94;
        roData->pitchTarget = CAM_DEG_TO_BINANG(GET_NEXT_RO_DATA(values));
        roData->unk_0C = GET_NEXT_RO_DATA(values);
        roData->unk_10 = GET_NEXT_RO_DATA(values);
        roData->unk_14 = GET_NEXT_SCALED_RO_DATA(values);
        roData->fovTarget = GET_NEXT_RO_DATA(values);
        roData->atLERPScaleMax = GET_NEXT_SCALED_RO_DATA(values);
        roData->interfaceField = GET_NEXT_RO_DATA(values);
    }

    if (R_RELOAD_CAM_PARAMS) {
        Camera_CopyPREGToModeValues(camera);
    }

    sCameraInterfaceField = roData->interfaceField;

    OLib_Vec3fDiffToVecGeo(&atEyeGeo, at, eye);
    OLib_Vec3fDiffToVecGeo(&atEyeNextGeo, at, eyeNext);

    switch (camera->animState) {
        case 20:
            camera->yawUpdateRateInv = OREG(27);
            camera->pitchUpdateRateInv = OREG(27);
            FALLTHROUGH;
        case 0:
        case 10:
        case 25:
            rwData->swing.atEyePoly = NULL;
            rwData->slopePitchAdj = 0;
            rwData->unk_28 = 0xA;
            rwData->swing.unk_16 = rwData->swing.unk_14 = rwData->swing.unk_18 = 0;
            rwData->swing.swingUpdateRate = roData->unk_0C;
            rwData->yOffset = camera->playerPosRot.pos.y;
            rwData->unk_20 = camera->xzSpeed;
            rwData->swing.swingUpdateRateTimer = 0;
            rwData->swingYawTarget = atEyeGeo.yaw;
            sUpdateCameraDirection = 0;
            rwData->startSwingTimer = OREG(50) + OREG(51);
            break;
        default:
            break;
    }

    camera->animState = 1;
    sUpdateCameraDirection = 1;

    if (rwData->unk_28 != 0) {
        rwData->unk_28--;
    }

    if (camera->xzSpeed > 0.001f) {
        rwData->startSwingTimer = OREG(50) + OREG(51);
    } else if (rwData->startSwingTimer > 0) {
        if (rwData->startSwingTimer > OREG(50)) {
            rwData->swingYawTarget = atEyeGeo.yaw + ((s16)((s16)(camera->playerPosRot.rot.y - 0x7FFF) - atEyeGeo.yaw) /
                                                     rwData->startSwingTimer);
        }
        rwData->startSwingTimer--;
    }

    spA0 = camera->speedRatio * CAM_DATA_SCALED(R_CAM_UPDATE_RATE_STEP_SCALE_XZ);
    sp9C = camera->speedRatio * CAM_DATA_SCALED(R_CAM_UPDATE_RATE_STEP_SCALE_Y);
    sp98 = rwData->swing.unk_18 != 0 ? CAM_DATA_SCALED(R_CAM_UPDATE_RATE_STEP_SCALE_XZ) : spA0;

    sp94 = (camera->xzSpeed - rwData->unk_20) * (0.333333f);
    if (sp94 > 1.0f) {
        sp94 = 1.0f;
    }
    if (sp94 > -1.0f) {
        sp94 = -1.0f;
    }

    rwData->unk_20 = camera->xzSpeed;

    if (rwData->swing.swingUpdateRateTimer != 0) {
        camera->yawUpdateRateInv =
            Camera_LERPCeilF(rwData->swing.swingUpdateRate + (f32)(rwData->swing.swingUpdateRateTimer * 2),
                             camera->yawUpdateRateInv, sp98, rate);
        camera->pitchUpdateRateInv =
            Camera_LERPCeilF((f32)R_CAM_PITCH_UPDATE_RATE_INV + (f32)(rwData->swing.swingUpdateRateTimer * 2),
                             camera->pitchUpdateRateInv, sp9C, rate);
        rwData->swing.swingUpdateRateTimer--;
    } else {
        camera->yawUpdateRateInv = Camera_LERPCeilF(rwData->swing.swingUpdateRate -
                                                        ((OREG(49) * 0.01f) * rwData->swing.swingUpdateRate * sp94),
                                                    camera->yawUpdateRateInv, sp98, rate);
        camera->pitchUpdateRateInv =
            Camera_LERPCeilF(R_CAM_PITCH_UPDATE_RATE_INV, camera->pitchUpdateRateInv, sp9C, rate);
    }

    camera->pitchUpdateRateInv = Camera_LERPCeilF(R_CAM_PITCH_UPDATE_RATE_INV, camera->pitchUpdateRateInv, sp9C, rate);
    camera->xzOffsetUpdateRate =
        Camera_LERPCeilF(CAM_DATA_SCALED(R_CAM_XZ_OFFSET_UPDATE_RATE), camera->xzOffsetUpdateRate, spA0, rate);
    camera->yOffsetUpdateRate =
        Camera_LERPCeilF(CAM_DATA_SCALED(R_CAM_Y_OFFSET_UPDATE_RATE), camera->yOffsetUpdateRate, sp9C, rate);
    camera->fovUpdateRate = Camera_LERPCeilF(CAM_DATA_SCALED(R_CAM_FOV_UPDATE_RATE), camera->yOffsetUpdateRate,
                                             camera->speedRatio * 0.05f, rate);

    if (roData->interfaceField & NORMAL1_FLAG_0) {
        t = Camera_GetPitchAdjFromFloorHeightDiffs(camera, atEyeGeo.yaw - 0x7FFF, false);
        sp9C = ((1.0f / roData->unk_10) * 0.5f) * (1.0f - camera->speedRatio);
        rwData->slopePitchAdj =
            Camera_LERPCeilS(t, rwData->slopePitchAdj, ((1.0f / roData->unk_10) * 0.5f) + sp9C, 0xF);
    } else {
        rwData->slopePitchAdj = 0;
        if (camera->playerGroundY == camera->playerPosRot.pos.y) {
            rwData->yOffset = camera->playerPosRot.pos.y;
        }
    }

    spA0 = ((rwData->swing.unk_18 != 0) && (roData->yOffset > -40.0f))
               ? (sp9C = Math_SinS(rwData->swing.unk_14), ((-40.0f * sp9C) + (roData->yOffset * (1.0f - sp9C))))
               : roData->yOffset;

    if (roData->interfaceField & NORMAL1_FLAG_7) {
        func_800458D4(camera, &atEyeNextGeo, spA0, &rwData->yOffset, roData->interfaceField & NORMAL1_FLAG_0);
    } else if (roData->interfaceField & NORMAL1_FLAG_5) {
        func_80045B08(camera, &atEyeNextGeo, spA0, rwData->slopePitchAdj);
    } else {
        Camera_CalcAtDefault(camera, &atEyeNextGeo, spA0, roData->interfaceField & NORMAL1_FLAG_0);
    }

    OLib_Vec3fDiffToVecGeo(&eyeAdjustment, at, eyeNext);

    camera->dist = eyeAdjustment.r =
        Camera_ClampDist(camera, eyeAdjustment.r, roData->distMin, roData->distMax, rwData->unk_28);

    if (rwData->startSwingTimer <= 0) {
        eyeAdjustment.pitch = atEyeNextGeo.pitch;
        eyeAdjustment.yaw =
            Camera_LERPCeilS(rwData->swingYawTarget, atEyeNextGeo.yaw, 1.0f / camera->yawUpdateRateInv, 0xA);
    } else if (rwData->swing.unk_18 != 0) {
        eyeAdjustment.yaw =
            Camera_LERPCeilS(rwData->swing.unk_16, atEyeNextGeo.yaw, 1.0f / camera->yawUpdateRateInv, 0xA);
        eyeAdjustment.pitch =
            Camera_LERPCeilS(rwData->swing.unk_14, atEyeNextGeo.pitch, 1.0f / camera->yawUpdateRateInv, 0xA);
    } else {
        // rotate yaw to follow player.
        eyeAdjustment.yaw =
            Camera_CalcDefaultYaw(camera, atEyeNextGeo.yaw, camera->playerPosRot.rot.y, roData->unk_14, sp94);
        eyeAdjustment.pitch =
            Camera_CalcDefaultPitch(camera, atEyeNextGeo.pitch, roData->pitchTarget, rwData->slopePitchAdj);
    }

    // set eyeAdjustment pitch from 79.65 degrees to -85 degrees
    if (eyeAdjustment.pitch > 0x38A4) {
        eyeAdjustment.pitch = 0x38A4;
    }
    if (eyeAdjustment.pitch < -0x3C8C) {
        eyeAdjustment.pitch = -0x3C8C;
    }

    Camera_AddVecGeoToVec3f(eyeNext, at, &eyeAdjustment);
    if ((camera->status == CAM_STAT_ACTIVE) && !(roData->interfaceField & NORMAL1_FLAG_4)) {
        rwData->swingYawTarget = camera->playerPosRot.rot.y - 0x7FFF;
        if (rwData->startSwingTimer > 0) {
            func_80046E20(camera, &eyeAdjustment, roData->distMin, roData->unk_0C, &sp98, &rwData->swing);
        } else {
            sp88 = *eyeNext;
            rwData->swing.swingUpdateRate = camera->yawUpdateRateInv = roData->unk_0C * 2.0f;
            if (Camera_BGCheck(camera, at, &sp88)) {
                rwData->swingYawTarget = atEyeNextGeo.yaw;
                rwData->startSwingTimer = -1;
            } else {
                *eye = *eyeNext;
            }
            rwData->swing.unk_18 = 0;
        }

        if (rwData->swing.unk_18 != 0) {
            camera->inputDir.y =
                Camera_LERPCeilS(camera->inputDir.y + (s16)((s16)(rwData->swing.unk_16 - 0x7FFF) - camera->inputDir.y),
                                 camera->inputDir.y, 1.0f - (0.99f * sp98), 0xA);
        }

        if (roData->interfaceField & NORMAL1_FLAG_2) {
            camera->inputDir.x = -atEyeGeo.pitch;
            camera->inputDir.y = atEyeGeo.yaw - 0x7FFF;
            camera->inputDir.z = 0;
        } else {
            OLib_Vec3fDiffToVecGeo(&eyeAdjustment, eye, at);
            camera->inputDir.x = eyeAdjustment.pitch;
            camera->inputDir.y = eyeAdjustment.yaw;
            camera->inputDir.z = 0;
        }

        // crit wiggle
        if (gSaveContext.health <= 16 && ((camera->play->state.frames % 256) == 0)) {
            wiggleAdj = Rand_ZeroOne() * 10000.0f;
            camera->inputDir.y = wiggleAdj + camera->inputDir.y;
        }
    } else {
        rwData->swing.swingUpdateRate = roData->unk_0C;
        rwData->swing.unk_18 = 0;
        sUpdateCameraDirection = 0;
        *eye = *eyeNext;
    }

    spA0 = (gSaveContext.health <= 16 ? 0.8f : 1.0f);
    camera->fov = Camera_LERPCeilF(roData->fovTarget * spA0, camera->fov, camera->fovUpdateRate, 1.0f);
    camera->roll = Camera_LERPCeilS(0, camera->roll, 0.5f, 0xA);
    camera->atLERPStepScale = Camera_ClampLERPScale(camera, roData->atLERPScaleMax);
    return 1;
}

s32 Camera_Normal2(Camera* camera) {
    Vec3f* eye = &camera->eye;
    Vec3f* at = &camera->at;
    Vec3f* eyeNext = &camera->eyeNext;
    CamColChk bgChk;
    s16 phi_a0;
    s16 phi_a1;
    f32 spA4;
    f32 spA0;
    VecGeo adjGeo;
    VecGeo sp90;
    VecGeo sp88;
    VecGeo atToEyeDir;
    VecGeo atToEyeNextDir;
    PosRot* playerPosRot = &camera->playerPosRot;
    Normal2ReadOnlyData* roData = &camera->paramData.norm2.roData;
    Normal2ReadWriteData* rwData = &camera->paramData.norm2.rwData;
    s32 pad;
    BgCamFuncData* bgCamFuncData;
    f32 playerHeight;
    f32 yNormal;

    playerHeight = Player_GetHeight(camera->player);
    yNormal =
        1.0f + CAM_DATA_SCALED(R_CAM_YOFFSET_NORM) - (CAM_DATA_SCALED(R_CAM_YOFFSET_NORM) * (68.0f / playerHeight));

    if (R_RELOAD_CAM_PARAMS) {
        Camera_CopyPREGToModeValues(camera);
    }

    if (RELOAD_PARAMS(camera) || R_RELOAD_CAM_PARAMS) {
        CameraModeValue* values = sCameraSettings[camera->setting].cameraModes[camera->mode].values;

        roData->unk_00 = GET_NEXT_SCALED_RO_DATA(values) * playerHeight * yNormal;
        roData->unk_04 = GET_NEXT_SCALED_RO_DATA(values) * playerHeight * yNormal;
        roData->unk_08 = GET_NEXT_SCALED_RO_DATA(values) * playerHeight * yNormal;
        roData->unk_1C = CAM_DEG_TO_BINANG(GET_NEXT_RO_DATA(values));
        roData->unk_0C = GET_NEXT_RO_DATA(values);
        roData->unk_10 = GET_NEXT_SCALED_RO_DATA(values);
        roData->unk_14 = GET_NEXT_RO_DATA(values);
        roData->unk_18 = GET_NEXT_SCALED_RO_DATA(values);
        roData->interfaceField = GET_NEXT_RO_DATA(values);
    }

    if (R_RELOAD_CAM_PARAMS) {
        Camera_CopyPREGToModeValues(camera);
    }

    sCameraInterfaceField = roData->interfaceField;

    switch (camera->animState) {
        case 0:
        case 10:
        case 20:
        case 25:
            bgCamFuncData = (BgCamFuncData*)Camera_GetBgCamFuncData(camera);
            Camera_Vec3sToVec3f(&rwData->unk_00, &bgCamFuncData->pos);
            rwData->unk_20 = bgCamFuncData->rot.x;
            rwData->unk_22 = bgCamFuncData->rot.y;
            rwData->unk_24 = playerPosRot->pos.y;
            rwData->unk_1C = bgCamFuncData->fov == -1   ? roData->unk_14
                             : bgCamFuncData->fov > 360 ? CAM_DATA_SCALED(bgCamFuncData->fov)
                                                        : bgCamFuncData->fov;

            rwData->unk_28 = bgCamFuncData->flags == -1 ? 0 : bgCamFuncData->flags;

            rwData->unk_18 = 0.0f;

            if (roData->interfaceField & NORMAL2_FLAG_2) {
                sp88.pitch = rwData->unk_20;
                sp88.yaw = rwData->unk_22 + 0x3FFF;
                sp88.r = 100.0f;
                OLib_VecGeoToVec3f(&rwData->unk_0C, &sp88);
            }

            camera->animState = 1;
            camera->yawUpdateRateInv = 50.0f;
            break;
        default:
            if (camera->playerGroundY == playerPosRot->pos.y) {
                rwData->unk_24 = playerPosRot->pos.y;
            }
            break;
    }

    OLib_Vec3fDiffToVecGeo(&atToEyeDir, at, eye);
    OLib_Vec3fDiffToVecGeo(&atToEyeNextDir, at, eyeNext);

    camera->speedRatio *= 0.5f;
    spA4 = CAM_DATA_SCALED(R_CAM_UPDATE_RATE_STEP_SCALE_XZ) * camera->speedRatio;
    spA0 = CAM_DATA_SCALED(R_CAM_UPDATE_RATE_STEP_SCALE_Y) * camera->speedRatio;

    camera->yawUpdateRateInv = Camera_LERPCeilF(roData->unk_0C, camera->yawUpdateRateInv * camera->speedRatio,
                                                CAM_DATA_SCALED(R_CAM_UPDATE_RATE_STEP_SCALE_XZ), 0.1f);
    camera->pitchUpdateRateInv = Camera_LERPCeilF(R_CAM_PITCH_UPDATE_RATE_INV, camera->pitchUpdateRateInv, spA0, 0.1f);
    camera->xzOffsetUpdateRate =
        Camera_LERPCeilF(CAM_DATA_SCALED(R_CAM_XZ_OFFSET_UPDATE_RATE), camera->xzOffsetUpdateRate, spA4, 0.1f);
    camera->yOffsetUpdateRate =
        Camera_LERPCeilF(CAM_DATA_SCALED(R_CAM_Y_OFFSET_UPDATE_RATE), camera->yOffsetUpdateRate, spA0, 0.1f);
    camera->fovUpdateRate = Camera_LERPCeilF(CAM_DATA_SCALED(R_CAM_FOV_UPDATE_RATE), camera->yOffsetUpdateRate,
                                             camera->speedRatio * 0.05f, 0.1f);

    if (!(roData->interfaceField & NORMAL2_FLAG_7)) {
        Camera_CalcAtDefault(camera, &atToEyeNextDir, roData->unk_00, roData->interfaceField & NORMAL2_FLAG_0);
    } else {
        func_800458D4(camera, &atToEyeNextDir, roData->unk_00, &rwData->unk_24,
                      roData->interfaceField & NORMAL2_FLAG_0);
    }

    if (roData->interfaceField & NORMAL2_FLAG_2) {
        rwData->unk_00.x = playerPosRot->pos.x + rwData->unk_0C.x;
        rwData->unk_00.z = playerPosRot->pos.z + rwData->unk_0C.z;
    }

    rwData->unk_00.y = playerPosRot->pos.y;

    OLib_Vec3fDiffToVecGeo(&sp88, &rwData->unk_00, at);
    OLib_Vec3fDiffToVecGeo(&sp90, at, eyeNext);

    phi_a1 = (rwData->unk_28 & 2 ? rwData->unk_22 : roData->unk_1C);
    phi_a0 = sp90.yaw - sp88.yaw;
    if ((phi_a1 < 0x4000 && ABS(phi_a0) > phi_a1) || (phi_a1 >= 0x4000 && ABS(phi_a0) < phi_a1)) {

        phi_a0 = (phi_a0 < 0 ? -phi_a1 : phi_a1);
        phi_a0 += sp88.yaw;
        adjGeo.yaw =
            Camera_LERPCeilS(phi_a0, atToEyeDir.yaw, (1.0f / camera->yawUpdateRateInv) * camera->speedRatio, 0xA);
        if (rwData->unk_28 & 1) {
            adjGeo.pitch = Camera_CalcDefaultPitch(camera, atToEyeNextDir.pitch, rwData->unk_20, 0);
        } else {
            adjGeo.pitch = atToEyeDir.pitch;
        }
    } else {
        adjGeo = sp90;
    }

    camera->dist = adjGeo.r = Camera_ClampDist(camera, sp90.r, roData->unk_04, roData->unk_08, 0);

    if (!(rwData->unk_28 & 1)) {
        if (adjGeo.pitch >= 0xE39) {
            adjGeo.pitch += ((s16)(0xE38 - adjGeo.pitch) >> 2);
        }

        if (adjGeo.pitch < 0) {
            adjGeo.pitch += ((s16)(-0x38E - adjGeo.pitch) >> 2);
        }
    }

    Camera_AddVecGeoToVec3f(eyeNext, at, &adjGeo);

    if (camera->status == CAM_STAT_ACTIVE) {
        bgChk.pos = *eyeNext;
        if (!camera->play->envCtx.skyboxDisabled || roData->interfaceField & NORMAL2_FLAG_4) {
            Camera_BGCheckInfo(camera, at, &bgChk);
            *eye = bgChk.pos;
        } else {
            func_80043F94(camera, at, &bgChk);
            *eye = bgChk.pos;
            OLib_Vec3fDiffToVecGeo(&adjGeo, eye, at);
            camera->inputDir.x = adjGeo.pitch;
            camera->inputDir.y = adjGeo.yaw;
            camera->inputDir.z = 0;
        }
    }

    camera->fov = Camera_LERPCeilF(rwData->unk_1C, camera->fov, camera->fovUpdateRate, 1.0f);
    camera->roll = Camera_LERPCeilS(0, camera->roll, 0.5f, 0xA);
    camera->atLERPStepScale = Camera_ClampLERPScale(camera, roData->unk_18);
    return 1;
}

// riding epona
s32 Camera_Normal3(Camera* camera) {
    Vec3f* eye = &camera->eye;
    Vec3f* at = &camera->at;
    Vec3f* eyeNext = &camera->eyeNext;
    f32 sp98;
    f32 sp94;
    f32 sp90;
    f32 sp8C;
    VecGeo sp84;
    VecGeo sp7C;
    VecGeo sp74;
    PosRot* playerPosRot = &camera->playerPosRot;
    f32 temp_f0;
    f32 temp_f6;
    s16 phi_a0;
    s16 t2;
    Normal3ReadOnlyData* roData = &camera->paramData.norm3.roData;
    Normal3ReadWriteData* rwData = &camera->paramData.norm3.rwData;
    f32 playerHeight;

    playerHeight = Player_GetHeight(camera->player);
    if (RELOAD_PARAMS(camera) || R_RELOAD_CAM_PARAMS) {
        CameraModeValue* values = sCameraSettings[camera->setting].cameraModes[camera->mode].values;

        roData->yOffset = GET_NEXT_RO_DATA(values) * CAM_DATA_SCALED(playerHeight);
        roData->distMin = GET_NEXT_RO_DATA(values) * CAM_DATA_SCALED(playerHeight);
        roData->distMax = GET_NEXT_RO_DATA(values) * CAM_DATA_SCALED(playerHeight);
        roData->pitchTarget = CAM_DEG_TO_BINANG(GET_NEXT_RO_DATA(values));
        roData->yawUpdateSpeed = GET_NEXT_RO_DATA(values);
        roData->unk_10 = GET_NEXT_RO_DATA(values);
        roData->fovTarget = GET_NEXT_RO_DATA(values);
        roData->maxAtLERPScale = GET_NEXT_SCALED_RO_DATA(values);
        roData->interfaceField = GET_NEXT_RO_DATA(values);
    }

    if (R_RELOAD_CAM_PARAMS) {
        Camera_CopyPREGToModeValues(camera);
    }

    OLib_Vec3fDiffToVecGeo(&sp7C, at, eye);
    OLib_Vec3fDiffToVecGeo(&sp74, at, eyeNext);

    sUpdateCameraDirection = true;
    sCameraInterfaceField = roData->interfaceField;
    switch (camera->animState) {
        case 0:
        case 10:
        case 20:
        case 25:
            rwData->swing.atEyePoly = NULL;
            rwData->curPitch = 0;
            rwData->unk_1C = 0.0f;
            rwData->unk_20 = camera->playerGroundY;
            rwData->swing.unk_16 = rwData->swing.unk_14 = rwData->swing.unk_18 = 0;
            rwData->swing.swingUpdateRate = roData->yawUpdateSpeed;
            rwData->yawUpdAmt =
                (s16)((s16)(playerPosRot->rot.y - 0x7FFF) - sp7C.yaw) * (1.0f / R_CAM_DEFAULT_ANIM_TIME);
            rwData->distTimer = 10;
            rwData->yawTimer = R_CAM_DEFAULT_ANIM_TIME;
            camera->animState = 1;
            rwData->swing.swingUpdateRateTimer = 0;
    }

    if (rwData->distTimer != 0) {
        rwData->distTimer--;
    }

    sp98 = CAM_DATA_SCALED(R_CAM_UPDATE_RATE_STEP_SCALE_XZ) * camera->speedRatio;
    sp94 = CAM_DATA_SCALED(R_CAM_UPDATE_RATE_STEP_SCALE_Y) * camera->speedRatio;

    if (rwData->swing.swingUpdateRateTimer != 0) {
        camera->yawUpdateRateInv = Camera_LERPCeilF(roData->yawUpdateSpeed + (rwData->swing.swingUpdateRateTimer * 2),
                                                    camera->yawUpdateRateInv, sp98, 0.1f);
        camera->pitchUpdateRateInv =
            Camera_LERPCeilF((f32)R_CAM_PITCH_UPDATE_RATE_INV + (rwData->swing.swingUpdateRateTimer * 2),
                             camera->pitchUpdateRateInv, sp94, 0.1f);
        if (1) {}
        rwData->swing.swingUpdateRateTimer--;
    } else {
        camera->yawUpdateRateInv = Camera_LERPCeilF(roData->yawUpdateSpeed, camera->yawUpdateRateInv, sp98, 0.1f);
        camera->pitchUpdateRateInv =
            Camera_LERPCeilF(R_CAM_PITCH_UPDATE_RATE_INV, camera->pitchUpdateRateInv, sp94, 0.1f);
    }

    camera->xzOffsetUpdateRate =
        Camera_LERPCeilF(CAM_DATA_SCALED(R_CAM_XZ_OFFSET_UPDATE_RATE), camera->xzOffsetUpdateRate, sp98, 0.1f);
    camera->yOffsetUpdateRate =
        Camera_LERPCeilF(CAM_DATA_SCALED(R_CAM_Y_OFFSET_UPDATE_RATE), camera->yOffsetUpdateRate, sp94, 0.1f);
    camera->fovUpdateRate = Camera_LERPCeilF(CAM_DATA_SCALED(R_CAM_FOV_UPDATE_RATE), camera->fovUpdateRate, sp94, 0.1f);

    t2 = Camera_GetPitchAdjFromFloorHeightDiffs(camera, sp7C.yaw - 0x7FFF, true);
    sp94 = ((1.0f / roData->unk_10) * 0.5f);
    temp_f0 = (((1.0f / roData->unk_10) * 0.5f) * (1.0f - camera->speedRatio));
    rwData->curPitch = Camera_LERPCeilS(t2, rwData->curPitch, sp94 + temp_f0, 0xF);

    Camera_CalcAtForHorse(camera, &sp74, roData->yOffset, &rwData->unk_20, 1);
    sp90 = (roData->distMax + roData->distMin) * 0.5f;
    OLib_Vec3fDiffToVecGeo(&sp84, at, eyeNext);
    camera->dist = sp84.r = Camera_ClampDist(camera, sp84.r, roData->distMin, roData->distMax, rwData->distTimer);
    if (camera->xzSpeed > 0.001f) {
        sp84.r += (sp90 - sp84.r) * 0.002f;
    }
    phi_a0 = roData->pitchTarget - rwData->curPitch;
    sp84.pitch = Camera_LERPCeilS(phi_a0, sp74.pitch, 1.0f / camera->pitchUpdateRateInv, 0xA);

    if (sp84.pitch > R_CAM_MAX_PITCH) {
        sp84.pitch = R_CAM_MAX_PITCH;
    }
    if (sp84.pitch < R_CAM_MIN_PITCH_1) {
        sp84.pitch = R_CAM_MIN_PITCH_1;
    }

    phi_a0 = playerPosRot->rot.y - (s16)(sp74.yaw - 0x7FFF);
    if (ABS(phi_a0) > 0x2AF8) {
        if (phi_a0 > 0) {
            phi_a0 = 0x2AF8;
        } else {
            phi_a0 = -0x2AF8;
        }
    }

    sp90 = 1.0f;
    sp98 = 0.5;
    sp94 = camera->speedRatio;
    sp90 -= sp98;
    sp98 = sp98 + (sp94 * sp90);
    sp98 = (sp98 * phi_a0) / camera->yawUpdateRateInv;

    sp84.yaw = fabsf(sp98) > (150.0f * (1.0f - camera->speedRatio)) ? (s16)(sp74.yaw + sp98) : sp74.yaw;

    if (rwData->yawTimer > 0) {
        sp84.yaw += rwData->yawUpdAmt;
        rwData->yawTimer--;
    }

    Camera_AddVecGeoToVec3f(eyeNext, at, &sp84);

    if (camera->status == CAM_STAT_ACTIVE) {
        func_80046E20(camera, &sp84, roData->distMin, roData->yawUpdateSpeed, &sp8C, &rwData->swing);
    } else {
        *eye = *eyeNext;
    }

    camera->fov = Camera_LERPCeilF(roData->fovTarget, camera->fov, camera->fovUpdateRate, 1.0f);
    camera->roll = Camera_LERPCeilS(0, camera->roll, 0.5f, 0xA);
    camera->atLERPStepScale = Camera_ClampLERPScale(camera, roData->maxAtLERPScale);
    return 1;
}

s32 Camera_Normal4(Camera* camera) {
    return Camera_Noop(camera);
}

s32 Camera_Normal0(Camera* camera) {
    return Camera_Noop(camera);
}

s32 Camera_Parallel1(Camera* camera) {
    Vec3f* eye = &camera->eye;
    Vec3f* at = &camera->at;
    Vec3f* eyeNext = &camera->eyeNext;
    f32 spB8;
    f32 spB4;
    s16 tangle;
    VecGeo spA8;
    VecGeo atToEyeDir;
    VecGeo atToEyeNextDir;
    PosRot* playerPosRot = &camera->playerPosRot;
    CamColChk sp6C;
    s16 sp6A;
    s16 phi_a0;
    Parallel1ReadOnlyData* roData = &camera->paramData.para1.roData;
    Parallel1ReadWriteData* rwData = &camera->paramData.para1.rwData;
    f32 pad2;
    f32 playerHeight;
    s32 pad3;

    playerHeight = Player_GetHeight(camera->player);
    if (RELOAD_PARAMS(camera) || R_RELOAD_CAM_PARAMS) {
        CameraModeValue* values = sCameraSettings[camera->setting].cameraModes[camera->mode].values;
        f32 yNormal =
            1.0f + CAM_DATA_SCALED(R_CAM_YOFFSET_NORM) - (CAM_DATA_SCALED(R_CAM_YOFFSET_NORM) * (68.0f / playerHeight));

        roData->yOffset = GET_NEXT_SCALED_RO_DATA(values) * playerHeight * yNormal;
        roData->distTarget = GET_NEXT_SCALED_RO_DATA(values) * playerHeight * yNormal;
        roData->pitchTarget = CAM_DEG_TO_BINANG(GET_NEXT_RO_DATA(values));
        roData->yawTarget = CAM_DEG_TO_BINANG(GET_NEXT_RO_DATA(values));
        roData->unk_08 = GET_NEXT_RO_DATA(values);
        roData->unk_0C = GET_NEXT_RO_DATA(values);
        roData->fovTarget = GET_NEXT_RO_DATA(values);
        roData->unk_14 = GET_NEXT_SCALED_RO_DATA(values);
        roData->interfaceField = GET_NEXT_RO_DATA(values);
        roData->unk_18 = GET_NEXT_SCALED_RO_DATA(values) * playerHeight * yNormal;
        roData->unk_1C = GET_NEXT_SCALED_RO_DATA(values);
    }

    if (R_RELOAD_CAM_PARAMS) {
        Camera_CopyPREGToModeValues(camera);
    }

    OLib_Vec3fDiffToVecGeo(&atToEyeDir, at, eye);
    OLib_Vec3fDiffToVecGeo(&atToEyeNextDir, at, eyeNext);

    switch (camera->animState) {
        case 0:
        case 10:
        case 20:
        case 25:
            rwData->unk_16 = 0;
            rwData->unk_10 = 0;
            if (roData->interfaceField & PARALLEL1_FLAG_2) {
                rwData->animTimer = 20;
            } else {
                rwData->animTimer = R_CAM_DEFAULT_ANIM_TIME;
            }
            rwData->unk_00.x = 0.0f;
            rwData->yTarget = playerPosRot->pos.y - camera->playerPosDelta.y;
            camera->animState++;
            break;
    }

    if (rwData->animTimer != 0) {
        if (roData->interfaceField & PARALLEL1_FLAG_1) {
            // Rotate roData->yawTarget degrees from behind the player.
            rwData->yawTarget = (s16)(playerPosRot->rot.y - 0x7FFF) + roData->yawTarget;
        } else if (roData->interfaceField & PARALLEL1_FLAG_2) {
            // rotate to roData->yawTarget
            rwData->yawTarget = roData->yawTarget;
        } else {
            // leave the rotation alone.
            rwData->yawTarget = atToEyeNextDir.yaw;
        }
    } else {
        if (roData->interfaceField & PARALLEL1_FLAG_5) {
            rwData->yawTarget = (s16)(playerPosRot->rot.y - 0x7FFF) + roData->yawTarget;
        }
        sCameraInterfaceField = roData->interfaceField;
    }

    rwData->pitchTarget = roData->pitchTarget;

    if (camera->animState == 21) {
        rwData->unk_16 = 1;
        camera->animState = 1;
    } else if (camera->animState == 11) {
        camera->animState = 1;
    }

    spB8 = CAM_DATA_SCALED(R_CAM_UPDATE_RATE_STEP_SCALE_XZ) * camera->speedRatio;
    spB4 = CAM_DATA_SCALED(R_CAM_UPDATE_RATE_STEP_SCALE_Y) * camera->speedRatio;

    camera->rUpdateRateInv = Camera_LERPCeilF(R_CAM_R_UPDATE_RATE_INV, camera->rUpdateRateInv, spB8, 0.1f);
    camera->yawUpdateRateInv = Camera_LERPCeilF(roData->unk_08, camera->yawUpdateRateInv, spB8, 0.1f);
    camera->pitchUpdateRateInv = Camera_LERPCeilF(2.0f, camera->pitchUpdateRateInv, spB4, 0.1f);
    camera->xzOffsetUpdateRate =
        Camera_LERPCeilF(CAM_DATA_SCALED(R_CAM_XZ_OFFSET_UPDATE_RATE), camera->xzOffsetUpdateRate, spB8, 0.1f);
    camera->yOffsetUpdateRate =
        Camera_LERPCeilF(CAM_DATA_SCALED(R_CAM_Y_OFFSET_UPDATE_RATE), camera->yOffsetUpdateRate, spB4, 0.1f);
    camera->fovUpdateRate = Camera_LERPCeilF(CAM_DATA_SCALED(R_CAM_FOV_UPDATE_RATE), camera->fovUpdateRate,
                                             camera->speedRatio * 0.05f, 0.1f);

    if (roData->interfaceField & PARALLEL1_FLAG_0) {
        tangle = Camera_GetPitchAdjFromFloorHeightDiffs(camera, atToEyeDir.yaw - 0x7FFF, true);

        spB8 = ((1.0f / roData->unk_0C) * 0.3f);
        pad2 = (((1.0f / roData->unk_0C) * 0.7f) * (1.0f - camera->speedRatio));
        rwData->unk_10 = Camera_LERPCeilS(tangle, rwData->unk_10, spB8 + pad2, 0xF);
    } else {
        rwData->unk_10 = 0;
    }

    if (camera->playerGroundY == camera->playerPosRot.pos.y || camera->player->actor.gravity > -0.1f ||
        camera->player->stateFlags1 & PLAYER_STATE1_21) {
        rwData->yTarget = playerPosRot->pos.y;
        sp6A = 0;
    } else {
        sp6A = 1;
    }

    if (!(roData->interfaceField & PARALLEL1_FLAG_7) && !sp6A) {
        Camera_CalcAtForParallel(camera, &atToEyeNextDir, roData->yOffset, &rwData->yTarget,
                                 roData->interfaceField & PARALLEL1_FLAG_0);
    } else {
        func_800458D4(camera, &atToEyeNextDir, roData->unk_18, &rwData->yTarget,
                      roData->interfaceField & PARALLEL1_FLAG_0);
    }

    if (rwData->animTimer != 0) {
        camera->stateFlags |= CAM_STATE_5;
        tangle = (((rwData->animTimer + 1) * rwData->animTimer) >> 1);
        spA8.yaw = atToEyeDir.yaw + (((s16)(rwData->yawTarget - atToEyeDir.yaw) / tangle) * rwData->animTimer);
        spA8.pitch = atToEyeDir.pitch;
        spA8.r = atToEyeDir.r;
        rwData->animTimer--;
    } else {
        rwData->unk_16 = 0;
        camera->dist = Camera_LERPCeilF(roData->distTarget, camera->dist, 1.0f / camera->rUpdateRateInv, 2.0f);
        OLib_Vec3fDiffToVecGeo(&spA8, at, eyeNext);
        spA8.r = camera->dist;

        if (roData->interfaceField & PARALLEL1_FLAG_6) {
            spA8.yaw = Camera_LERPCeilS(rwData->yawTarget, atToEyeNextDir.yaw, 0.6f, 0xA);
        } else {
            spA8.yaw = Camera_LERPCeilS(rwData->yawTarget, atToEyeNextDir.yaw, 0.8f, 0xA);
        }

        if (roData->interfaceField & PARALLEL1_FLAG_0) {
            phi_a0 = rwData->pitchTarget - rwData->unk_10;
        } else {
            phi_a0 = rwData->pitchTarget;
        }

        spA8.pitch = Camera_LERPCeilS(phi_a0, atToEyeNextDir.pitch, 1.0f / camera->pitchUpdateRateInv, 4);

        if (spA8.pitch > R_CAM_MAX_PITCH) {
            spA8.pitch = R_CAM_MAX_PITCH;
        }

        if (spA8.pitch < R_CAM_MIN_PITCH_1) {
            spA8.pitch = R_CAM_MIN_PITCH_1;
        }
    }
    Camera_AddVecGeoToVec3f(eyeNext, at, &spA8);
    if (camera->status == CAM_STAT_ACTIVE) {
        sp6C.pos = *eyeNext;
        if (!camera->play->envCtx.skyboxDisabled || roData->interfaceField & PARALLEL1_FLAG_4) {
            Camera_BGCheckInfo(camera, at, &sp6C);
            *eye = sp6C.pos;
        } else {
            func_80043F94(camera, at, &sp6C);
            *eye = sp6C.pos;
            OLib_Vec3fDiffToVecGeo(&spA8, eye, at);
            camera->inputDir.x = spA8.pitch;
            camera->inputDir.y = spA8.yaw;
            camera->inputDir.z = 0;
        }
    }
    camera->fov = Camera_LERPCeilF(roData->fovTarget, camera->fov, camera->fovUpdateRate, 1.0f);
    camera->roll = Camera_LERPCeilS(0, camera->roll, 0.5, 0xA);
    camera->atLERPStepScale = Camera_ClampLERPScale(camera, sp6A ? roData->unk_1C : roData->unk_14);
    //! @bug doesn't return
}

s32 Camera_Parallel2(Camera* camera) {
    return Camera_Noop(camera);
}

s32 Camera_Parallel3(Camera* camera) {
    CameraModeValue* values = sCameraSettings[camera->setting].cameraModes[camera->mode].values;
    s16 interfaceField = GET_NEXT_RO_DATA(values);

    sCameraInterfaceField = interfaceField;

    if (interfaceField & PARALLEL3_FLAG_0) {
        camera->stateFlags |= CAM_STATE_10;
    }
    if (interfaceField & PARALLEL3_FLAG_1) {
        camera->stateFlags |= CAM_STATE_4;
    }
    //! @bug doesn't return
}

s32 Camera_Parallel4(Camera* camera) {
    return Camera_Noop(camera);
}

s32 Camera_Parallel0(Camera* camera) {
    return Camera_Noop(camera);
}

/**
 * Generic jump, jumping off ledges
 */
s32 Camera_Jump1(Camera* camera) {
    Vec3f* eye = &camera->eye;
    Vec3f* at = &camera->at;
    Vec3f* eyeNext = &camera->eyeNext;
    s32 pad2;
    f32 spA4;
    Vec3f newEye;
    VecGeo eyeAtOffset;
    VecGeo eyeNextAtOffset;
    VecGeo eyeDiffGeo;
    VecGeo eyeDiffTarget;
    PosRot* playerPosRot = &camera->playerPosRot;
    PosRot playerhead;
    s16 tangle;
    Jump1ReadOnlyData* roData = &camera->paramData.jump1.roData;
    Jump1ReadWriteData* rwData = &camera->paramData.jump1.rwData;
    s32 pad;
    f32 playerHeight;

    playerHeight = Player_GetHeight(camera->player);
    if (RELOAD_PARAMS(camera) || R_RELOAD_CAM_PARAMS) {
        CameraModeValue* values = sCameraSettings[camera->setting].cameraModes[camera->mode].values;
        f32 yNormal =
            1.0f + CAM_DATA_SCALED(R_CAM_YOFFSET_NORM) - (CAM_DATA_SCALED(R_CAM_YOFFSET_NORM) * (68.0f / playerHeight));

        roData->atYOffset = CAM_DATA_SCALED(GET_NEXT_RO_DATA(values)) * playerHeight * yNormal;
        roData->distMin = CAM_DATA_SCALED(GET_NEXT_RO_DATA(values)) * playerHeight * yNormal;
        roData->distMax = CAM_DATA_SCALED(GET_NEXT_RO_DATA(values)) * playerHeight * yNormal;
        roData->yawUpateRateTarget = GET_NEXT_RO_DATA(values);
        roData->maxYawUpdate = CAM_DATA_SCALED(GET_NEXT_RO_DATA(values));
        roData->unk_14 = GET_NEXT_RO_DATA(values);
        roData->atLERPScaleMax = CAM_DATA_SCALED(GET_NEXT_RO_DATA(values));
        roData->interfaceField = GET_NEXT_RO_DATA(values);
    }

    if (R_RELOAD_CAM_PARAMS) {
        Camera_CopyPREGToModeValues(camera);
    }

    // playerhead never gets used.
    Actor_GetFocus(&playerhead, &camera->player->actor);

    OLib_Vec3fDiffToVecGeo(&eyeAtOffset, at, eye);
    OLib_Vec3fDiffToVecGeo(&eyeNextAtOffset, at, eyeNext);

    sCameraInterfaceField = roData->interfaceField;

    if (RELOAD_PARAMS(camera)) {
        rwData->swing.unk_16 = rwData->swing.unk_18 = 0;
        rwData->swing.atEyePoly = NULL;
        rwData->unk_24 = 0;
        rwData->unk_26 = 200;
        rwData->swing.swingUpdateRateTimer = 0;
        rwData->swing.swingUpdateRate = roData->yawUpateRateTarget;
        rwData->unk_1C = playerPosRot->pos.y - camera->playerPosDelta.y;
        rwData->unk_20 = eyeAtOffset.r;
        camera->posOffset.y -= camera->playerPosDelta.y;
        camera->xzOffsetUpdateRate = (1.0f / 10000.0f);
        camera->animState++;
    }

    if (rwData->swing.swingUpdateRateTimer != 0) {
        camera->yawUpdateRateInv =
            Camera_LERPCeilF(roData->yawUpateRateTarget + rwData->swing.swingUpdateRateTimer, camera->yawUpdateRateInv,
                             CAM_DATA_SCALED(R_CAM_UPDATE_RATE_STEP_SCALE_Y), 0.1f);
        camera->pitchUpdateRateInv =
            Camera_LERPCeilF((f32)R_CAM_PITCH_UPDATE_RATE_INV + rwData->swing.swingUpdateRateTimer,
                             camera->pitchUpdateRateInv, CAM_DATA_SCALED(R_CAM_UPDATE_RATE_STEP_SCALE_Y), 0.1f);
        rwData->swing.swingUpdateRateTimer--;
    } else {
        camera->yawUpdateRateInv = Camera_LERPCeilF(roData->yawUpateRateTarget, camera->yawUpdateRateInv,
                                                    CAM_DATA_SCALED(R_CAM_UPDATE_RATE_STEP_SCALE_Y), 0.1f);
        camera->pitchUpdateRateInv = Camera_LERPCeilF((f32)R_CAM_PITCH_UPDATE_RATE_INV, camera->pitchUpdateRateInv,
                                                      CAM_DATA_SCALED(R_CAM_UPDATE_RATE_STEP_SCALE_Y), 0.1f);
    }

    camera->xzOffsetUpdateRate =
        Camera_LERPCeilF(CAM_DATA_SCALED(R_CAM_XZ_OFFSET_UPDATE_RATE), camera->xzOffsetUpdateRate,
                         CAM_DATA_SCALED(R_CAM_UPDATE_RATE_STEP_SCALE_XZ), 0.1f);
    camera->yOffsetUpdateRate = Camera_LERPCeilF(CAM_DATA_SCALED(R_CAM_Y_OFFSET_UPDATE_RATE), camera->yOffsetUpdateRate,
                                                 CAM_DATA_SCALED(R_CAM_UPDATE_RATE_STEP_SCALE_Y), 0.1f);
    camera->fovUpdateRate =
        Camera_LERPCeilF(CAM_DATA_SCALED(R_CAM_FOV_UPDATE_RATE), camera->yOffsetUpdateRate, 0.05f, 0.1f);

    func_800458D4(camera, &eyeNextAtOffset, roData->atYOffset, &rwData->unk_1C, 0);

    eyeDiffGeo = eyeAtOffset;

    OLib_Vec3fDiffToVecGeo(&eyeDiffTarget, at, eye);

    eyeDiffGeo.r = Camera_LERPCeilF(eyeDiffTarget.r, eyeAtOffset.r, CAM_DATA_SCALED(OREG(29)), 1.0f);
    eyeDiffGeo.pitch = Camera_LERPCeilS(eyeDiffTarget.pitch, eyeAtOffset.pitch, CAM_DATA_SCALED(OREG(29)), 0xA);

    if (rwData->swing.unk_18) {
        eyeDiffGeo.yaw =
            Camera_LERPCeilS(rwData->swing.unk_16, eyeNextAtOffset.yaw, 1.0f / camera->yawUpdateRateInv, 0xA);
        eyeDiffGeo.pitch =
            Camera_LERPCeilS(rwData->swing.unk_14, eyeNextAtOffset.pitch, 1.0f / camera->yawUpdateRateInv, 0xA);
    } else {
        eyeDiffGeo.yaw =
            Camera_CalcDefaultYaw(camera, eyeNextAtOffset.yaw, camera->playerPosRot.rot.y, roData->maxYawUpdate, 0.0f);
    }

    // Clamp the eye->at distance to roData->distMin < eyeDiffGeo.r < roData->distMax
    if (eyeDiffGeo.r < roData->distMin) {
        eyeDiffGeo.r = roData->distMin;
    } else if (eyeDiffGeo.r > roData->distMax) {
        eyeDiffGeo.r = roData->distMax;
    }

    // Clamp the phi rotation at R_CAM_MAX_PITCH AND R_CAM_MIN_PITCH_2
    if (eyeDiffGeo.pitch > R_CAM_MAX_PITCH) {
        eyeDiffGeo.pitch = R_CAM_MAX_PITCH;
    } else if (eyeDiffGeo.pitch < R_CAM_MIN_PITCH_2) {
        eyeDiffGeo.pitch = R_CAM_MIN_PITCH_2;
    }

    Camera_AddVecGeoToVec3f(&newEye, at, &eyeDiffGeo);
    eyeNext->x = newEye.x;
    eyeNext->z = newEye.z;
    eyeNext->y += (newEye.y - eyeNext->y) * CAM_DATA_SCALED(R_CAM_JUMP1_EYE_Y_STEP_SCALE);
    if ((camera->status == CAM_STAT_ACTIVE) && !(roData->interfaceField & JUMP1_FLAG_4)) {
        func_80046E20(camera, &eyeDiffGeo, roData->distMin, roData->yawUpateRateTarget, &spA4, &rwData->swing);
        if (roData->interfaceField & JUMP1_FLAG_2) {
            camera->inputDir.x = -eyeAtOffset.pitch;
            camera->inputDir.y = eyeAtOffset.yaw - 0x7FFF;
            camera->inputDir.z = 0;
        } else {
            OLib_Vec3fDiffToVecGeo(&eyeDiffGeo, eye, at);
            camera->inputDir.x = eyeDiffGeo.pitch;
            camera->inputDir.y = eyeDiffGeo.yaw;
            camera->inputDir.z = 0;
        }
        if (rwData->swing.unk_18) {
            camera->inputDir.y =
                Camera_LERPCeilS(camera->inputDir.y + (s16)((s16)(rwData->swing.unk_16 - 0x7FFF) - camera->inputDir.y),
                                 camera->inputDir.y, 1.0f - (0.99f * spA4), 0xA);
        }
    } else {
        rwData->swing.swingUpdateRate = roData->yawUpateRateTarget;
        rwData->swing.unk_18 = 0;
        sUpdateCameraDirection = 0;
        *eye = *eyeNext;
    }

    camera->dist = OLib_Vec3fDist(at, eye);
    camera->roll = Camera_LERPCeilS(0, camera->roll, 0.5f, 0xA);
    camera->atLERPStepScale = Camera_ClampLERPScale(camera, roData->atLERPScaleMax);
    return true;
}

// Climbing ladders/vines
s32 Camera_Jump2(Camera* camera) {
    Vec3f* eye = &camera->eye;
    Vec3f* at = &camera->at;
    Vec3f* eyeNext = &camera->eyeNext;
    Vec3f bgChkPos;
    Vec3f floorNorm;
    VecGeo adjAtToEyeDir;
    VecGeo bgChkPara;
    VecGeo atToEyeNextDir;
    VecGeo atToEyeDir;
    f32 temp_f14;
    f32 temp_f16;
    f32 sp90;
    f32 sp8C;
    s32 bgId;
    CamColChk camBgChk;
    PosRot* playerPosRot = &camera->playerPosRot;
    s16 yawDiff;
    s16 playerYawRot180;
    Jump2ReadOnlyData* roData = &camera->paramData.jump2.roData;
    Jump2ReadWriteData* rwData = &camera->paramData.jump2.rwData;
    CameraModeValue* values;
    f32 playerHeight;
    f32 yNormal;

    playerHeight = Player_GetHeight(camera->player);

    if (RELOAD_PARAMS(camera) || R_RELOAD_CAM_PARAMS) {
        values = sCameraSettings[camera->setting].cameraModes[camera->mode].values;
        yNormal =
            1.0f + CAM_DATA_SCALED(R_CAM_YOFFSET_NORM) - (CAM_DATA_SCALED(R_CAM_YOFFSET_NORM) * (68.0f / playerHeight));
        roData->atYOffset =
            CAM_DATA_SCALED((camera->playerPosDelta.y > 0.0f ? -10.0f : 10.0f) + GET_NEXT_RO_DATA(values)) *
            playerHeight * yNormal;
        roData->minDist = GET_NEXT_SCALED_RO_DATA(values) * playerHeight * yNormal;
        roData->maxDist = GET_NEXT_SCALED_RO_DATA(values) * playerHeight * yNormal;
        roData->minMaxDistFactor = GET_NEXT_SCALED_RO_DATA(values);
        roData->yawUpdRateTarget = GET_NEXT_RO_DATA(values);
        roData->xzUpdRateTarget = GET_NEXT_SCALED_RO_DATA(values);
        roData->fovTarget = GET_NEXT_RO_DATA(values);
        roData->atLERPStepScale = GET_NEXT_SCALED_RO_DATA(values);
        roData->interfaceField = GET_NEXT_RO_DATA(values);
    }

    if (R_RELOAD_CAM_PARAMS) {
        Camera_CopyPREGToModeValues(camera);
    }

    OLib_Vec3fDiffToVecGeo(&atToEyeDir, at, eye);
    OLib_Vec3fDiffToVecGeo(&atToEyeNextDir, at, eyeNext);

    sCameraInterfaceField = roData->interfaceField;

    if (RELOAD_PARAMS(camera)) {
        bgChkPos = playerPosRot->pos;
        rwData->floorY = Camera_GetFloorY(camera, &bgChkPos);
        rwData->yawTarget = atToEyeNextDir.yaw;
        rwData->initYawDiff = 0;
        if (rwData->floorY == BGCHECK_Y_MIN) {
            osSyncPrintf(VT_COL(YELLOW, BLACK) "camera: climb: no floor \n" VT_RST);
            rwData->onFloor = -1;
            rwData->floorY = playerPosRot->pos.y - 1000.0f;
        } else if (playerPosRot->pos.y - rwData->floorY < playerHeight) {
            // player's model is within the height of the floor.
            rwData->onFloor = 1;
        } else {
            rwData->onFloor = -1;
        }

        yawDiff = (s16)(playerPosRot->rot.y - 0x7FFF) - atToEyeNextDir.yaw;
        rwData->initYawDiff = ((yawDiff / R_CAM_DEFAULT_ANIM_TIME) / 4) * 3;
        if (roData->interfaceField & JUMP2_FLAG_1) {
            rwData->yawAdj = 0xA;
        } else {
            rwData->yawAdj = 0x2710;
        }

        playerPosRot->pos.x -= camera->playerPosDelta.x;
        playerPosRot->pos.y -= camera->playerPosDelta.y;
        playerPosRot->pos.z -= camera->playerPosDelta.z;
        rwData->animTimer = R_CAM_DEFAULT_ANIM_TIME;
        camera->animState++;
        camera->atLERPStepScale = roData->atLERPStepScale;
    }

    sp90 = CAM_DATA_SCALED(R_CAM_UPDATE_RATE_STEP_SCALE_XZ) * camera->speedRatio;
    sp8C = CAM_DATA_SCALED(R_CAM_UPDATE_RATE_STEP_SCALE_Y) * camera->speedRatio;
    camera->yawUpdateRateInv = Camera_LERPCeilF(roData->yawUpdRateTarget, camera->yawUpdateRateInv, sp90, 0.1f);
    camera->xzOffsetUpdateRate = Camera_LERPCeilF(roData->xzUpdRateTarget, camera->xzOffsetUpdateRate, sp90, 0.1f);
    camera->yOffsetUpdateRate =
        Camera_LERPCeilF(CAM_DATA_SCALED(R_CAM_Y_OFFSET_UPDATE_RATE), camera->yOffsetUpdateRate, sp8C, 0.1f);

    camera->fovUpdateRate = Camera_LERPCeilF(CAM_DATA_SCALED(R_CAM_FOV_UPDATE_RATE), camera->yOffsetUpdateRate,
                                             camera->speedRatio * 0.05f, 0.1f);
    camera->rUpdateRateInv = OREG(27);

    Camera_CalcAtDefault(camera, &atToEyeNextDir, roData->atYOffset, 0);
    OLib_Vec3fDiffToVecGeo(&adjAtToEyeDir, at, eye);

    temp_f16 = roData->minDist;
    sp90 = roData->maxDist + (roData->maxDist * roData->minMaxDistFactor);
    temp_f14 = temp_f16 - (roData->minDist * roData->minMaxDistFactor);

    if (adjAtToEyeDir.r > sp90) {
        adjAtToEyeDir.r = sp90;
    } else if (adjAtToEyeDir.r < temp_f14) {
        adjAtToEyeDir.r = temp_f14;
    }

    yawDiff = (s16)(playerPosRot->rot.y - 0x7FFF) - adjAtToEyeDir.yaw;
    if (rwData->animTimer != 0) {
        rwData->yawTarget = playerPosRot->rot.y - 0x7FFF;
        rwData->animTimer--;
        adjAtToEyeDir.yaw = Camera_LERPCeilS(rwData->yawTarget, atToEyeNextDir.yaw, 0.5f, 0xA);
    } else if (rwData->yawAdj < ABS(yawDiff)) {
        playerYawRot180 = playerPosRot->rot.y - 0x7FFF;
        adjAtToEyeDir.yaw = Camera_LERPFloorS(
            ((yawDiff < 0) ? (s16)(playerYawRot180 + rwData->yawAdj) : (s16)(playerYawRot180 - rwData->yawAdj)),
            atToEyeNextDir.yaw, 0.1f, 0xA);
    } else {
        adjAtToEyeDir.yaw = Camera_LERPCeilS(adjAtToEyeDir.yaw, atToEyeNextDir.yaw, 0.25f, 0xA);
    }

    // Check the floor at the top of the climb
    bgChkPos.x = playerPosRot->pos.x + (Math_SinS(playerPosRot->rot.y) * 25.0f);
    bgChkPos.y = playerPosRot->pos.y + (playerHeight * 2.2f);
    bgChkPos.z = playerPosRot->pos.z + (Math_CosS(playerPosRot->rot.y) * 25.0f);

    sp90 = Camera_GetFloorYNorm(camera, &floorNorm, &bgChkPos, &bgId);
    if ((sp90 != BGCHECK_Y_MIN) && (playerPosRot->pos.y < sp90)) {
        // top of the climb is within 2.2x of the player's height.
        camera->pitchUpdateRateInv =
            Camera_LERPCeilF(20.0f, camera->pitchUpdateRateInv, CAM_DATA_SCALED(R_CAM_UPDATE_RATE_STEP_SCALE_Y), 0.1f);
        camera->rUpdateRateInv =
            Camera_LERPCeilF(20.0f, camera->rUpdateRateInv, CAM_DATA_SCALED(R_CAM_UPDATE_RATE_STEP_SCALE_Y), 0.1f);
        adjAtToEyeDir.pitch = Camera_LERPCeilS(0x1F4, atToEyeNextDir.pitch, 1.0f / camera->pitchUpdateRateInv, 0xA);
    } else if ((playerPosRot->pos.y - rwData->floorY) < playerHeight) {
        // player is within his height of the ground.
        camera->pitchUpdateRateInv =
            Camera_LERPCeilF(20.0f, camera->pitchUpdateRateInv, CAM_DATA_SCALED(R_CAM_UPDATE_RATE_STEP_SCALE_Y), 0.1f);
        camera->rUpdateRateInv =
            Camera_LERPCeilF(20.0f, camera->rUpdateRateInv, CAM_DATA_SCALED(R_CAM_UPDATE_RATE_STEP_SCALE_Y), 0.1f);
        adjAtToEyeDir.pitch = Camera_LERPCeilS(0x1F4, atToEyeNextDir.pitch, 1.0f / camera->pitchUpdateRateInv, 0xA);
    } else {
        camera->pitchUpdateRateInv = 100.0f;
        camera->rUpdateRateInv = 100.0f;
    }

    // max pitch to +/- ~ 60 degrees
    if (adjAtToEyeDir.pitch > 0x2AF8) {
        adjAtToEyeDir.pitch = 0x2AF8;
    }

    if (adjAtToEyeDir.pitch < -0x2AF8) {
        adjAtToEyeDir.pitch = -0x2AF8;
    }

    Camera_AddVecGeoToVec3f(eyeNext, at, &adjAtToEyeDir);
    camBgChk.pos = *eyeNext;
    if (Camera_BGCheckInfo(camera, at, &camBgChk)) {
        // Collision detected between at->eyeNext, Check if collision between
        // at->eyeNext, but parallel to at (pitch = 0).
        bgChkPos = camBgChk.pos;
        bgChkPara.r = adjAtToEyeDir.r;
        bgChkPara.pitch = 0;
        bgChkPara.yaw = adjAtToEyeDir.yaw;
        Camera_AddVecGeoToVec3f(&camBgChk.pos, at, &bgChkPara);
        if (Camera_BGCheckInfo(camera, at, &camBgChk)) {
            // Collision found between parallel at->eyeNext, set eye position to
            // first collision point.
            *eye = bgChkPos;
        } else {
            // no collision found with the parallel at->eye, animate to be parallel
            adjAtToEyeDir.pitch = Camera_LERPCeilS(0, adjAtToEyeDir.pitch, 0.2f, 0xA);
            Camera_AddVecGeoToVec3f(eye, at, &adjAtToEyeDir);
            // useless?
            Camera_BGCheck(camera, at, eye);
        }
    } else {
        // no collision detected.
        *eye = *eyeNext;
    }

    camera->dist = adjAtToEyeDir.r;
    camera->fov = Camera_LERPCeilF(roData->fovTarget, camera->fov, camera->fovUpdateRate, 1.0f);
    camera->roll = Camera_LERPCeilS(0, camera->roll, 0.5f, 0xA);
    return true;
}

// swimming
s32 Camera_Jump3(Camera* camera) {
    Vec3f* eye = &camera->eye;
    Vec3f* at = &camera->at;
    Vec3f* eyeNext = &camera->eyeNext;
    s32 prevMode;
    f32 spC4;
    f32 spC0;
    f32 spBC;
    UNUSED Vec3f spB0;
    VecGeo eyeDiffGeo;
    PosRot* playerPosRot = &camera->playerPosRot;
    Jump3ReadOnlyData* roData = &camera->paramData.jump3.roData;
    VecGeo eyeAtOffset;
    VecGeo eyeNextAtOffset;
    s32 pad;
    s32 pad2;
    CameraModeValue* values;
    f32 t2;
    f32 phi_f0;
    f32 phi_f2;
    f32 playerHeight;
    PosRot playerhead;
    f32 yNormal;
    f32 temp_f18;
    s32 modeSwitch;
    f32 temp_f2_2;
    Jump3ReadWriteData* rwData = &camera->paramData.jump3.rwData;

    playerHeight = Player_GetHeight(camera->player);
    Actor_GetFocus(&playerhead, &camera->player->actor);

    modeSwitch = false;
    if (((camera->waterYPos - eye->y) < OREG(44) || (camera->animState == 0))) {
        if (rwData->mode != CAM_MODE_NORMAL) {
            rwData->mode = CAM_MODE_NORMAL;
            modeSwitch = true;
        }
    } else if (((camera->waterYPos - eye->y) > OREG(45)) && (rwData->mode != CAM_MODE_AIM_BOOMERANG)) {
        rwData->mode = CAM_MODE_AIM_BOOMERANG;
        modeSwitch = true;
    }

    OLib_Vec3fDiffToVecGeo(&eyeAtOffset, at, eye);
    OLib_Vec3fDiffToVecGeo(&eyeNextAtOffset, at, eyeNext);

    if (RELOAD_PARAMS(camera) || modeSwitch || R_RELOAD_CAM_PARAMS) {
        values = sCameraSettings[camera->setting].cameraModes[rwData->mode].values;
        yNormal =
            1.0f + CAM_DATA_SCALED(R_CAM_YOFFSET_NORM) - (CAM_DATA_SCALED(R_CAM_YOFFSET_NORM) * (68.0f / playerHeight));
        t2 = CAM_DATA_SCALED(playerHeight) * yNormal;
        roData->yOffset = GET_NEXT_RO_DATA(values) * t2;
        roData->distMin = GET_NEXT_RO_DATA(values) * t2;
        roData->distMax = GET_NEXT_RO_DATA(values) * t2;
        roData->pitchTarget = CAM_DEG_TO_BINANG(GET_NEXT_RO_DATA(values));
        roData->swingUpdateRate = GET_NEXT_RO_DATA(values);
        roData->unk_10 = GET_NEXT_RO_DATA(values);
        roData->unk_14 = GET_NEXT_SCALED_RO_DATA(values);
        roData->fovTarget = GET_NEXT_RO_DATA(values);
        roData->unk_1C = GET_NEXT_SCALED_RO_DATA(values);
        roData->interfaceField = GET_NEXT_RO_DATA(values);
    }

    if (R_RELOAD_CAM_PARAMS) {
        prevMode = camera->mode;
        camera->mode = rwData->mode;
        Camera_CopyPREGToModeValues(camera);
        camera->mode = prevMode;
    }

    sCameraInterfaceField = roData->interfaceField;

    switch (camera->animState) {
        case 0:
        case 10:
        case 20:
        case 25:
            rwData->swing.atEyePoly = NULL;
            rwData->unk_1C = camera->playerGroundY;
            rwData->swing.unk_16 = rwData->swing.unk_14 = rwData->swing.unk_18 = 0;
            rwData->animTimer = 10;
            rwData->swing.swingUpdateRate = roData->swingUpdateRate;
            camera->animState++;
            rwData->swing.swingUpdateRateTimer = 0;
            break;
        default:
            if (rwData->animTimer != 0) {
                rwData->animTimer--;
            }
            break;
    }

    spB0 = *eye;

    spC4 = CAM_DATA_SCALED(R_CAM_UPDATE_RATE_STEP_SCALE_XZ) * camera->speedRatio;
    spC0 = camera->speedRatio * CAM_DATA_SCALED(R_CAM_UPDATE_RATE_STEP_SCALE_Y);
    spBC = rwData->swing.unk_18 != 0 ? CAM_DATA_SCALED(R_CAM_UPDATE_RATE_STEP_SCALE_XZ) : spC4;

    if (rwData->swing.swingUpdateRateTimer != 0) {
        camera->yawUpdateRateInv =
            Camera_LERPCeilF(rwData->swing.swingUpdateRate + (rwData->swing.swingUpdateRateTimer * 2),
                             camera->yawUpdateRateInv, spC4, 0.1f);
        camera->pitchUpdateRateInv =
            Camera_LERPCeilF((rwData->swing.swingUpdateRateTimer * 2) + 40.0f, camera->pitchUpdateRateInv, spC0, 0.1f);
        rwData->swing.swingUpdateRateTimer--;
    } else {
        camera->yawUpdateRateInv =
            Camera_LERPCeilF(rwData->swing.swingUpdateRate, camera->yawUpdateRateInv, spBC, 0.1f);
        camera->pitchUpdateRateInv = Camera_LERPCeilF(40.0f, camera->pitchUpdateRateInv, spC0, 0.1f);
    }

    camera->xzOffsetUpdateRate =
        Camera_LERPCeilF(CAM_DATA_SCALED(R_CAM_XZ_OFFSET_UPDATE_RATE), camera->xzOffsetUpdateRate, spC4, 0.1f);
    camera->yOffsetUpdateRate =
        Camera_LERPCeilF(CAM_DATA_SCALED(R_CAM_Y_OFFSET_UPDATE_RATE), camera->yOffsetUpdateRate, spC0, 0.1f);
    camera->fovUpdateRate = Camera_LERPCeilF(CAM_DATA_SCALED(R_CAM_FOV_UPDATE_RATE), camera->yOffsetUpdateRate,
                                             camera->speedRatio * 0.05f, 0.1f);

    Camera_CalcAtDefault(camera, &eyeNextAtOffset, roData->yOffset, roData->interfaceField);
    OLib_Vec3fDiffToVecGeo(&eyeDiffGeo, at, eyeNext);

    camera->dist = eyeDiffGeo.r =
        Camera_ClampDist(camera, eyeDiffGeo.r, roData->distMin, roData->distMax, rwData->animTimer);

    if (camera->playerGroundY <= playerPosRot->pos.y) {
        phi_f0 = playerPosRot->pos.y - camera->playerGroundY;
    } else {
        phi_f0 = -(playerPosRot->pos.y - camera->playerGroundY);
    }

    if (!(phi_f0 < 10.0f)) {
        if (camera->waterYPos <= playerhead.pos.y) {
            phi_f2 = playerhead.pos.y - camera->waterYPos;
        } else {
            phi_f2 = -(playerhead.pos.y - camera->waterYPos);
        }
        if (!(phi_f2 < 50.0f)) {
            camera->pitchUpdateRateInv = 100.0f;
        }
    }
    if (rwData->swing.unk_18 != 0) {
        eyeDiffGeo.yaw =
            Camera_LERPCeilS(rwData->swing.unk_16, eyeNextAtOffset.yaw, 1.0f / camera->yawUpdateRateInv, 0xA);
        eyeDiffGeo.pitch =
            Camera_LERPCeilS(rwData->swing.unk_14, eyeNextAtOffset.pitch, 1.0f / camera->yawUpdateRateInv, 0xA);
    } else {
        eyeDiffGeo.yaw = Camera_CalcDefaultYaw(camera, eyeNextAtOffset.yaw, playerPosRot->rot.y, roData->unk_14, 0.0f);
        eyeDiffGeo.pitch = Camera_CalcDefaultPitch(camera, eyeNextAtOffset.pitch, roData->pitchTarget, 0);
    }

    if (eyeDiffGeo.pitch > R_CAM_MAX_PITCH) {
        eyeDiffGeo.pitch = R_CAM_MAX_PITCH;
    }

    if (eyeDiffGeo.pitch < R_CAM_MIN_PITCH_1) {
        eyeDiffGeo.pitch = R_CAM_MIN_PITCH_1;
    }

    Camera_AddVecGeoToVec3f(eyeNext, at, &eyeDiffGeo);
    if ((camera->status == CAM_STAT_ACTIVE) && !(roData->interfaceField & JUMP3_FLAG_4)) {
        func_80046E20(camera, &eyeDiffGeo, roData->distMin, roData->swingUpdateRate, &spBC, &rwData->swing);
        if (roData->interfaceField & JUMP3_FLAG_2) {
            camera->inputDir.x = -eyeAtOffset.pitch;
            camera->inputDir.y = eyeAtOffset.yaw - 0x7FFF;
            camera->inputDir.z = 0;
        } else {
            OLib_Vec3fDiffToVecGeo(&eyeDiffGeo, eye, at);
            camera->inputDir.x = eyeDiffGeo.pitch;
            camera->inputDir.y = eyeDiffGeo.yaw;
            camera->inputDir.z = 0;
        }

        if (rwData->swing.unk_18 != 0) {
            camera->inputDir.y =
                Camera_LERPCeilS(camera->inputDir.y + (s16)((s16)(rwData->swing.unk_16 - 0x7FFF) - camera->inputDir.y),
                                 camera->inputDir.y, 1.0f - (0.99f * spBC), 0xA);
        }
    } else {
        rwData->swing.swingUpdateRate = roData->swingUpdateRate;
        rwData->swing.unk_18 = 0;
        sUpdateCameraDirection = 0;
        *eye = *eyeNext;
    }
    camera->fov = Camera_LERPCeilF(roData->fovTarget, camera->fov, camera->fovUpdateRate, 1.0f);
    camera->roll = Camera_LERPCeilS(0, camera->roll, 0.5f, 0xA);
    camera->atLERPStepScale = Camera_ClampLERPScale(camera, roData->unk_1C);
    return true;
}

s32 Camera_Jump4(Camera* camera) {
    return Camera_Noop(camera);
}

s32 Camera_Jump0(Camera* camera) {
    return Camera_Noop(camera);
}

s32 Camera_Battle1(Camera* camera) {
    Vec3f* eye = &camera->eye;
    Vec3f* at = &camera->at;
    Vec3f* eyeNext = &camera->eyeNext;
    Vec3f sp128;
    Vec3f playerHead;
    Vec3f targetPos;
    f32 var3;
    f32 var2;
    f32 temp_f0_2;
    f32 temp_f12_2;
    f32 spFC;
    f32 spF8;
    f32 swingAngle;
    f32 temp_f2_2;
    f32 temp_f14;
    s32 skipEyeAtCalc;
    f32 distRatio;
    CamColChk spBC;
    VecGeo spB4;
    VecGeo atToTargetDir;
    VecGeo playerToTargetDir;
    VecGeo atToEyeDir;
    VecGeo atToEyeNextDir;
    PosRot* playerPosRot = &camera->playerPosRot;
    s16 tmpAng1;
    s16 tmpAng2;
    Player* player;
    s16 sp86;
    s16 isOffGround;
    f32 distance;
    f32 sp7C;
    f32 sp78;
    f32 fov;
    Battle1ReadOnlyData* roData = &camera->paramData.batt1.roData;
    Battle1ReadWriteData* rwData = &camera->paramData.batt1.rwData;
    s32 pad;
    f32 playerHeight;

    skipEyeAtCalc = false;
    player = camera->player;
    playerHeight = Player_GetHeight(camera->player);
    if (RELOAD_PARAMS(camera) || R_RELOAD_CAM_PARAMS) {
        CameraModeValue* values = sCameraSettings[camera->setting].cameraModes[camera->mode].values;
        f32 yNormal =
            1.0f + CAM_DATA_SCALED(R_CAM_YOFFSET_NORM) - (CAM_DATA_SCALED(R_CAM_YOFFSET_NORM) * (68.0f / playerHeight));

        roData->yOffset = GET_NEXT_SCALED_RO_DATA(values) * playerHeight * yNormal;
        roData->distance = GET_NEXT_RO_DATA(values);
        roData->swingYawInitial = GET_NEXT_RO_DATA(values);
        roData->swingYawFinal = GET_NEXT_RO_DATA(values);
        roData->swingPitchInitial = GET_NEXT_RO_DATA(values);
        roData->swingPitchFinal = GET_NEXT_RO_DATA(values);
        roData->swingPitchAdj = GET_NEXT_SCALED_RO_DATA(values);
        roData->fov = GET_NEXT_RO_DATA(values);
        roData->atLERPScaleOnGround = GET_NEXT_SCALED_RO_DATA(values);
        roData->interfaceField = GET_NEXT_RO_DATA(values);
        roData->yOffsetOffGround = GET_NEXT_SCALED_RO_DATA(values) * playerHeight * yNormal;
        roData->atLERPScaleOffGround = GET_NEXT_SCALED_RO_DATA(values);
        rwData->chargeTimer = 40;
        rwData->unk_10 = CAM_DATA_SCALED(OREG(12));
    }

    if (R_RELOAD_CAM_PARAMS) {
        Camera_CopyPREGToModeValues(camera);
    }

    distance = roData->distance;
    sp7C = roData->swingPitchInitial;
    sp78 = roData->swingPitchFinal;
    fov = roData->fov;

    if (camera->player->stateFlags1 & PLAYER_STATE1_12) {
        // charging sword.
        rwData->unk_10 = Camera_LERPCeilF(CAM_DATA_SCALED(OREG(12)) * 0.5f, rwData->unk_10,
                                          CAM_DATA_SCALED(R_CAM_UPDATE_RATE_STEP_SCALE_XZ), 0.1f);
        camera->xzOffsetUpdateRate =
            Camera_LERPCeilF(0.2f, camera->xzOffsetUpdateRate, CAM_DATA_SCALED(R_CAM_UPDATE_RATE_STEP_SCALE_XZ), 0.1f);
        camera->yOffsetUpdateRate =
            Camera_LERPCeilF(0.2f, camera->yOffsetUpdateRate, CAM_DATA_SCALED(R_CAM_UPDATE_RATE_STEP_SCALE_XZ), 0.1f);
        if (rwData->chargeTimer > -20) {
            rwData->chargeTimer--;
        } else {
            distance = 250.0f;
            sp7C = 50.0f;
            sp78 = 40.0f;
            fov = 60.0f;
        }
    } else if (rwData->chargeTimer < 0) {
        distance = 250.0f;
        sp7C = 50.0f;
        sp78 = 40.0f;
        fov = 60.0f;
        rwData->chargeTimer++;
    } else {
        rwData->chargeTimer = 40;
        rwData->unk_10 = Camera_LERPCeilF(CAM_DATA_SCALED(OREG(12)), rwData->unk_10,
                                          CAM_DATA_SCALED(R_CAM_UPDATE_RATE_STEP_SCALE_XZ), 0.1f);
        camera->xzOffsetUpdateRate =
            Camera_LERPCeilF(CAM_DATA_SCALED(R_CAM_BATTLE1_XYZ_OFFSET_UPDATE_RATE_TARGET), camera->xzOffsetUpdateRate,
                             CAM_DATA_SCALED(R_CAM_UPDATE_RATE_STEP_SCALE_XZ) * camera->speedRatio, 0.1f);
        camera->yOffsetUpdateRate =
            Camera_LERPCeilF(CAM_DATA_SCALED(R_CAM_BATTLE1_XYZ_OFFSET_UPDATE_RATE_TARGET), camera->yOffsetUpdateRate,
                             CAM_DATA_SCALED(R_CAM_UPDATE_RATE_STEP_SCALE_Y) * camera->speedRatio, 0.1f);
    }
    camera->fovUpdateRate = Camera_LERPCeilF(CAM_DATA_SCALED(R_CAM_FOV_UPDATE_RATE), camera->fovUpdateRate,
                                             camera->speedRatio * 0.05f, 0.1f);
    playerHeight += roData->yOffset;
    OLib_Vec3fDiffToVecGeo(&atToEyeDir, at, eye);
    OLib_Vec3fDiffToVecGeo(&atToEyeNextDir, at, eyeNext);
    if (camera->target == NULL || camera->target->update == NULL) {
        if (camera->target == NULL) {
            osSyncPrintf(
                VT_COL(YELLOW, BLACK) "camera: warning: battle: target is not valid, change parallel\n" VT_RST);
        }
        camera->target = NULL;
        Camera_ChangeMode(camera, CAM_MODE_Z_PARALLEL);
        return true;
    }

    sCameraInterfaceField = roData->interfaceField;

    if (RELOAD_PARAMS(camera)) {
        rwData->unk_14 = 0;
        rwData->roll = 0.0f;
        rwData->target = camera->target;
        camera->animState++;
        if (rwData->target->id > 0) {
            osSyncPrintf("camera: battle: target actor name " VT_FGCOL(BLUE) "%d" VT_RST "\n", rwData->target->id);
        } else {
            osSyncPrintf("camera: battle: target actor name " VT_COL(RED, WHITE) "%d" VT_RST "\n", rwData->target->id);
            camera->target = NULL;
            Camera_ChangeMode(camera, CAM_MODE_Z_PARALLEL);
            return true;
        }
        rwData->animTimer = R_CAM_DEFAULT_ANIM_TIME + OREG(24);
        rwData->initialEyeToAtYaw = atToEyeDir.yaw;
        rwData->initialEyeToAtPitch = atToEyeDir.pitch;
        rwData->initialEyeToAtDist = atToEyeDir.r;
        rwData->yPosOffset = playerPosRot->pos.y - camera->playerPosDelta.y;
    }

    if (camera->status == CAM_STAT_ACTIVE) {
        sUpdateCameraDirection = 1;
        camera->inputDir.x = -atToEyeDir.pitch;
        camera->inputDir.y = atToEyeDir.yaw - 0x7FFF;
        camera->inputDir.z = 0;
    }

    if (camera->playerGroundY == camera->playerPosRot.pos.y || camera->player->actor.gravity > -0.1f ||
        camera->player->stateFlags1 & PLAYER_STATE1_21) {
        isOffGround = false;
        rwData->yPosOffset = playerPosRot->pos.y;
    } else {
        isOffGround = true;
    }

    if (rwData->animTimer == 0) {
        camera->atLERPStepScale =
            Camera_ClampLERPScale(camera, isOffGround ? roData->atLERPScaleOffGround : roData->atLERPScaleOnGround);
    }
    Actor_GetFocus(&camera->targetPosRot, camera->target);
    if (rwData->target != camera->target) {
        osSyncPrintf("camera: battle: change target %d -> " VT_FGCOL(BLUE) "%d" VT_RST "\n", rwData->target->id,
                     camera->target->id);
        camera->animState = 0;
        return true;
    }

    Camera_CalcAtForLockOn(camera, &atToEyeNextDir, &camera->targetPosRot.pos,
                           isOffGround ? roData->yOffsetOffGround : roData->yOffset, distance, &rwData->yPosOffset,
                           &playerToTargetDir, (isOffGround ? 0x81 : 1) | roData->interfaceField);
    tmpAng2 = playerToTargetDir.yaw;
    playerHead = playerPosRot->pos;
    playerHead.y += playerHeight;
    OLib_Vec3fDiffToVecGeo(&playerToTargetDir, &playerHead, &camera->targetPosRot.pos);
    distRatio = playerToTargetDir.r > distance ? 1 : playerToTargetDir.r / distance;
    targetPos = camera->targetPosRot.pos;
    OLib_Vec3fDiffToVecGeo(&atToTargetDir, at, &targetPos);
    atToTargetDir.r = distance - ((atToTargetDir.r <= distance ? atToTargetDir.r : distance) * 0.5f);
    swingAngle = roData->swingYawInitial + ((roData->swingYawFinal - roData->swingYawInitial) * (1.1f - distRatio));
    spF8 = OREG(13) + swingAngle;

    spB4.r = camera->dist = Camera_LERPCeilF(distance, camera->dist, CAM_DATA_SCALED(OREG(11)), 2.0f);
    spB4.yaw = atToEyeNextDir.yaw;
    tmpAng1 = (s16)(atToTargetDir.yaw - (s16)(atToEyeNextDir.yaw - 0x7FFF));
    if (rwData->animTimer != 0) {
        if (rwData->animTimer >= OREG(24)) {
            sp86 = rwData->animTimer - OREG(24);
            OLib_Vec3fDiffToVecGeo(&playerToTargetDir, at, eye);
            playerToTargetDir.yaw = tmpAng2 - 0x7FFF;

            var2 = 1.0f / R_CAM_DEFAULT_ANIM_TIME;
            var3 = (rwData->initialEyeToAtDist - playerToTargetDir.r) * var2;
            tmpAng1 = (s16)(rwData->initialEyeToAtYaw - playerToTargetDir.yaw) * var2;
            tmpAng2 = (s16)(rwData->initialEyeToAtPitch - playerToTargetDir.pitch) * var2;

            spB4.r =
                Camera_LERPCeilF(playerToTargetDir.r + (var3 * sp86), atToEyeDir.r, CAM_DATA_SCALED(OREG(28)), 1.0f);
            spB4.yaw = Camera_LERPCeilS(playerToTargetDir.yaw + (tmpAng1 * sp86), atToEyeDir.yaw,
                                        CAM_DATA_SCALED(OREG(28)), 0xA);
            spB4.pitch = Camera_LERPCeilS(playerToTargetDir.pitch + (tmpAng2 * sp86), atToEyeDir.pitch,
                                          CAM_DATA_SCALED(OREG(28)), 0xA);
        } else {
            skipEyeAtCalc = true;
        }
        rwData->animTimer--;
    } else if (ABS(tmpAng1) > CAM_DEG_TO_BINANG(swingAngle)) {
        spFC = CAM_BINANG_TO_DEG(tmpAng1);
        temp_f2_2 = swingAngle + (spF8 - swingAngle) * (OLib_ClampMaxDist(atToTargetDir.r, spB4.r) / spB4.r);
        temp_f12_2 = ((temp_f2_2 * temp_f2_2) - 2.0f) / (temp_f2_2 - 360.0f);
        var2 = ((temp_f12_2 * spFC) + (2.0f - (360.0f * temp_f12_2)));
        temp_f14 = SQ(spFC) / var2;
        tmpAng2 = tmpAng1 >= 0 ? CAM_DEG_TO_BINANG(temp_f14) : (-CAM_DEG_TO_BINANG(temp_f14));
        spB4.yaw = (s16)((s16)(atToEyeNextDir.yaw - 0x7FFF) + tmpAng2) - 0x7FFF;
    } else {
        spFC = 0.05f;
        spFC = (1 - camera->speedRatio) * spFC;
        tmpAng2 = tmpAng1 >= 0 ? CAM_DEG_TO_BINANG(swingAngle) : -CAM_DEG_TO_BINANG(swingAngle);
        spB4.yaw = atToEyeNextDir.yaw - (s16)((tmpAng2 - tmpAng1) * spFC);
    }

    if (!skipEyeAtCalc) {
        var3 = atToTargetDir.pitch * roData->swingPitchAdj;
        var2 = F32_LERPIMP(sp7C, sp78, distRatio);
        tmpAng1 = CAM_DEG_TO_BINANG(var2) - (s16)(playerToTargetDir.pitch * (0.5f + distRatio * (1.0f - 0.5f)));
        tmpAng1 += (s16)(var3);

        if (tmpAng1 < -0x2AA8) {
            tmpAng1 = -0x2AA8;
        } else if (tmpAng1 > 0x2AA8) {
            tmpAng1 = 0x2AA8;
        }

        spB4.pitch = Camera_LERPCeilS(tmpAng1, atToEyeNextDir.pitch, rwData->unk_10, 0xA);
        Camera_AddVecGeoToVec3f(eyeNext, at, &spB4);
        spBC.pos = *eyeNext;
        if (camera->status == CAM_STAT_ACTIVE) {
            if (!camera->play->envCtx.skyboxDisabled || roData->interfaceField & BATTLE1_FLAG_0) {
                Camera_BGCheckInfo(camera, at, &spBC);
            } else if (roData->interfaceField & BATTLE1_FLAG_1) {
                func_80043F94(camera, at, &spBC);
            } else {
                OLib_Vec3fDistNormalize(&sp128, at, &spBC.pos);
                spBC.pos.x -= sp128.x;
                spBC.pos.y -= sp128.y;
                spBC.pos.z -= sp128.z;
            }
            *eye = spBC.pos;
        } else {
            *eye = *eyeNext;
        }
    }
    rwData->roll += ((R_CAM_BATTLE1_ROLL_TARGET_BASE * camera->speedRatio * (1.0f - distRatio)) - rwData->roll) *
                    CAM_DATA_SCALED(R_CAM_BATTLE1_ROLL_STEP_SCALE);
    camera->roll = CAM_DEG_TO_BINANG(rwData->roll);
    camera->fov = Camera_LERPCeilF((player->meleeWeaponState != 0 ? 0.8f
                                    : gSaveContext.health <= 0x10 ? 0.8f
                                                                  : 1.0f) *
                                       (fov - ((fov * 0.05f) * distRatio)),
                                   camera->fov, camera->fovUpdateRate, 1.0f);
}

s32 Camera_Battle2(Camera* camera) {
    return Camera_Noop(camera);
}

s32 Camera_Battle3(Camera* camera) {
    return Camera_Noop(camera);
}

/**
 * Charging spin attack
 * Camera zooms out slowly for 50 frames, then tilts up to a specified
 * setting value.
 */
s32 Camera_Battle4(Camera* camera) {
    Vec3f* eye = &camera->eye;
    Vec3f* at = &camera->at;
    Vec3f* eyeNext = &camera->eyeNext;
    VecGeo eyeNextOffset;
    VecGeo eyeAtOffset;
    VecGeo eyeNextAtOffset;
    Battle4ReadOnlyData* roData = &camera->paramData.batt4.roData;
    Battle4ReadWriteData* rwData = &camera->paramData.batt4.rwData;
    s32 pad;
    f32 playerHeight;

    playerHeight = Player_GetHeight(camera->player);
    if (RELOAD_PARAMS(camera) || R_RELOAD_CAM_PARAMS) {
        CameraModeValue* values = sCameraSettings[camera->setting].cameraModes[camera->mode].values;
        f32 yNormal =
            1.0f + CAM_DATA_SCALED(R_CAM_YOFFSET_NORM) - (CAM_DATA_SCALED(R_CAM_YOFFSET_NORM) * (68.0f / playerHeight));

        roData->yOffset = GET_NEXT_SCALED_RO_DATA(values) * playerHeight * yNormal;
        roData->rTarget = GET_NEXT_SCALED_RO_DATA(values) * playerHeight * yNormal;
        roData->pitchTarget = CAM_DEG_TO_BINANG(GET_NEXT_RO_DATA(values));
        roData->lerpUpdateRate = GET_NEXT_SCALED_RO_DATA(values);
        roData->fovTarget = GET_NEXT_RO_DATA(values);
        roData->atLERPTarget = GET_NEXT_SCALED_RO_DATA(values);
        roData->interfaceField = GET_NEXT_RO_DATA(values);
    }

    if (R_RELOAD_CAM_PARAMS) {
        Camera_CopyPREGToModeValues(camera);
    }

    OLib_Vec3fDiffToVecGeo(&eyeAtOffset, at, eye);
    OLib_Vec3fDiffToVecGeo(&eyeNextAtOffset, at, eyeNext);

    sCameraInterfaceField = roData->interfaceField;

    switch (camera->animState) {
        case 0:
        case 10:
        case 20:
            rwData->animTimer = 50;
            camera->animState++;
            break;
    }

    camera->yawUpdateRateInv =
        Camera_LERPCeilF(roData->lerpUpdateRate, camera->yawUpdateRateInv,
                         CAM_DATA_SCALED(R_CAM_UPDATE_RATE_STEP_SCALE_XZ) * camera->speedRatio, 0.1f);
    camera->rUpdateRateInv = 1000.0f;
    camera->pitchUpdateRateInv = 1000.0f;
    camera->xzOffsetUpdateRate =
        Camera_LERPCeilF(0.025f, camera->xzOffsetUpdateRate, CAM_DATA_SCALED(R_CAM_UPDATE_RATE_STEP_SCALE_XZ), 0.1f);
    camera->yOffsetUpdateRate =
        Camera_LERPCeilF(CAM_DATA_SCALED(R_CAM_Y_OFFSET_UPDATE_RATE), camera->yOffsetUpdateRate,
                         CAM_DATA_SCALED(R_CAM_UPDATE_RATE_STEP_SCALE_Y) * camera->speedRatio, 0.1f);
    camera->fovUpdateRate = 0.0001f;
    Camera_CalcAtDefault(camera, &eyeNextAtOffset, roData->yOffset, 1);
    if (rwData->animTimer != 0) {
        eyeNextOffset.yaw = eyeAtOffset.yaw;
        eyeNextOffset.pitch = eyeAtOffset.pitch;
        eyeNextOffset.r = eyeAtOffset.r;
        rwData->animTimer--;
    } else {
        eyeNextOffset.yaw = eyeAtOffset.yaw;
        eyeNextOffset.pitch = Camera_LERPCeilS(roData->pitchTarget, eyeAtOffset.pitch, roData->lerpUpdateRate, 2);
        eyeNextOffset.r = Camera_LERPCeilF(roData->rTarget, eyeAtOffset.r, roData->lerpUpdateRate, 0.001f);
    }
    Camera_AddVecGeoToVec3f(eyeNext, at, &eyeNextOffset);
    *eye = *eyeNext;
    camera->dist = eyeNextOffset.r;
    camera->fov = Camera_LERPCeilF(roData->fovTarget, camera->fov, roData->lerpUpdateRate, 1.0f);
    camera->roll = 0;
    camera->atLERPStepScale = Camera_ClampLERPScale(camera, roData->atLERPTarget);
    return true;
}

s32 Camera_Battle0(Camera* camera) {
    return Camera_Noop(camera);
}

// Targeting non-enemy
s32 Camera_KeepOn1(Camera* camera) {
    Vec3f* eye = &camera->eye;
    Vec3f* at = &camera->at;
    Vec3f* eyeNext = &camera->eyeNext;
    Vec3f sp120;
    Vec3f sp114;
    Vec3f sp108;
    f32 sp104;
    f32 temp_f12_2;
    f32 temp_f14;
    f32 t1;
    f32 spF4;
    f32 spF0;
    f32 spEC;
    f32 spE8;
    f32 t2;
    s16 spE2;
    s16 spE0;
    VecGeo spD8;
    VecGeo spD0;
    VecGeo spC8;
    VecGeo spC0;
    VecGeo spB8;
    PosRot* playerPosRot = &camera->playerPosRot;
    CamColChk sp8C;
    s32 sp88;
    f32 sp84;
    s16 sp82;
    s16 sp80;
    KeepOn1ReadOnlyData* roData = &camera->paramData.keep1.roData;
    KeepOn1ReadWriteData* rwData = &camera->paramData.keep1.rwData;
    s16 t3;
    f32 playerHeight;

    sp88 = 0;
    playerHeight = Player_GetHeight(camera->player);
    if ((camera->target == NULL) || (camera->target->update == NULL)) {
        if (camera->target == NULL) {
            osSyncPrintf(
                VT_COL(YELLOW, BLACK) "camera: warning: keepon: target is not valid, change parallel\n" VT_RST);
        }
        camera->target = NULL;
        Camera_ChangeMode(camera, CAM_MODE_Z_PARALLEL);
        return 1;
    }

    if (RELOAD_PARAMS(camera) || R_RELOAD_CAM_PARAMS) {
        CameraModeValue* values = sCameraSettings[camera->setting].cameraModes[camera->mode].values;
        f32 yNormal =
            1.0f + CAM_DATA_SCALED(R_CAM_YOFFSET_NORM) - (CAM_DATA_SCALED(R_CAM_YOFFSET_NORM) * (68.0f / playerHeight));

        roData->unk_00 = GET_NEXT_SCALED_RO_DATA(values) * playerHeight * yNormal;
        roData->unk_04 = GET_NEXT_RO_DATA(values);
        roData->unk_08 = GET_NEXT_RO_DATA(values);
        roData->unk_0C = GET_NEXT_RO_DATA(values);
        roData->unk_10 = GET_NEXT_RO_DATA(values);
        roData->unk_14 = GET_NEXT_RO_DATA(values);
        roData->unk_18 = GET_NEXT_RO_DATA(values);
        roData->unk_1C = GET_NEXT_SCALED_RO_DATA(values);
        roData->unk_20 = GET_NEXT_RO_DATA(values);
        roData->unk_24 = GET_NEXT_SCALED_RO_DATA(values);
        roData->interfaceField = GET_NEXT_RO_DATA(values);
        roData->unk_28 = GET_NEXT_SCALED_RO_DATA(values) * playerHeight * yNormal;
        roData->unk_2C = GET_NEXT_SCALED_RO_DATA(values);
    }
    if (R_RELOAD_CAM_PARAMS) {
        Camera_CopyPREGToModeValues(camera);
    }

    playerHeight += roData->unk_00;
    OLib_Vec3fDiffToVecGeo(&spC0, at, eye);
    OLib_Vec3fDiffToVecGeo(&spB8, at, eyeNext);
    sCameraInterfaceField = roData->interfaceField;
    if (RELOAD_PARAMS(camera)) {
        camera->animState++;
        rwData->unk_10 = 0;
        rwData->unk_04 = 0.0f;
        rwData->unk_0C = camera->target;
        rwData->unk_16 = R_CAM_DEFAULT_ANIM_TIME + OREG(24);
        rwData->unk_12 = spC0.yaw;
        rwData->unk_14 = spC0.pitch;
        rwData->unk_00 = spC0.r;
        rwData->unk_08 = playerPosRot->pos.y - camera->playerPosDelta.y;
    }
    if (camera->status == CAM_STAT_ACTIVE) {
        sUpdateCameraDirection = 1;
        camera->inputDir.x = -spC0.pitch;
        camera->inputDir.y = spC0.yaw - 0x7FFF;
        camera->inputDir.z = 0;
    }

    sp104 = roData->unk_04;
    sp84 = 1;

    switch (camera->viewFlags & (CAM_VIEW_TARGET | CAM_VIEW_TARGET_POS)) {
        case CAM_VIEW_TARGET:
            if ((camera->player->actor.category == 2) && (camera->player->interactRangeActor == camera->target)) {
                PosRot sp54;

                Actor_GetFocus(&sp54, &camera->player->actor);
                spC8.r = 60.0f;
                spC8.yaw = camera->playerPosRot.rot.y;
                spC8.pitch = 0x2EE0;
                Camera_AddVecGeoToVec3f(&camera->targetPosRot.pos, &sp54.pos, &spC8);
            } else {
                Actor_GetFocus(&camera->targetPosRot, camera->target);
            }
            Actor_GetFocus(&camera->targetPosRot, camera->target);
            if (rwData->unk_0C != camera->target) {
                rwData->unk_0C = camera->target;
                camera->atLERPStepScale = 0.0f;
            }
            camera->xzOffsetUpdateRate =
                Camera_LERPCeilF(1.0f, camera->xzOffsetUpdateRate,
                                 CAM_DATA_SCALED(R_CAM_UPDATE_RATE_STEP_SCALE_XZ) * camera->speedRatio, 0.1f);
            camera->yOffsetUpdateRate =
                Camera_LERPCeilF(1.0f, camera->yOffsetUpdateRate,
                                 CAM_DATA_SCALED(R_CAM_UPDATE_RATE_STEP_SCALE_Y) * camera->speedRatio, 0.1f);
            camera->fovUpdateRate = Camera_LERPCeilF(CAM_DATA_SCALED(R_CAM_FOV_UPDATE_RATE), camera->fovUpdateRate,
                                                     camera->speedRatio * 0.05f, 0.1f);
            goto cont;
        case CAM_VIEW_TARGET_POS:
            rwData->unk_0C = NULL;
        cont:
            if (camera->playerGroundY == camera->playerPosRot.pos.y || camera->player->actor.gravity > -0.1f ||
                camera->player->stateFlags1 & PLAYER_STATE1_21) {
                rwData->unk_08 = playerPosRot->pos.y;
                sp80 = 0;
            } else {
                sp80 = 1;
            }

            Camera_CalcAtForLockOn(camera, &spB8, &camera->targetPosRot.pos, sp80 ? roData->unk_28 : roData->unk_00,
                                   sp104, &rwData->unk_08, &spC8, (sp80 ? 0x80 : 0) | roData->interfaceField);
            sp114 = playerPosRot->pos;
            sp114.y += playerHeight;
            OLib_Vec3fDiffToVecGeo(&spC8, &sp114, &camera->targetPosRot.pos);
            sp84 = spC8.r > sp104 ? 1.0f : spC8.r / sp104;
            break;
        default:
            *at = playerPosRot->pos;
            at->y += playerHeight;
            rwData->unk_0C = NULL;
            break;
    }
    OLib_Vec3fDiffToVecGeo(&spD8, at, eyeNext);
    if (spD8.r < roData->unk_04) {
        sp104 = roData->unk_04;
        spE8 = R_CAM_R_UPDATE_RATE_INV;
    } else if (roData->unk_08 < spD8.r) {
        sp104 = roData->unk_08;
        spE8 = R_CAM_R_UPDATE_RATE_INV;
    } else {
        sp104 = spD8.r;
        spE8 = 1.0f;
    }

    camera->rUpdateRateInv =
        Camera_LERPCeilF(spE8, camera->rUpdateRateInv, CAM_DATA_SCALED(R_CAM_UPDATE_RATE_STEP_SCALE_XZ), 0.1f);
    spD8.r = spE8 = camera->dist = Camera_LERPCeilF(sp104, camera->dist, 1.0f / camera->rUpdateRateInv, 0.2f);
    sp108 = camera->targetPosRot.pos;
    OLib_Vec3fDiffToVecGeo(&spD0, at, &sp108);
    spD0.r = spE8 - ((spD0.r <= spE8 ? spD0.r : spE8) * 0.5f);
    spEC = roData->unk_0C + ((roData->unk_10 - roData->unk_0C) * (1.1f - sp84));
    spF0 = OREG(13) + spEC;
    spD8.r = camera->dist = Camera_LERPCeilF(spE8, camera->dist, CAM_DATA_SCALED(OREG(11)), 2.0f);
    spD8.yaw = spB8.yaw;
    spE2 = spD0.yaw - (s16)(spB8.yaw - 0x7FFF);
    if (rwData->unk_16 != 0) {
        if (rwData->unk_16 >= OREG(24)) {
            sp82 = rwData->unk_16 - OREG(24);
            spE2 = spC8.yaw;
            OLib_Vec3fDiffToVecGeo(&spC8, at, eye);
            spC8.yaw = spE2 - 0x7FFF;

            t2 = 1.0f / R_CAM_DEFAULT_ANIM_TIME;
            spE8 = (rwData->unk_00 - spC8.r) * t2;
            spE2 = (s16)(rwData->unk_12 - spC8.yaw) * t2;
            spE0 = (s16)(rwData->unk_14 - spC8.pitch) * t2;

            spD8.r = Camera_LERPCeilF(spC8.r + (spE8 * sp82), spC0.r, CAM_DATA_SCALED(OREG(28)), 1.0f);
            spD8.yaw = Camera_LERPCeilS(spC8.yaw + (spE2 * sp82), spC0.yaw, CAM_DATA_SCALED(OREG(28)), 0xA);
            spD8.pitch = Camera_LERPCeilS(spC8.pitch + (spE0 * sp82), spC0.pitch, CAM_DATA_SCALED(OREG(28)), 0xA);
        } else {
            sp88 = 1;
        }
        rwData->unk_16--;
    } else if (ABS(spE2) > CAM_DEG_TO_BINANG(spEC)) {
        spF4 = CAM_BINANG_TO_DEG(spE2);
        t2 = spEC + (spF0 - spEC) * (OLib_ClampMaxDist(spD0.r, spD8.r) / spD8.r);
        temp_f12_2 = ((SQ(t2) - 2.0f) / (t2 - 360.0f));
        t1 = (temp_f12_2 * spF4) + (2.0f - (360.0f * temp_f12_2));
        temp_f14 = SQ(spF4) / t1;
        spE0 = spE2 >= 0 ? (CAM_DEG_TO_BINANG(temp_f14)) : (-CAM_DEG_TO_BINANG(temp_f14));
        spD8.yaw = (s16)((s16)(spB8.yaw - 0x7FFF) + spE0) - 0x7FFF;
    } else {
        spF4 = 0.02f;
        spF4 = (1.0f - camera->speedRatio) * spF4;
        spE0 = spE2 >= 0 ? CAM_DEG_TO_BINANG(spEC) : -CAM_DEG_TO_BINANG(spEC);
        spD8.yaw = spB8.yaw - (s16)((spE0 - spE2) * spF4);
    }

    if (sp88 == 0) {
        spE2 = CAM_DEG_TO_BINANG((f32)(roData->unk_14 + ((roData->unk_18 - roData->unk_14) * sp84)));
        spE2 -= (s16)(spC8.pitch * (0.5f + (sp84 * 0.5f)));

        spE8 = spD0.pitch * roData->unk_1C;
        spE2 += (s16)spE8;
        if (spE2 < -0x3200) {
            spE2 = -0x3200;
        } else if (spE2 > 0x3200) {
            spE2 = 0x3200;
        }

        spD8.pitch = Camera_LERPCeilS(spE2, spB8.pitch, CAM_DATA_SCALED(OREG(12)), 0xA);
        Camera_AddVecGeoToVec3f(eyeNext, at, &spD8);
        sp8C.pos = *eyeNext;
        if (camera->status == CAM_STAT_ACTIVE) {
            if (!camera->play->envCtx.skyboxDisabled || roData->interfaceField & KEEPON1_FLAG_0) {
                Camera_BGCheckInfo(camera, at, &sp8C);
            } else if (roData->interfaceField & KEEPON1_FLAG_1) {
                func_80043F94(camera, at, &sp8C);
            } else {
                OLib_Vec3fDistNormalize(&sp120, at, &sp8C.pos);
                sp8C.pos.x -= sp120.x;
                sp8C.pos.y -= sp120.y;
                sp8C.pos.z -= sp120.z;
            }
            *eye = sp8C.pos;
        } else {
            *eye = *eyeNext;
        }
        OLib_Vec3fDistNormalize(&sp120, eye, at);
        Camera_Vec3fTranslateByUnitVector(eye, eye, &sp120, OREG(1));
    }
    camera->fov = Camera_LERPCeilF(roData->unk_20, camera->fov, camera->fovUpdateRate, 1.0f);
    camera->roll = Camera_LERPCeilS(0, camera->roll, 0.5f, 0xA);
    camera->atLERPStepScale = Camera_ClampLERPScale(camera, sp80 ? roData->unk_2C : roData->unk_24);
    return 1;
}

s32 Camera_KeepOn2(Camera* camera) {
    return Camera_Noop(camera);
}

/**
 * Talking to an NPC
 */
s32 Camera_KeepOn3(Camera* camera) {
    Vec3f* eye = &camera->eye;
    Vec3f* at = &camera->at;
    Vec3f* eyeNext = &camera->eyeNext;
    Vec3f playerHeadPos;
    Vec3f lineChkPointB;
    f32 temp_f0;
    f32 spBC;
    f32 prevTargetPlayerDist;
    f32 swingAngle;
    Actor* colChkActors[2];
    VecGeo targetToPlayerDir;
    VecGeo atToEyeAdj;
    VecGeo atToEyeDir;
    VecGeo atToEyeNextDir;
    s32 i;
    s32 angleCnt;
    s16 sp82;
    s16 sp80;
    PosRot playerPosRot;
    PosRot* camPlayerPosRot = &camera->playerPosRot;
    KeepOn3ReadOnlyData* roData = &camera->paramData.keep3.roData;
    KeepOn3ReadWriteData* rwData = &camera->paramData.keep3.rwData;
    s32 pad;
    f32 playerHeight;

    playerHeight = Player_GetHeight(camera->player);
    if (camera->target == NULL || camera->target->update == NULL) {
        if (camera->target == NULL) {
            osSyncPrintf(VT_COL(YELLOW, BLACK) "camera: warning: talk: target is not valid, change parallel\n" VT_RST);
        }
        camera->target = NULL;
        Camera_ChangeMode(camera, CAM_MODE_Z_PARALLEL);
        return 1;
    }
    if (RELOAD_PARAMS(camera)) {
        if (camera->play->view.unk_124 == 0) {
            camera->stateFlags |= CAM_STATE_5;
            camera->play->view.unk_124 = camera->camId | 0x50;
            return 1;
        }
        camera->stateFlags &= ~CAM_STATE_5;
    }
    camera->stateFlags &= ~CAM_STATE_4;
    if (RELOAD_PARAMS(camera) || R_RELOAD_CAM_PARAMS) {
        CameraModeValue* values = sCameraSettings[camera->setting].cameraModes[camera->mode].values;
        f32 yNormal =
            1.0f + CAM_DATA_SCALED(R_CAM_YOFFSET_NORM) - (CAM_DATA_SCALED(R_CAM_YOFFSET_NORM) * (68.0f / playerHeight));

        roData->yOffset = GET_NEXT_SCALED_RO_DATA(values) * playerHeight * yNormal;
        roData->minDist = GET_NEXT_RO_DATA(values);
        roData->maxDist = GET_NEXT_RO_DATA(values);
        roData->swingYawInital = GET_NEXT_RO_DATA(values);
        roData->swingYawFinal = GET_NEXT_RO_DATA(values);
        roData->swingPitchInitial = GET_NEXT_RO_DATA(values);
        roData->swingPitchFinal = GET_NEXT_RO_DATA(values);
        roData->swingPitchAdj = GET_NEXT_SCALED_RO_DATA(values);
        roData->fovTarget = GET_NEXT_RO_DATA(values);
        roData->atLERPScaleMax = GET_NEXT_SCALED_RO_DATA(values);
        roData->initTimer = GET_NEXT_RO_DATA(values);
        roData->interfaceField = GET_NEXT_RO_DATA(values);
    }

    if (R_RELOAD_CAM_PARAMS) {
        Camera_CopyPREGToModeValues(camera);
    }

    playerHeight += roData->yOffset;
    OLib_Vec3fDiffToVecGeo(&atToEyeDir, at, eye);
    OLib_Vec3fDiffToVecGeo(&atToEyeNextDir, at, eyeNext);
    Actor_GetFocus(&camera->targetPosRot, camera->target);
    Actor_GetFocus(&playerPosRot, &camera->player->actor);
    playerHeadPos = camPlayerPosRot->pos;
    playerHeadPos.y += playerHeight;
    OLib_Vec3fDiffToVecGeo(&targetToPlayerDir, &playerHeadPos, &camera->targetPosRot.pos);
    sCameraInterfaceField = roData->interfaceField;
    if (RELOAD_PARAMS(camera)) {
        colChkActors[0] = camera->target;
        colChkActors[1] = &camera->player->actor;
        camera->animState++;
        rwData->target = camera->target;
        temp_f0 = (roData->maxDist < targetToPlayerDir.r ? 1.0f : targetToPlayerDir.r / roData->maxDist);
        rwData->animTimer = roData->initTimer;
        spBC = ((1.0f - temp_f0) * targetToPlayerDir.r) / rwData->animTimer;
        swingAngle = F32_LERPIMP(roData->swingPitchInitial, roData->swingPitchFinal, temp_f0);
        atToEyeAdj.pitch = CAM_DEG_TO_BINANG(swingAngle) + ((s16)(-(targetToPlayerDir.pitch * roData->swingPitchAdj)));
        swingAngle = F32_LERPIMP(roData->swingYawInital, roData->swingYawFinal, temp_f0);
        if (roData->interfaceField & KEEPON3_FLAG_4) {
            if ((s16)(targetToPlayerDir.yaw - atToEyeNextDir.yaw) < 0) {
                atToEyeAdj.yaw = targetToPlayerDir.yaw + CAM_DEG_TO_BINANG(swingAngle);
            } else {
                atToEyeAdj.yaw = targetToPlayerDir.yaw - CAM_DEG_TO_BINANG(swingAngle);
            }
        } else if (roData->interfaceField & KEEPON3_FLAG_5) {
            if ((s16)(targetToPlayerDir.yaw - atToEyeNextDir.yaw) < 0) {
                atToEyeAdj.yaw = (s16)(targetToPlayerDir.yaw - 0x7FFF) - CAM_DEG_TO_BINANG(swingAngle);
            } else {
                atToEyeAdj.yaw = (s16)(targetToPlayerDir.yaw - 0x7FFF) + CAM_DEG_TO_BINANG(swingAngle);
            }
        } else if (ABS((s16)(targetToPlayerDir.yaw - atToEyeNextDir.yaw)) < 0x3FFF) {
            if ((s16)(targetToPlayerDir.yaw - atToEyeNextDir.yaw) < 0) {
                atToEyeAdj.yaw = targetToPlayerDir.yaw + CAM_DEG_TO_BINANG(swingAngle);
            } else {
                atToEyeAdj.yaw = targetToPlayerDir.yaw - CAM_DEG_TO_BINANG(swingAngle);
            }
        } else {
            if ((s16)(targetToPlayerDir.yaw - atToEyeNextDir.yaw) < 0) {
                atToEyeAdj.yaw = (s16)(targetToPlayerDir.yaw - 0x7FFF) - CAM_DEG_TO_BINANG(swingAngle);
            } else {
                atToEyeAdj.yaw = (s16)(targetToPlayerDir.yaw - 0x7FFF) + CAM_DEG_TO_BINANG(swingAngle);
            }
        }
        prevTargetPlayerDist = targetToPlayerDir.r;
        temp_f0 = 0.6f;
        targetToPlayerDir.r = (spBC * 0.6f) + (prevTargetPlayerDist * (1.0f - temp_f0));
        sp80 = atToEyeAdj.yaw;
        sp82 = atToEyeAdj.pitch;
        playerHeadPos = camPlayerPosRot->pos;
        playerHeadPos.y += playerHeight;
        Camera_AddVecGeoToVec3f(&rwData->atTarget, &playerHeadPos, &targetToPlayerDir);
        angleCnt = ARRAY_COUNT(D_8011D3B0);
        i = 0;
        targetToPlayerDir.r = prevTargetPlayerDist;
        atToEyeAdj.r = roData->minDist + (targetToPlayerDir.r * (1 - 0.5f)) - atToEyeNextDir.r + atToEyeNextDir.r;
        Camera_AddVecGeoToVec3f(&lineChkPointB, &rwData->atTarget, &atToEyeAdj);
        if (!(roData->interfaceField & KEEPON3_FLAG_7)) {
            while (i < angleCnt) {
                if (!CollisionCheck_LineOCCheck(camera->play, &camera->play->colChkCtx, &rwData->atTarget,
                                                &lineChkPointB, colChkActors, 2) &&
                    !Camera_BGCheck(camera, &rwData->atTarget, &lineChkPointB)) {
                    break;
                }
                atToEyeAdj.yaw = sp80 + D_8011D3B0[i];
                atToEyeAdj.pitch = sp82 + D_8011D3CC[i];
                Camera_AddVecGeoToVec3f(&lineChkPointB, &rwData->atTarget, &atToEyeAdj);
                i++;
            }
        }
        osSyncPrintf("camera: talk: BG&collision check %d time(s)\n", i);
        camera->stateFlags &= ~(CAM_STATE_2 | CAM_STATE_3);
        pad = ((rwData->animTimer + 1) * rwData->animTimer) >> 1;
        rwData->eyeToAtTargetYaw = (f32)(s16)(atToEyeAdj.yaw - atToEyeNextDir.yaw) / pad;
        rwData->eyeToAtTargetPitch = (f32)(s16)(atToEyeAdj.pitch - atToEyeNextDir.pitch) / pad;
        rwData->eyeToAtTargetR = (atToEyeAdj.r - atToEyeNextDir.r) / pad;
        return 1;
    }

    if (rwData->animTimer != 0) {
        at->x += (rwData->atTarget.x - at->x) / rwData->animTimer;
        at->y += (rwData->atTarget.y - at->y) / rwData->animTimer;
        at->z += (rwData->atTarget.z - at->z) / rwData->animTimer;
        // needed to match
        if (!prevTargetPlayerDist) {}
        atToEyeAdj.r = ((rwData->eyeToAtTargetR * rwData->animTimer) + atToEyeNextDir.r) + 1.0f;
        atToEyeAdj.yaw = atToEyeNextDir.yaw + (s16)(rwData->eyeToAtTargetYaw * rwData->animTimer);
        atToEyeAdj.pitch = atToEyeNextDir.pitch + (s16)(rwData->eyeToAtTargetPitch * rwData->animTimer);
        Camera_AddVecGeoToVec3f(eyeNext, at, &atToEyeAdj);
        *eye = *eyeNext;
        camera->fov = Camera_LERPCeilF(roData->fovTarget, camera->fov, 0.5, 1.0f);
        camera->roll = Camera_LERPCeilS(0, camera->roll, 0.5, 0xA);
        camera->atLERPStepScale = Camera_ClampLERPScale(camera, roData->atLERPScaleMax);
        Camera_BGCheck(camera, at, eye);
        rwData->animTimer--;
    } else {
        camera->stateFlags |= (CAM_STATE_4 | CAM_STATE_10);
    }

    if (camera->stateFlags & CAM_STATE_3) {
        sCameraInterfaceField = CAM_INTERFACE_FIELD(CAM_LETTERBOX_NONE, CAM_HUD_VISIBILITY_ALL, 0);
        func_80043B60(camera);
        camera->atLERPStepScale = 0.0f;

        if (camera->xzSpeed > 0.001f || CHECK_BTN_ALL(D_8015BD7C->state.input[0].press.button, BTN_A) ||
            CHECK_BTN_ALL(D_8015BD7C->state.input[0].press.button, BTN_B) ||
            CHECK_BTN_ALL(D_8015BD7C->state.input[0].press.button, BTN_CLEFT) ||
            CHECK_BTN_ALL(D_8015BD7C->state.input[0].press.button, BTN_CDOWN) ||
            CHECK_BTN_ALL(D_8015BD7C->state.input[0].press.button, BTN_CUP) ||
            CHECK_BTN_ALL(D_8015BD7C->state.input[0].press.button, BTN_CRIGHT) ||
            CHECK_BTN_ALL(D_8015BD7C->state.input[0].press.button, BTN_R) ||
            CHECK_BTN_ALL(D_8015BD7C->state.input[0].press.button, BTN_Z)) {
            camera->stateFlags |= CAM_STATE_2;
            camera->stateFlags &= ~CAM_STATE_3;
        }
    }
    return 1;
}

s32 Camera_KeepOn4(Camera* camera) {
    static Vec3f D_8015BD50;
    static Vec3f D_8015BD60;
    static Vec3f D_8015BD70;
    Vec3f* eye = &camera->eye;
    Vec3f* at = &camera->at;
    Vec3f* eyeNext = &camera->eyeNext;
    Actor* spCC[2];
    f32 t = -0.5f;
    f32 temp_f0_2;
    CollisionPoly* spC0;
    VecGeo spB8;
    VecGeo spB0;
    VecGeo spA8;
    s16* temp_s0 = &camera->data2;
    s16 spA2;
    s16 spA0;
    s16 sp9E;
    s16 sp9C;
    PosRot* playerPosRot = &camera->playerPosRot;
    KeepOn4ReadOnlyData* roData = &camera->paramData.keep4.roData;
    KeepOn4ReadWriteData* rwData = &camera->paramData.keep4.rwData;
    s32 pad;
    f32 playerHeight;
    Player* player = GET_PLAYER(camera->play);
    s16 angleCnt;
    s32 i;

    if (RELOAD_PARAMS(camera)) {
        if (camera->play->view.unk_124 == 0) {
            camera->stateFlags |= CAM_STATE_5;
            camera->stateFlags &= ~(CAM_STATE_1 | CAM_STATE_2);
            camera->play->view.unk_124 = camera->camId | 0x50;
            return 1;
        }
        rwData->unk_14 = *temp_s0;
        camera->stateFlags &= ~CAM_STATE_5;
    }

    if (rwData->unk_14 != *temp_s0) {
        osSyncPrintf(VT_COL(YELLOW, BLACK) "camera: item: item type changed %d -> %d\n" VT_RST, rwData->unk_14,
                     *temp_s0);
        camera->animState = 20;
        camera->stateFlags |= CAM_STATE_5;
        camera->stateFlags &= ~(CAM_STATE_1 | CAM_STATE_2);
        camera->play->view.unk_124 = camera->camId | 0x50;
        return 1;
    }

    playerHeight = Player_GetHeight(camera->player);
    camera->stateFlags &= ~CAM_STATE_4;
    if (RELOAD_PARAMS(camera) || R_RELOAD_CAM_PARAMS) {
        CameraModeValue* values = sCameraSettings[camera->setting].cameraModes[camera->mode].values;
        f32 yNormal = 1.0f + t - (68.0f / playerHeight * t);

        roData->unk_00 = GET_NEXT_SCALED_RO_DATA(values) * playerHeight * yNormal;
        roData->unk_04 = GET_NEXT_SCALED_RO_DATA(values) * playerHeight * yNormal;
        roData->unk_08 = GET_NEXT_RO_DATA(values);
        roData->unk_0C = GET_NEXT_RO_DATA(values);
        roData->unk_10 = GET_NEXT_RO_DATA(values);
        roData->unk_18 = GET_NEXT_RO_DATA(values);
        roData->interfaceField = GET_NEXT_RO_DATA(values);
        roData->unk_14 = GET_NEXT_SCALED_RO_DATA(values);
        roData->unk_1E = GET_NEXT_RO_DATA(values);
        osSyncPrintf("camera: item: type %d\n", *temp_s0);
        switch (*temp_s0) {
            case 1:
                roData->unk_00 = playerHeight * -0.6f * yNormal;
                roData->unk_04 = playerHeight * 2.0f * yNormal;
                roData->unk_08 = 10.0f;
                break;

            case 2:
            case 3:
                roData->unk_08 = -20.0f;
                roData->unk_18 = 80.0f;
                break;

            case 4:
                roData->unk_00 = playerHeight * -0.2f * yNormal;
                roData->unk_08 = 25.0f;
                break;

            case 8:
                roData->unk_00 = playerHeight * -0.2f * yNormal;
                roData->unk_04 = playerHeight * 0.8f * yNormal;
                roData->unk_08 = 50.0f;
                roData->unk_18 = 70.0f;
                break;

            case 9:
                roData->unk_00 = playerHeight * 0.1f * yNormal;
                roData->unk_04 = playerHeight * 0.5f * yNormal;
                roData->unk_08 = -20.0f;
                roData->unk_0C = 0.0f;
                roData->interfaceField =
                    CAM_INTERFACE_FIELD(CAM_LETTERBOX_MEDIUM, CAM_HUD_VISIBILITY_A_HEARTS_MAGIC_FORCE, KEEPON4_FLAG_6);
                break;

            case 5:
                roData->unk_00 = playerHeight * -0.4f * yNormal;
                roData->unk_08 = -10.0f;
                roData->unk_0C = 45.0f;
                roData->interfaceField =
                    CAM_INTERFACE_FIELD(CAM_LETTERBOX_MEDIUM, CAM_HUD_VISIBILITY_ALL, KEEPON4_FLAG_1);
                break;

            case 10:
                roData->unk_00 = playerHeight * -0.5f * yNormal;
                roData->unk_04 = playerHeight * 1.5f * yNormal;
                roData->unk_08 = -15.0f;
                roData->unk_0C = 175.0f;
                roData->unk_18 = 70.0f;
                roData->interfaceField =
                    CAM_INTERFACE_FIELD(CAM_LETTERBOX_MEDIUM, CAM_HUD_VISIBILITY_NOTHING_ALT, KEEPON4_FLAG_1);
                roData->unk_1E = 0x3C;
                break;

            case 12:
                roData->unk_00 = playerHeight * -0.6f * yNormal;
                roData->unk_04 = playerHeight * 1.6f * yNormal;
                roData->unk_08 = -2.0f;
                roData->unk_0C = 120.0f;
                roData->unk_10 = player->stateFlags1 & PLAYER_STATE1_27 ? 0.0f : 20.0f;
                roData->interfaceField = CAM_INTERFACE_FIELD(CAM_LETTERBOX_LARGE, CAM_HUD_VISIBILITY_NOTHING_ALT,
                                                             KEEPON4_FLAG_4 | KEEPON4_FLAG_1);
                roData->unk_1E = 0x1E;
                roData->unk_18 = 50.0f;
                break;

            case 0x5A:
                roData->unk_00 = playerHeight * -0.3f * yNormal;
                roData->unk_18 = 45.0f;
                roData->interfaceField =
                    CAM_INTERFACE_FIELD(CAM_LETTERBOX_MEDIUM, CAM_HUD_VISIBILITY_IGNORE, KEEPON4_FLAG_1);
                break;

            case 0x5B:
                roData->unk_00 = playerHeight * -0.1f * yNormal;
                roData->unk_04 = playerHeight * 1.5f * yNormal;
                roData->unk_08 = -3.0f;
                roData->unk_0C = 10.0f;
                roData->unk_18 = 55.0f;
                roData->interfaceField =
                    CAM_INTERFACE_FIELD(CAM_LETTERBOX_MEDIUM, CAM_HUD_VISIBILITY_IGNORE, KEEPON4_FLAG_3);
                break;

            case 0x51:
                roData->unk_00 = playerHeight * -0.3f * yNormal;
                roData->unk_04 = playerHeight * 1.5f * yNormal;
                roData->unk_08 = 2.0f;
                roData->unk_0C = 20.0f;
                roData->unk_10 = 20.0f;
                roData->interfaceField =
                    CAM_INTERFACE_FIELD(CAM_LETTERBOX_MEDIUM, CAM_HUD_VISIBILITY_NOTHING_ALT, KEEPON4_FLAG_7);
                roData->unk_1E = 0x1E;
                roData->unk_18 = 45.0f;
                break;

            case 11:
                roData->unk_00 = playerHeight * -0.19f * yNormal;
                roData->unk_04 = playerHeight * 0.7f * yNormal;
                roData->unk_0C = 130.0f;
                roData->unk_10 = 10.0f;
                roData->interfaceField = CAM_INTERFACE_FIELD(
                    CAM_LETTERBOX_MEDIUM, CAM_HUD_VISIBILITY_A_HEARTS_MAGIC_FORCE, KEEPON4_FLAG_5 | KEEPON4_FLAG_1);
                break;

            default:
                break;
        }
    }

    if (R_RELOAD_CAM_PARAMS) {
        Camera_CopyPREGToModeValues(camera);
    }

    sUpdateCameraDirection = 1;
    sCameraInterfaceField = roData->interfaceField;
    OLib_Vec3fDiffToVecGeo(&spB0, at, eye);
    OLib_Vec3fDiffToVecGeo(&spA8, at, eyeNext);
    D_8015BD50 = playerPosRot->pos;
    D_8015BD50.y += playerHeight;
    temp_f0_2 = BgCheck_CameraRaycastDown2(&camera->play->colCtx, &spC0, &i, &D_8015BD50);
    if (temp_f0_2 > (roData->unk_00 + D_8015BD50.y)) {
        D_8015BD50.y = temp_f0_2 + 10.0f;
    } else {
        D_8015BD50.y += roData->unk_00;
    }

    sp9C = 0;
    switch (camera->animState) {
        case 0:
        case 20:
            spCC[sp9C] = &camera->player->actor;
            sp9C++;
            func_80043ABC(camera);
            camera->stateFlags &= ~(CAM_STATE_1 | CAM_STATE_2);
            rwData->unk_10 = roData->unk_1E;
            rwData->unk_08 = playerPosRot->pos.y - camera->playerPosDelta.y;
            if (roData->interfaceField & KEEPON4_FLAG_1) {
                spA2 = CAM_DEG_TO_BINANG(roData->unk_08);
                spA0 = (s16)((s16)(playerPosRot->rot.y - 0x7FFF) - spA8.yaw) > 0
                           ? (s16)(playerPosRot->rot.y - 0x7FFF) + CAM_DEG_TO_BINANG(roData->unk_0C)
                           : (s16)(playerPosRot->rot.y - 0x7FFF) - CAM_DEG_TO_BINANG(roData->unk_0C);
            } else if (roData->interfaceField & KEEPON4_FLAG_2) {
                spA2 = CAM_DEG_TO_BINANG(roData->unk_08);
                spA0 = CAM_DEG_TO_BINANG(roData->unk_0C);
            } else if ((roData->interfaceField & KEEPON4_FLAG_3) && camera->target != NULL) {
                PosRot sp60;

                Actor_GetWorldPosShapeRot(&sp60, camera->target);
                spA2 = CAM_DEG_TO_BINANG(roData->unk_08) - sp60.rot.x;
                spA0 = (s16)((s16)(sp60.rot.y - 0x7FFF) - spA8.yaw) > 0
                           ? (s16)(sp60.rot.y - 0x7FFF) + CAM_DEG_TO_BINANG(roData->unk_0C)
                           : (s16)(sp60.rot.y - 0x7FFF) - CAM_DEG_TO_BINANG(roData->unk_0C);
                spCC[1] = camera->target;
                sp9C++;
            } else if ((roData->interfaceField & KEEPON4_FLAG_7) && camera->target != NULL) {
                PosRot sp4C;

                Actor_GetWorld(&sp4C, camera->target);
                spA2 = CAM_DEG_TO_BINANG(roData->unk_08);
                sp9E = Camera_XZAngle(&sp4C.pos, &playerPosRot->pos);
                spA0 = ((s16)(sp9E - spA8.yaw) > 0) ? sp9E + CAM_DEG_TO_BINANG(roData->unk_0C)
                                                    : sp9E - CAM_DEG_TO_BINANG(roData->unk_0C);
                spCC[1] = camera->target;
                sp9C++;
            } else if (roData->interfaceField & KEEPON4_FLAG_6) {
                spA2 = CAM_DEG_TO_BINANG(roData->unk_08);
                spA0 = spA8.yaw;
            } else {
                spA2 = spA8.pitch;
                spA0 = spA8.yaw;
            }

            spB8.pitch = spA2;
            spB8.yaw = spA0;
            spB8.r = roData->unk_04;
            Camera_AddVecGeoToVec3f(&D_8015BD70, &D_8015BD50, &spB8);
            if (!(roData->interfaceField & KEEPON4_FLAG_0)) {
                angleCnt = ARRAY_COUNT(D_8011D3B0);
                for (i = 0; i < angleCnt; i++) {
                    if (!CollisionCheck_LineOCCheck(camera->play, &camera->play->colChkCtx, &D_8015BD50, &D_8015BD70,
                                                    spCC, sp9C) &&
                        !Camera_BGCheck(camera, &D_8015BD50, &D_8015BD70)) {
                        break;
                    }
                    spB8.yaw = D_8011D3B0[i] + spA0;
                    spB8.pitch = D_8011D3CC[i] + spA2;
                    Camera_AddVecGeoToVec3f(&D_8015BD70, &D_8015BD50, &spB8);
                }
                osSyncPrintf("camera: item: BG&collision check %d time(s)\n", i);
            }
            rwData->unk_04 = (s16)(spB8.pitch - spA8.pitch) / (f32)rwData->unk_10;
            rwData->unk_00 = (s16)(spB8.yaw - spA8.yaw) / (f32)rwData->unk_10;
            rwData->unk_0C = spA8.yaw;
            rwData->unk_0E = spA8.pitch;
            camera->animState++;
            rwData->unk_12 = 1;
            break;
        case 10:
            rwData->unk_08 = playerPosRot->pos.y - camera->playerPosDelta.y;
        default:
            break;
    }
    camera->xzOffsetUpdateRate = 0.25f;
    camera->yOffsetUpdateRate = 0.25f;
    camera->atLERPStepScale = 0.75f;
    Camera_LERPCeilVec3f(&D_8015BD50, at, 0.5f, 0.5f, 0.2f);
    if (roData->unk_10 != 0.0f) {
        spB8.r = roData->unk_10;
        spB8.pitch = 0;
        spB8.yaw = playerPosRot->rot.y;
        Camera_AddVecGeoToVec3f(at, at, &spB8);
    }
    camera->atLERPStepScale = 0.0f;
    camera->dist = Camera_LERPCeilF(roData->unk_04, camera->dist, 0.25f, 2.0f);
    spB8.r = camera->dist;
    if (rwData->unk_10 != 0) {
        camera->stateFlags |= CAM_STATE_5;
        rwData->unk_0C += (s16)rwData->unk_00;
        rwData->unk_0E += (s16)rwData->unk_04;
        rwData->unk_10--;
    } else if (roData->interfaceField & KEEPON4_FLAG_4) {
        camera->stateFlags |= (CAM_STATE_4 | CAM_STATE_10);
        camera->stateFlags |= (CAM_STATE_1 | CAM_STATE_2);
        camera->stateFlags &= ~CAM_STATE_3;
        if (camera->timer > 0) {
            camera->timer--;
        }
    } else {
        camera->stateFlags |= (CAM_STATE_4 | CAM_STATE_10);
        if ((camera->stateFlags & CAM_STATE_3) || (roData->interfaceField & KEEPON4_FLAG_7)) {
            sCameraInterfaceField = CAM_INTERFACE_FIELD(CAM_LETTERBOX_NONE, CAM_HUD_VISIBILITY_ALL, 0);
            camera->stateFlags |= (CAM_STATE_1 | CAM_STATE_2);
            camera->stateFlags &= ~CAM_STATE_3;
            if (camera->prevBgCamIndex < 0) {
                Camera_ChangeSettingFlags(camera, camera->prevSetting, 2);
            } else {
                Camera_ChangeBgCamIndex(camera, camera->prevBgCamIndex);
                camera->prevBgCamIndex = -1;
            }
        }
    }
    spB8.yaw = Camera_LERPCeilS(rwData->unk_0C, spA8.yaw, roData->unk_14, 4);
    spB8.pitch = Camera_LERPCeilS(rwData->unk_0E, spA8.pitch, roData->unk_14, 4);
    Camera_AddVecGeoToVec3f(eyeNext, at, &spB8);
    *eye = *eyeNext;
    Camera_BGCheck(camera, at, eye);
    camera->fov = Camera_LERPCeilF(roData->unk_18, camera->fov, camera->fovUpdateRate, 1.0f);
    camera->roll = Camera_LERPCeilS(0, camera->roll, 0.5f, 0xA);
}

/**
 * Talking in a pre-rendered room
 */
s32 Camera_KeepOn0(Camera* camera) {
    Vec3f* eye = &camera->eye;
    Vec3f* eyeNext = &camera->eyeNext;
    Vec3f* at = &camera->at;
    VecGeo eyeTargetPosOffset;
    VecGeo eyeAtOffset;
    KeepOn0ReadOnlyData* roData = &camera->paramData.keep0.roData;
    KeepOn0ReadWriteData* rwData = &camera->paramData.keep0.rwData;
    s32 pad;
    BgCamFuncData* bgCamFuncData;
    UNUSED Vec3s bgCamRot;
    s16 fov;

    camera->stateFlags &= ~CAM_STATE_4;

    if (RELOAD_PARAMS(camera) || R_RELOAD_CAM_PARAMS) {
        CameraModeValue* values = sCameraSettings[camera->setting].cameraModes[camera->mode].values;

        roData->fovScale = GET_NEXT_SCALED_RO_DATA(values);
        roData->yawScale = GET_NEXT_SCALED_RO_DATA(values);
        roData->timerInit = GET_NEXT_RO_DATA(values);
        roData->interfaceField = GET_NEXT_RO_DATA(values);
    }

    if (R_RELOAD_CAM_PARAMS) {
        Camera_CopyPREGToModeValues(camera);
    }

    bgCamFuncData = (BgCamFuncData*)Camera_GetBgCamFuncData(camera);
    Camera_Vec3sToVec3f(eyeNext, &bgCamFuncData->pos);
    *eye = *eyeNext;

    bgCamRot = bgCamFuncData->rot;

    fov = bgCamFuncData->fov;
    if (fov == -1) {
        fov = 6000;
    }

    if (camera->target == NULL || camera->target->update == NULL) {
        if (camera->target == NULL) {
            osSyncPrintf(
                VT_COL(YELLOW, BLACK) "camera: warning: talk: target is not valid, change normal camera\n" VT_RST);
        }
        camera->target = NULL;
        Camera_ChangeMode(camera, CAM_MODE_NORMAL);
        return true;
    }

    Actor_GetFocus(&camera->targetPosRot, camera->target);

    OLib_Vec3fDiffToVecGeo(&eyeAtOffset, eye, at);
    OLib_Vec3fDiffToVecGeo(&eyeTargetPosOffset, eye, &camera->targetPosRot.pos);

    sCameraInterfaceField = roData->interfaceField;

    if (camera->animState == 0) {
        camera->animState++;
        camera->fov = CAM_DATA_SCALED(fov);
        camera->roll = 0;
        camera->atLERPStepScale = 0.0f;
        rwData->animTimer = roData->timerInit;
        rwData->fovTarget = camera->fov - (camera->fov * roData->fovScale);
    }

    if (rwData->animTimer != 0) {
        eyeAtOffset.yaw += ((s16)(eyeTargetPosOffset.yaw - eyeAtOffset.yaw) / rwData->animTimer) * roData->yawScale;
        Camera_AddVecGeoToVec3f(at, eye, &eyeAtOffset);
        rwData->animTimer--;
    } else {
        camera->stateFlags |= (CAM_STATE_4 | CAM_STATE_10);
    }
    camera->fov = Camera_LERPCeilF(rwData->fovTarget, camera->fov, 0.5f, 10.0f);
    return true;
}

s32 Camera_Fixed1(Camera* camera) {
    Fixed1ReadOnlyData* roData = &camera->paramData.fixd1.roData;
    Fixed1ReadWriteData* rwData = &camera->paramData.fixd1.rwData;
    s32 pad;
    VecGeo eyeOffset;
    VecGeo eyeAtOffset;
    s32 pad2;
    Vec3f adjustedPos;
    BgCamFuncData* bgCamFuncData;
    Vec3f* eye = &camera->eye;
    Vec3f* at = &camera->at;
    PosRot* playerPosRot = &camera->playerPosRot;
    f32 playerHeight;

    playerHeight = Player_GetHeight(camera->player);
    if (RELOAD_PARAMS(camera) || R_RELOAD_CAM_PARAMS) {
        CameraModeValue* values = sCameraSettings[camera->setting].cameraModes[camera->mode].values;

        bgCamFuncData = (BgCamFuncData*)Camera_GetBgCamFuncData(camera);
        Camera_Vec3sToVec3f(&rwData->eyePosRotTarget.pos, &bgCamFuncData->pos);
        rwData->eyePosRotTarget.rot = bgCamFuncData->rot;
        rwData->fov = bgCamFuncData->fov;

        roData->unk_00 = GET_NEXT_SCALED_RO_DATA(values) * playerHeight;
        roData->lerpStep = GET_NEXT_SCALED_RO_DATA(values);
        roData->fov = GET_NEXT_RO_DATA(values);
        roData->interfaceField = GET_NEXT_RO_DATA(values);
    }

    if (R_RELOAD_CAM_PARAMS) {
        Camera_CopyPREGToModeValues(camera);
    }
    if (rwData->fov == -1) {
        rwData->fov = roData->fov * 100.0f;
    } else if (rwData->fov <= 360) {
        rwData->fov *= 100;
    }

    sCameraInterfaceField = roData->interfaceField;

    if (camera->animState == 0) {
        camera->animState++;
        func_80043B60(camera);
        if (rwData->fov != -1) {
            roData->fov = CAM_DATA_SCALED(rwData->fov);
        }
    }

    OLib_Vec3fDiffToVecGeo(&eyeAtOffset, eye, at);

    Camera_LERPCeilVec3f(&rwData->eyePosRotTarget.pos, eye, 0.1f, 0.1f, 0.2f);
    adjustedPos = playerPosRot->pos;
    adjustedPos.y += playerHeight;
    camera->dist = OLib_Vec3fDist(&adjustedPos, eye);

    eyeOffset.r = camera->dist;
    eyeOffset.pitch = Camera_LERPCeilS(-rwData->eyePosRotTarget.rot.x, eyeAtOffset.pitch, roData->lerpStep, 5);
    eyeOffset.yaw = Camera_LERPCeilS(rwData->eyePosRotTarget.rot.y, eyeAtOffset.yaw, roData->lerpStep, 5);

    Camera_AddVecGeoToVec3f(at, eye, &eyeOffset);

    camera->eyeNext = *eye;

    camera->fov = Camera_LERPCeilF(roData->fov, camera->fov, roData->lerpStep, 0.01f);
    camera->roll = 0;
    camera->atLERPStepScale = 0.0f;

    camera->posOffset.x = camera->at.x - playerPosRot->pos.x;
    camera->posOffset.y = camera->at.y - playerPosRot->pos.y;
    camera->posOffset.z = camera->at.z - playerPosRot->pos.z;

    return true;
}

s32 Camera_Fixed2(Camera* camera) {
    Vec3f* eye = &camera->eye;
    Vec3f* at = &camera->at;
    Vec3f* eyeNext = &camera->eyeNext;
    Vec3f atTarget;
    Vec3f posOffsetTarget;
    PosRot* playerPosRot = &camera->playerPosRot;
    BgCamFuncData* bgCamFuncData;
    Fixed2ReadOnlyData* roData = &camera->paramData.fixd2.roData;
    Fixed2ReadWriteData* rwData = &camera->paramData.fixd2.rwData;
    s32 pad;
    f32 playerHeight;

    playerHeight = Player_GetHeight(camera->player);

    if (RELOAD_PARAMS(camera) || R_RELOAD_CAM_PARAMS) {
        CameraModeValue* values = sCameraSettings[camera->setting].cameraModes[camera->mode].values;
        f32 yNormal =
            1.0f + CAM_DATA_SCALED(R_CAM_YOFFSET_NORM) - (CAM_DATA_SCALED(R_CAM_YOFFSET_NORM) * (68.0f / playerHeight));

        roData->yOffset = (GET_NEXT_SCALED_RO_DATA(values) * playerHeight) * yNormal;
        roData->eyeStepScale = GET_NEXT_SCALED_RO_DATA(values);
        roData->posStepScale = GET_NEXT_SCALED_RO_DATA(values);
        roData->fov = GET_NEXT_RO_DATA(values);
        roData->interfaceField = GET_NEXT_RO_DATA(values);
        rwData->fov = roData->fov * 100.0f;

        bgCamFuncData = (BgCamFuncData*)Camera_GetBgCamFuncData(camera);
        if (bgCamFuncData != NULL) {
            Camera_Vec3sToVec3f(&rwData->eye, &bgCamFuncData->pos);
            if (bgCamFuncData->fov != -1) {
                rwData->fov = bgCamFuncData->fov;
            }
        } else {
            rwData->eye = *eye;
        }
        if (rwData->fov <= 360) {
            rwData->fov *= 100;
        }
    }

    if (R_RELOAD_CAM_PARAMS) {
        Camera_CopyPREGToModeValues(camera);
    }

    sCameraInterfaceField = roData->interfaceField;

    posOffsetTarget.x = 0.0f;
    posOffsetTarget.y = roData->yOffset + playerHeight;
    posOffsetTarget.z = 0.0f;

    Camera_LERPCeilVec3f(&posOffsetTarget, &camera->posOffset, roData->posStepScale, roData->posStepScale, 0.1f);
    atTarget.x = playerPosRot->pos.x + camera->posOffset.x;
    atTarget.y = playerPosRot->pos.y + camera->posOffset.y;
    atTarget.z = playerPosRot->pos.z + camera->posOffset.z;
    if (camera->animState == 0) {
        camera->animState++;
        func_80043B60(camera);
        if (!(roData->interfaceField & FIXED2_FLAG_0)) {
            *eye = *eyeNext = rwData->eye;
            camera->at = atTarget;
        }
    }

    Camera_LERPCeilVec3f(&atTarget, &camera->at, roData->posStepScale, roData->posStepScale, 10.0f);
    Camera_LERPCeilVec3f(&rwData->eye, eyeNext, roData->eyeStepScale, roData->eyeStepScale, 0.1f);

    *eye = *eyeNext;
    camera->dist = OLib_Vec3fDist(at, eye);
    camera->roll = 0;
    camera->xzSpeed = 0.0f;
    camera->fov = CAM_DATA_SCALED(rwData->fov);
    camera->atLERPStepScale = Camera_ClampLERPScale(camera, 1.0f);
    camera->posOffset.x = camera->at.x - playerPosRot->pos.x;
    camera->posOffset.y = camera->at.y - playerPosRot->pos.y;
    camera->posOffset.z = camera->at.z - playerPosRot->pos.z;
    return true;
}

/**
 * Camera's position is fixed, does not move, or rotate
 */
s32 Camera_Fixed3(Camera* camera) {
    Vec3f* eye = &camera->eye;
    Vec3f* at = &camera->at;
    Vec3f* eyeNext = &camera->eyeNext;
    VecGeo atGeo;
    BgCamFuncData* bgCamFuncData;
    VecGeo eyeAtOffset;
    Fixed3ReadOnlyData* roData = &camera->paramData.fixd3.roData;
    Fixed3ReadWriteData* rwData = &camera->paramData.fixd3.rwData;
    s32 pad;

    bgCamFuncData = (BgCamFuncData*)Camera_GetBgCamFuncData(camera);

    OLib_Vec3fDiffToVecGeo(&eyeAtOffset, eye, at);

    if (RELOAD_PARAMS(camera) || R_RELOAD_CAM_PARAMS) {
        CameraModeValue* values = sCameraSettings[camera->setting].cameraModes[camera->mode].values;

        roData->interfaceField = GET_NEXT_RO_DATA(values);
        Camera_Vec3sToVec3f(eyeNext, &bgCamFuncData->pos);
        *eye = *eyeNext;
        rwData->rot = bgCamFuncData->rot;
        rwData->fov = bgCamFuncData->fov;
        rwData->roomImageOverrideBgCamIndex = bgCamFuncData->roomImageOverrideBgCamIndex;
        if (rwData->fov == -1) {
            rwData->fov = 6000;
        }
        if (rwData->fov <= 360) {
            rwData->fov *= 100;
        }
    }

    if (R_RELOAD_CAM_PARAMS) {
        Camera_CopyPREGToModeValues(camera);
    }

    if (camera->animState == 0) {
        rwData->updDirTimer = 5;
        R_CAM_DATA(CAM_DATA_FOV) = rwData->fov;
        camera->animState++;
    }

    if (bgCamFuncData->roomImageOverrideBgCamIndex != rwData->roomImageOverrideBgCamIndex) {
        osSyncPrintf("camera: position change %d \n", rwData->roomImageOverrideBgCamIndex);
        rwData->roomImageOverrideBgCamIndex = bgCamFuncData->roomImageOverrideBgCamIndex;
        rwData->updDirTimer = 5;
    }

    if (rwData->updDirTimer > 0) {
        rwData->updDirTimer--;
        sUpdateCameraDirection = true;
    } else {
        sUpdateCameraDirection = false;
    }

    atGeo.r = 150.0f;
    atGeo.yaw = rwData->rot.y;
    atGeo.pitch = -rwData->rot.x;

    Camera_AddVecGeoToVec3f(at, eye, &atGeo);
    sCameraInterfaceField = roData->interfaceField;
    rwData->fov = R_CAM_DATA(CAM_DATA_FOV);
    camera->roll = 0;
    camera->fov = rwData->fov * 0.01f;
    camera->atLERPStepScale = 0.0f;
    return true;
}

/**
 * camera follow player, eye is in a fixed offset of the previous eye, and a value
 * specified in the scene.
 */
s32 Camera_Fixed4(Camera* camera) {
    Vec3f* eye = &camera->eye;
    Vec3f* at = &camera->at;
    Vec3f* eyeNext = &camera->eyeNext;
    Vec3f playerPosWithCamOffset;
    Vec3f atTarget;
    Vec3f posOffsetTarget;
    VecGeo atEyeNextOffset;
    VecGeo atTargetEyeNextOffset;
    PosRot* playerPosRot = &camera->playerPosRot;
    BgCamFuncData* bgCamFuncData;
    Vec3f* posOffset = &camera->posOffset;
    Fixed4ReadOnlyData* roData = &camera->paramData.fixd4.roData;
    Fixed4ReadWriteData* rwData = &camera->paramData.fixd4.rwData;
    f32 playerYOffset;

    playerYOffset = Player_GetHeight(camera->player);

    if (RELOAD_PARAMS(camera) || R_RELOAD_CAM_PARAMS) {
        CameraModeValue* values = sCameraSettings[camera->setting].cameraModes[camera->mode].values;
        f32 yNormal = 1.0f + CAM_DATA_SCALED(R_CAM_YOFFSET_NORM) -
                      (CAM_DATA_SCALED(R_CAM_YOFFSET_NORM) * (68.0f / playerYOffset));

        roData->yOffset = GET_NEXT_SCALED_RO_DATA(values) * playerYOffset * yNormal;
        roData->speedToEyePos = GET_NEXT_SCALED_RO_DATA(values);
        roData->followSpeed = GET_NEXT_SCALED_RO_DATA(values);
        roData->fov = GET_NEXT_RO_DATA(values);
        roData->interfaceField = GET_NEXT_RO_DATA(values);

        bgCamFuncData = (BgCamFuncData*)Camera_GetBgCamFuncData(camera);
        if (bgCamFuncData != NULL) {
            Camera_Vec3sToVec3f(&rwData->eyeTarget, &bgCamFuncData->pos);
        } else {
            rwData->eyeTarget = *eye;
        }
    }
    if (R_RELOAD_CAM_PARAMS) {
        Camera_CopyPREGToModeValues(camera);
    }
    sCameraInterfaceField = roData->interfaceField;
    if (camera->animState == 0) {
        camera->animState++;
        if (!(roData->interfaceField & FIXED4_FLAG_2)) {
            func_80043B60(camera);
        }
        rwData->followSpeed = roData->followSpeed;
    }

    VEC3F_LERPIMPDST(eyeNext, eyeNext, &rwData->eyeTarget, roData->speedToEyePos);
    *eye = *eyeNext;

    posOffsetTarget.x = 0.0f;
    posOffsetTarget.y = roData->yOffset + playerYOffset;
    posOffsetTarget.z = 0.0f;
    Camera_LERPCeilVec3f(&posOffsetTarget, &camera->posOffset, 0.1f, 0.1f, 0.1f);

    playerPosWithCamOffset.x = playerPosRot->pos.x + camera->posOffset.x;
    playerPosWithCamOffset.y = playerPosRot->pos.y + camera->posOffset.y;
    playerPosWithCamOffset.z = playerPosRot->pos.z + camera->posOffset.z;
    VEC3F_LERPIMPDST(&atTarget, at, &playerPosWithCamOffset, 0.5f);

    OLib_Vec3fDiffToVecGeo(&atEyeNextOffset, eyeNext, at);
    OLib_Vec3fDiffToVecGeo(&atTargetEyeNextOffset, eyeNext, &atTarget);

    atEyeNextOffset.r += (atTargetEyeNextOffset.r - atEyeNextOffset.r) * rwData->followSpeed;
    atEyeNextOffset.pitch = Camera_LERPCeilS(atTargetEyeNextOffset.pitch, atEyeNextOffset.pitch,
                                             rwData->followSpeed * camera->speedRatio, 0xA);
    atEyeNextOffset.yaw =
        Camera_LERPCeilS(atTargetEyeNextOffset.yaw, atEyeNextOffset.yaw, rwData->followSpeed * camera->speedRatio, 0xA);
    Camera_AddVecGeoToVec3f(at, eyeNext, &atEyeNextOffset);
    camera->dist = OLib_Vec3fDist(at, eye);
    camera->roll = 0;
    camera->fov = roData->fov;
    camera->atLERPStepScale = Camera_ClampLERPScale(camera, 1.0f);
    return true;
}

s32 Camera_Fixed0(Camera* camera) {
    return Camera_Noop(camera);
}

s32 Camera_Subj1(Camera* camera) {
    return Camera_Noop(camera);
}

s32 Camera_Subj2(Camera* camera) {
    return Camera_Noop(camera);
}

/**
 * First person view
 */
s32 Camera_Subj3(Camera* camera) {
    Vec3f* eye = &camera->eye;
    Vec3f* at = &camera->at;
    Vec3f* eyeNext = &camera->eyeNext;
    Vec3f sp98;
    Vec3f sp8C;
    VecGeo sp84;
    VecGeo sp7C;
    VecGeo tGeo;
    PosRot sp60;
    PosRot* playerPosRot = &camera->playerPosRot;
    f32 sp58;
    f32 temp_f0_3;
    s16 sp52;
    s16 sp50;
    Subj3ReadOnlyData* roData = &camera->paramData.subj3.roData;
    Subj3ReadWriteData* rwData = &camera->paramData.subj3.rwData;
    CameraModeValue* values;
    Vec3f* pad2;
    f32 playerHeight;

    Actor_GetFocus(&sp60, &camera->player->actor);
    playerHeight = Player_GetHeight(camera->player);

    if (camera->play->view.unk_124 == 0) {
        camera->play->view.unk_124 = camera->camId | 0x50;
        return true;
    }

    func_80043ABC(camera);
    Camera_CopyPREGToModeValues(camera);
    values = sCameraSettings[camera->setting].cameraModes[camera->mode].values;
    roData->eyeNextYOffset = GET_NEXT_SCALED_RO_DATA(values) * playerHeight;
    roData->eyeDist = GET_NEXT_RO_DATA(values);
    roData->eyeNextDist = GET_NEXT_RO_DATA(values);
    roData->unk_0C = GET_NEXT_RO_DATA(values);
    roData->atOffset.x = GET_NEXT_RO_DATA(values) * 0.1f;
    roData->atOffset.y = GET_NEXT_RO_DATA(values) * 0.1f;
    roData->atOffset.z = GET_NEXT_RO_DATA(values) * 0.1f;
    roData->fovTarget = GET_NEXT_RO_DATA(values);
    roData->interfaceField = GET_NEXT_RO_DATA(values);
    sp84.r = roData->eyeNextDist;
    sp84.yaw = sp60.rot.y - 0x7FFF;
    sp84.pitch = sp60.rot.x;
    sp98 = sp60.pos;
    sp98.y += roData->eyeNextYOffset;

    Camera_AddVecGeoToVec3f(&sp8C, &sp98, &sp84);
    OLib_Vec3fDiffToVecGeo(&sp7C, at, eye);

    sCameraInterfaceField = roData->interfaceField;
    if (RELOAD_PARAMS(camera)) {
        rwData->r = sp7C.r;
        rwData->yaw = sp7C.yaw;
        rwData->pitch = sp7C.pitch;
        rwData->animTimer = R_CAM_DEFAULT_ANIM_TIME;
        camera->dist = roData->eyeNextDist;
        camera->animState++;
        camera->rUpdateRateInv = 1.0f;
        camera->dist = roData->eyeNextDist;
    }

    tGeo.r = rwData->r;
    tGeo.yaw = rwData->yaw;
    tGeo.pitch = rwData->pitch;
    if (rwData->animTimer != 0) {
        temp_f0_3 = (1.0f / rwData->animTimer);
        pad2 = at;
        at->x = at->x + (sp98.x - pad2->x) * temp_f0_3;
        at->y = at->y + (sp98.y - pad2->y) * temp_f0_3;
        at->z = at->z + (sp98.z - pad2->z) * temp_f0_3;

        temp_f0_3 = (1.0f / R_CAM_DEFAULT_ANIM_TIME);
        sp58 = (tGeo.r - sp84.r) * temp_f0_3;
        sp52 = (s16)(tGeo.yaw - sp84.yaw) * temp_f0_3;
        sp50 = (s16)(tGeo.pitch - sp84.pitch) * temp_f0_3;

        sp7C.r = Camera_LERPCeilF(sp84.r + (sp58 * rwData->animTimer), sp7C.r, CAM_DATA_SCALED(OREG(28)), 1.0f);
        sp7C.yaw = Camera_LERPCeilS(sp84.yaw + (sp52 * rwData->animTimer), sp7C.yaw, CAM_DATA_SCALED(OREG(28)), 0xA);
        sp7C.pitch =
            Camera_LERPCeilS(sp84.pitch + (sp50 * rwData->animTimer), sp7C.pitch, CAM_DATA_SCALED(OREG(28)), 0xA);
        Camera_AddVecGeoToVec3f(eyeNext, at, &sp7C);

        *eye = *eyeNext;
        rwData->animTimer--;

        if (!camera->play->envCtx.skyboxDisabled) {
            Camera_BGCheck(camera, at, eye);
        } else {
            func_80044340(camera, at, eye);
        }
    } else {
        sp58 = Math_SinS(-sp60.rot.x);
        temp_f0_3 = Math_CosS(-sp60.rot.x);
        sp98.x = roData->atOffset.x;
        sp98.y = (roData->atOffset.y * temp_f0_3) - (roData->atOffset.z * sp58);
        sp98.z = (roData->atOffset.y * sp58) + (roData->atOffset.z * temp_f0_3);
        sp58 = Math_SinS(sp60.rot.y - 0x7FFF);
        temp_f0_3 = Math_CosS(sp60.rot.y - 0x7FFF);
        roData->atOffset.x = (sp98.z * sp58) + (sp98.x * temp_f0_3);
        roData->atOffset.y = sp98.y;
        roData->atOffset.z = (sp98.z * temp_f0_3) - (sp98.x * sp58);
        at->x = roData->atOffset.x + sp60.pos.x;
        at->y = roData->atOffset.y + sp60.pos.y;
        at->z = roData->atOffset.z + sp60.pos.z;
        sp7C.r = roData->eyeNextDist;
        sp7C.yaw = sp60.rot.y - 0x7FFF;
        sp7C.pitch = sp60.rot.x;
        Camera_AddVecGeoToVec3f(eyeNext, at, &sp7C);
        sp7C.r = roData->eyeDist;
        Camera_AddVecGeoToVec3f(eye, at, &sp7C);
    }

    camera->posOffset.x = camera->at.x - playerPosRot->pos.x;
    camera->posOffset.y = camera->at.y - playerPosRot->pos.y;
    camera->posOffset.z = camera->at.z - playerPosRot->pos.z;
    camera->fov = Camera_LERPCeilF(roData->fovTarget, camera->fov, 0.25f, 1.0f);
    camera->roll = 0;
    camera->atLERPStepScale = 0.0f;
    return 1;
}

/**
 * Crawlspaces
 * Moves the camera from third person to first person when entering a crawlspace
 * While in the crawlspace, link remains fixed in a single direction
 * The camera is what swings up and down while crawling forward or backwards
 *
 * Note:
 * Subject 4 uses bgCamFuncData.data differently than other functions:
 * All Vec3s data are points along the crawlspace
 * The second point represents the entrance, and the second to last point represents the exit
 * All other points are unused
 * All instances of crawlspaces have 6 points, except for the Testroom scene which has 9 points
 */
s32 Camera_Subj4(Camera* camera) {
    Vec3f* eye = &camera->eye;
    Vec3f* eyeNext = &camera->eyeNext;
    Vec3f* at = &camera->at;
    u16 crawlspaceNumPoints;
    Vec3s* crawlspacePoints;
    Vec3f temp1;
    Vec3f zoomAtTarget;
    f32 temp2;
    Player* player;
    f32 eyeLerp;
    PosRot playerPosRot;
    VecGeo targetOffset;
    VecGeo atEyeOffset;
    s16 eyeToAtYaw;
    s32 pad[2];
    f32 temp;
    Subj4ReadOnlyData* roData = &camera->paramData.subj4.roData;
    Subj4ReadWriteData* rwData = &camera->paramData.subj4.rwData;

#define vCrawlSpaceBackPos temp1
#define vEyeTarget temp1
#define vPlayerDistToFront temp2
#define vZoomTimer temp2

    if (RELOAD_PARAMS(camera) || R_RELOAD_CAM_PARAMS) {
        CameraModeValue* values = sCameraSettings[camera->setting].cameraModes[camera->mode].values;

        roData->interfaceField = GET_NEXT_RO_DATA(values);
    }

    if (R_RELOAD_CAM_PARAMS) {
        Camera_CopyPREGToModeValues(camera);
    }

    if (camera->play->view.unk_124 == 0) {
        camera->play->view.unk_124 = camera->camId | 0x50;
        rwData->xzSpeed = camera->xzSpeed;
        return true;
    }

    Actor_GetWorldPosShapeRot(&playerPosRot, &camera->player->actor);
    OLib_Vec3fDiffToVecGeo(&atEyeOffset, at, eye);

    sCameraInterfaceField = roData->interfaceField;

    // Crawlspace setup (runs for only 1 frame)
    if (camera->animState == 0) {
        crawlspacePoints = (Vec3s*)Camera_GetBgCamFuncDataUnderPlayer(camera, &crawlspaceNumPoints);
        // Second entry of crawlspacePoints contains the front position
        Camera_Vec3sToVec3f(&rwData->crawlspaceLine.point, &crawlspacePoints[1]);
        // Second last entry of crawlspacePoints contains the back position
        Camera_Vec3sToVec3f(&vCrawlSpaceBackPos, &crawlspacePoints[crawlspaceNumPoints - 2]);

        targetOffset.r = 10.0f;
        targetOffset.pitch = 0x238C; // ~50 degrees
        targetOffset.yaw = Camera_XZAngle(&vCrawlSpaceBackPos, &rwData->crawlspaceLine.point);

        vPlayerDistToFront = OLib_Vec3fDist(&camera->playerPosRot.pos, &rwData->crawlspaceLine.point);
        if (OLib_Vec3fDist(&camera->playerPosRot.pos, &vCrawlSpaceBackPos) < vPlayerDistToFront) {
            // Player is entering the crawlspace from the back
            rwData->crawlspaceLine.dir.x = rwData->crawlspaceLine.point.x - vCrawlSpaceBackPos.x;
            rwData->crawlspaceLine.dir.y = rwData->crawlspaceLine.point.y - vCrawlSpaceBackPos.y;
            rwData->crawlspaceLine.dir.z = rwData->crawlspaceLine.point.z - vCrawlSpaceBackPos.z;
            rwData->crawlspaceLine.point = vCrawlSpaceBackPos;
        } else {
            // Player is entering the crawlspace from the front
            rwData->crawlspaceLine.dir.x = vCrawlSpaceBackPos.x - rwData->crawlspaceLine.point.x;
            rwData->crawlspaceLine.dir.y = vCrawlSpaceBackPos.y - rwData->crawlspaceLine.point.y;
            rwData->crawlspaceLine.dir.z = vCrawlSpaceBackPos.z - rwData->crawlspaceLine.point.z;
            targetOffset.yaw -= 0x7FFF;
        }

        rwData->forwardYaw = targetOffset.yaw;
        rwData->zoomTimer = 10;
        rwData->eyeLerpPhase = 0;
        rwData->isSfxOff = false;
        rwData->eyeLerp = 0.0f;
        camera->animState++;
    }

    // Camera zooms in from third person to first person over 10 frames
    if (rwData->zoomTimer != 0) {
        targetOffset.r = 10.0f;
        targetOffset.pitch = 0x238C; // ~50 degrees
        targetOffset.yaw = rwData->forwardYaw;
        Camera_AddVecGeoToVec3f(&zoomAtTarget, &playerPosRot.pos, &targetOffset);

        vZoomTimer = rwData->zoomTimer + 1.0f;
        at->x = F32_LERPIMPINV(at->x, zoomAtTarget.x, vZoomTimer);
        at->y = F32_LERPIMPINV(at->y, zoomAtTarget.y, vZoomTimer);
        at->z = F32_LERPIMPINV(at->z, zoomAtTarget.z, vZoomTimer);

        atEyeOffset.r -= (atEyeOffset.r / vZoomTimer);
        atEyeOffset.yaw = BINANG_LERPIMPINV(atEyeOffset.yaw, (s16)(playerPosRot.rot.y - 0x7FFF), rwData->zoomTimer);
        atEyeOffset.pitch = BINANG_LERPIMPINV(atEyeOffset.pitch, playerPosRot.rot.x, rwData->zoomTimer);
        Camera_AddVecGeoToVec3f(eyeNext, at, &atEyeOffset);
        *eye = *eyeNext;
        rwData->zoomTimer--;
        return false;
    }

    if (rwData->xzSpeed < 0.5f) {
        return false;
    }

    Actor_GetWorldPosShapeRot(&playerPosRot, &camera->player->actor);
    Math3D_LineClosestToPoint(&rwData->crawlspaceLine, &playerPosRot.pos, eyeNext);

    // *at is unused before getting overwritten later this function
    at->x = eyeNext->x + rwData->crawlspaceLine.dir.x;
    at->y = eyeNext->y + rwData->crawlspaceLine.dir.y;
    at->z = eyeNext->z + rwData->crawlspaceLine.dir.z;

    *eye = *eyeNext;

    targetOffset.yaw = rwData->forwardYaw;
    targetOffset.r = 5.0f;
    targetOffset.pitch = 0x238C; // ~50 degrees

    Camera_AddVecGeoToVec3f(&vEyeTarget, eyeNext, &targetOffset);

    rwData->eyeLerpPhase += 0xBB8;
    eyeLerp = Math_CosS(rwData->eyeLerpPhase);

    // VEC3F_LERPIMPDST(eye, eye, &vEyeTarget, fabsf(eyeLerp))
    eye->x += (vEyeTarget.x - eye->x) * fabsf(eyeLerp);
    eye->y += (vEyeTarget.y - eye->y) * fabsf(eyeLerp);
    eye->z += (vEyeTarget.z - eye->z) * fabsf(eyeLerp);

    // When camera reaches the peak of offset and starts to move down
    // && alternating cycles (sfx plays only every 2nd cycle)
    if ((eyeLerp > rwData->eyeLerp) && !rwData->isSfxOff) {
        player = camera->player;
        rwData->isSfxOff = true;
        func_800F4010(&player->actor.projectedPos, NA_SE_PL_CRAWL + player->floorSfxOffset, 4.0f);
    } else if (eyeLerp < rwData->eyeLerp) {
        rwData->isSfxOff = false;
    }

    rwData->eyeLerp = eyeLerp;

    camera->player->actor.world.pos = *eyeNext;
    camera->player->actor.world.pos.y = camera->playerGroundY;
    camera->player->actor.shape.rot.y = targetOffset.yaw;

    eyeLerp = (240.0f * eyeLerp) * (rwData->xzSpeed * 0.416667f);
    eyeToAtYaw = rwData->forwardYaw + eyeLerp;

    at->x = eye->x + (Math_SinS(eyeToAtYaw) * 10.0f);
    at->y = eye->y;
    at->z = eye->z + (Math_CosS(eyeToAtYaw) * 10.0f);

    camera->roll = Camera_LERPCeilS(0, camera->roll, 0.5f, 0xA);

    return true;
}

s32 Camera_Subj0(Camera* camera) {
    return Camera_Noop(camera);
}

s32 Camera_Data0(Camera* camera) {
    return Camera_Noop(camera);
}

s32 Camera_Data1(Camera* camera) {
    osSyncPrintf("chau!chau!\n");
    return Camera_Normal1(camera);
}

s32 Camera_Data2(Camera* camera) {
    return Camera_Noop(camera);
}

s32 Camera_Data3(Camera* camera) {
    return Camera_Noop(camera);
}

s32 Camera_Data4(Camera* camera) {
    s32 pad2[2];
    Data4ReadOnlyData* roData = &camera->paramData.data4.roData;
    VecGeo eyeAtOffset;
    VecGeo atOffset;
    VecGeo eyeNextAtOffset;
    f32 yNormal;
    s16 fov;
    Vec3f* eyeNext = &camera->eyeNext;
    BgCamFuncData* bgCamFuncData;
    Vec3f lookAt;
    CameraModeValue* values;
    Data4ReadWriteData* rwData = &camera->paramData.data4.rwData;
    Vec3f* eye = &camera->eye;
    f32 playerHeight;
    Vec3f* at = &camera->at;
    s32 pad;

    playerHeight = Player_GetHeight(camera->player);

    if (RELOAD_PARAMS(camera) || R_RELOAD_CAM_PARAMS) {
        values = sCameraSettings[camera->setting].cameraModes[camera->mode].values;
        yNormal =
            1.0f + CAM_DATA_SCALED(R_CAM_YOFFSET_NORM) - (CAM_DATA_SCALED(R_CAM_YOFFSET_NORM) * (68.0f / playerHeight));
        roData->yOffset = GET_NEXT_SCALED_RO_DATA(values) * playerHeight * yNormal;
        roData->fov = GET_NEXT_RO_DATA(values);
        roData->interfaceField = GET_NEXT_RO_DATA(values);

        bgCamFuncData = (BgCamFuncData*)Camera_GetBgCamFuncData(camera);
        Camera_Vec3sToVec3f(&rwData->eyePosRot.pos, &bgCamFuncData->pos);
        rwData->eyePosRot.rot = bgCamFuncData->rot;
        fov = bgCamFuncData->fov;
        rwData->fov = fov;
        if (fov != -1) {
            roData->fov = rwData->fov <= 360 ? rwData->fov : CAM_DATA_SCALED(rwData->fov);
        }

        rwData->flags = bgCamFuncData->flags;
        *eye = rwData->eyePosRot.pos;
    }

    if (R_RELOAD_CAM_PARAMS) {
        Camera_CopyPREGToModeValues(camera);
    }

    sCameraInterfaceField = roData->interfaceField;

    if (camera->animState == 0) {
        camera->animState++;
        func_80043B60(camera);
    }

    OLib_Vec3fDiffToVecGeo(&eyeNextAtOffset, at, eyeNext);
    Camera_CalcAtDefault(camera, &eyeNextAtOffset, roData->yOffset, false);
    OLib_Vec3fDiffToVecGeo(&eyeAtOffset, eye, at);

    atOffset.r = eyeAtOffset.r;
    atOffset.yaw = (rwData->flags & 1) ? (CAM_DEG_TO_BINANG(camera->data2) + rwData->eyePosRot.rot.y) : eyeAtOffset.yaw;
    atOffset.pitch =
        (rwData->flags & 2) ? (CAM_DEG_TO_BINANG(camera->data3) + rwData->eyePosRot.rot.x) : eyeAtOffset.pitch;

    Camera_AddVecGeoToVec3f(at, eye, &atOffset);

    lookAt = camera->playerPosRot.pos;
    lookAt.y += playerHeight;

    camera->dist = OLib_Vec3fDist(&lookAt, eye);
    camera->roll = 0;
    camera->xzSpeed = 0.0f;
    camera->fov = roData->fov;
    camera->atLERPStepScale = 0;
    return true;
}

/**
 * Hanging off of a ledge
 */
s32 Camera_Unique1(Camera* camera) {
    Vec3f* eye = &camera->eye;
    Vec3f* at = &camera->at;
    Vec3f* eyeNext = &camera->eyeNext;
    Vec3f playerWaistPos;
    s16 phiTarget;
    VecGeo sp8C;
    VecGeo unk908PlayerPosOffset;
    VecGeo eyeAtOffset;
    VecGeo eyeNextAtOffset;
    PosRot* playerPosRot = &camera->playerPosRot;
    PosRot playerhead;
    Unique1ReadOnlyData* roData = &camera->paramData.uniq1.roData;
    Unique1ReadWriteData* rwData = &camera->paramData.uniq1.rwData;
    s32 pad;
    f32 playerHeight;
    s32 pad2;

    playerHeight = Player_GetHeight(camera->player);
    if (RELOAD_PARAMS(camera) || R_RELOAD_CAM_PARAMS) {
        CameraModeValue* values = sCameraSettings[camera->setting].cameraModes[camera->mode].values;
        f32 yNormal =
            1.0f + CAM_DATA_SCALED(R_CAM_YOFFSET_NORM) - (CAM_DATA_SCALED(R_CAM_YOFFSET_NORM) * (68.0f / playerHeight));

        roData->yOffset = GET_NEXT_SCALED_RO_DATA(values) * playerHeight * yNormal;
        roData->distMin = GET_NEXT_SCALED_RO_DATA(values) * playerHeight * yNormal;
        roData->distMax = GET_NEXT_SCALED_RO_DATA(values) * playerHeight * yNormal;
        roData->pitchTarget = CAM_DEG_TO_BINANG(GET_NEXT_RO_DATA(values));
        roData->fovTarget = GET_NEXT_RO_DATA(values);
        roData->atLERPScaleMax = GET_NEXT_SCALED_RO_DATA(values);
        roData->interfaceField = GET_NEXT_RO_DATA(values);
    }

    if (R_RELOAD_CAM_PARAMS != 0) {
        Camera_CopyPREGToModeValues(camera);
    }

    sUpdateCameraDirection = 1;

    OLib_Vec3fDiffToVecGeo(&eyeAtOffset, at, eye);
    OLib_Vec3fDiffToVecGeo(&eyeNextAtOffset, at, eyeNext);

    sCameraInterfaceField = roData->interfaceField;

    if (camera->animState == 0) {
        camera->posOffset.y = camera->posOffset.y - camera->playerPosDelta.y;
        rwData->yawTarget = eyeNextAtOffset.yaw;
        rwData->unk_00 = 0.0f;
        playerWaistPos = camera->player->bodyPartsPos[PLAYER_BODYPART_WAIST];
        OLib_Vec3fDiffToVecGeo(&unk908PlayerPosOffset, &playerPosRot->pos, &playerWaistPos);
        rwData->timer = R_CAM_DEFAULT_ANIM_TIME;
        rwData->yawTargetAdj = ABS((s16)(unk908PlayerPosOffset.yaw - eyeAtOffset.yaw)) < 0x3A98
                                   ? 0
                                   : (((s16)(unk908PlayerPosOffset.yaw - eyeAtOffset.yaw) / rwData->timer) / 4) * 3;
        camera->animState++;
    }

    Actor_GetFocus(&playerhead, &camera->player->actor); // unused

    camera->yawUpdateRateInv =
        Camera_LERPCeilF(100.0f, camera->yawUpdateRateInv, R_CAM_UPDATE_RATE_STEP_SCALE_XZ * 0.01f, 0.1f);
    camera->pitchUpdateRateInv =
        Camera_LERPCeilF(100.0f, camera->pitchUpdateRateInv, R_CAM_UPDATE_RATE_STEP_SCALE_XZ * 0.01f, 0.1f);
    camera->xzOffsetUpdateRate =
        Camera_LERPCeilF(0.005f, camera->xzOffsetUpdateRate, R_CAM_UPDATE_RATE_STEP_SCALE_XZ * 0.01f, 0.01f);
    camera->yOffsetUpdateRate =
        Camera_LERPCeilF(0.01f, camera->yOffsetUpdateRate, R_CAM_UPDATE_RATE_STEP_SCALE_Y * 0.01f, 0.01f);
    camera->fovUpdateRate = Camera_LERPCeilF(R_CAM_FOV_UPDATE_RATE * 0.01f, camera->fovUpdateRate, 0.05f, 0.1f);

    Camera_CalcAtDefault(camera, &eyeNextAtOffset, roData->yOffset, 1);
    OLib_Vec3fDiffToVecGeo(&sp8C, at, eyeNext);

    camera->dist = Camera_LERPClampDist(camera, sp8C.r, roData->distMin, roData->distMax);

    phiTarget = roData->pitchTarget;
    sp8C.pitch = Camera_LERPCeilS(phiTarget, eyeNextAtOffset.pitch, 1.0f / camera->pitchUpdateRateInv, 0xA);

    if (sp8C.pitch > R_CAM_MAX_PITCH) {
        sp8C.pitch = R_CAM_MAX_PITCH;
    }
    if (sp8C.pitch < -R_CAM_MAX_PITCH) {
        sp8C.pitch = -R_CAM_MAX_PITCH;
    }

    if (rwData->timer != 0) {
        rwData->yawTarget += rwData->yawTargetAdj;
        rwData->timer--;
    }

    sp8C.yaw = Camera_LERPFloorS(rwData->yawTarget, eyeNextAtOffset.yaw, 0.5f, 0x2710);
    Camera_AddVecGeoToVec3f(eyeNext, at, &sp8C);
    *eye = *eyeNext;
    Camera_BGCheck(camera, at, eye);
    camera->fov = Camera_LERPCeilF(roData->fovTarget, camera->fov, camera->fovUpdateRate, 1.0f);
    camera->roll = 0;
    camera->atLERPStepScale = Camera_ClampLERPScale(camera, roData->atLERPScaleMax);
    return true;
}

s32 Camera_Unique2(Camera* camera) {
    Vec3f* eye = &camera->eye;
    Vec3f* at = &camera->at;
    Vec3f* eyeNext = &camera->eyeNext;
    Vec3f playerPos;
    VecGeo eyeOffset;
    VecGeo eyeAtOffset;
    s32 pad;
    f32 lerpRateFactor;
    Unique2ReadOnlyData* roData = &camera->paramData.uniq2.roData;
    Unique2ReadWriteData* rwData = &camera->paramData.uniq2.rwData;
    s32 pad2;
    f32 playerHeight;

    playerHeight = Player_GetHeight(camera->player);

    OLib_Vec3fDiffToVecGeo(&eyeAtOffset, at, eye);

    if (RELOAD_PARAMS(camera) || R_RELOAD_CAM_PARAMS) {
        CameraModeValue* values = sCameraSettings[camera->setting].cameraModes[camera->mode].values;
        f32 yNormal =
            1.0f + CAM_DATA_SCALED(R_CAM_YOFFSET_NORM) - (CAM_DATA_SCALED(R_CAM_YOFFSET_NORM) * (68.0f / playerHeight));

        roData->yOffset = GET_NEXT_SCALED_RO_DATA(values) * playerHeight * yNormal;
        roData->distTarget = GET_NEXT_RO_DATA(values);
        roData->fovTarget = GET_NEXT_RO_DATA(values);
        roData->interfaceField = GET_NEXT_RO_DATA(values);
    }

    if (R_RELOAD_CAM_PARAMS) {
        Camera_CopyPREGToModeValues(camera);
    }

    sCameraInterfaceField = roData->interfaceField;

    if ((camera->animState == 0) || (rwData->unk_04 != roData->interfaceField)) {
        rwData->unk_04 = roData->interfaceField;
    }

    if (camera->animState == 0) {
        camera->animState = 1;
        func_80043B60(camera);
        rwData->unk_00 = 200.0f;
        if (roData->interfaceField & UNIQUE2_FLAG_4) {
            camera->stateFlags &= ~CAM_STATE_2;
        }
    }

    playerPos = camera->playerPosRot.pos;
    lerpRateFactor = (roData->interfaceField & UNIQUE2_FLAG_0 ? 1.0f : camera->speedRatio);
    at->x = F32_LERPIMP(at->x, playerPos.x, lerpRateFactor * 0.6f);
    at->y = F32_LERPIMP(at->y, playerPos.y + playerHeight + roData->yOffset, 0.4f);
    at->z = F32_LERPIMP(at->z, playerPos.z, lerpRateFactor * 0.6f);
    rwData->unk_00 = F32_LERPIMP(rwData->unk_00, 2.0f, 0.05f); // unused.

    if (roData->interfaceField & UNIQUE2_FLAG_0) {
        OLib_Vec3fDiffToVecGeo(&eyeOffset, at, eyeNext);
        eyeOffset.r = roData->distTarget;
        Camera_AddVecGeoToVec3f(&playerPos, at, &eyeOffset);
        Camera_LERPCeilVec3f(&playerPos, eye, 0.25f, 0.25f, 0.2f);
    } else if (roData->interfaceField & UNIQUE2_FLAG_1) {
        if (OLib_Vec3fDistXZ(at, eyeNext) < roData->distTarget) {
            OLib_Vec3fDiffToVecGeo(&eyeOffset, at, eyeNext);
            eyeOffset.yaw = Camera_LERPCeilS(eyeOffset.yaw, eyeAtOffset.yaw, 0.1f, 0xA);
            eyeOffset.r = roData->distTarget;
            eyeOffset.pitch = 0;
            Camera_AddVecGeoToVec3f(eye, at, &eyeOffset);
            eye->y = eyeNext->y;
        } else {
            Camera_LERPCeilVec3f(eyeNext, eye, 0.25f, 0.25f, 0.2f);
        }
    }

    Camera_BGCheck(camera, at, eye);
    camera->dist = OLib_Vec3fDist(at, eye);
    camera->roll = 0;
    camera->fov = Camera_LERPCeilF(roData->fovTarget, camera->fov, 0.2f, 0.1f);
    camera->atLERPStepScale = Camera_ClampLERPScale(camera, 1.0f);
    return true;
}

s32 Camera_Unique3(Camera* camera) {
    VecGeo sp60;
    f32 playerHeight;
    DoorParams* doorParams = &camera->paramData.doorParams;
    BgCamFuncData* bgCamFuncData;
    Vec3s bgCamRot;
    Unique3ReadWriteData* rwData = &camera->paramData.uniq3.rwData;
    Unique3ReadOnlyData* roData = &camera->paramData.uniq3.roData;
    Vec3f* at = &camera->at;
    PosRot* cameraPlayerPosRot = &camera->playerPosRot;

    playerHeight = Player_GetHeight(camera->player);
    camera->stateFlags &= ~CAM_STATE_4;

    if (RELOAD_PARAMS(camera) || R_RELOAD_CAM_PARAMS) {
        CameraModeValue* values = sCameraSettings[camera->setting].cameraModes[camera->mode].values;
        f32 yNormal =
            1.0f + CAM_DATA_SCALED(R_CAM_YOFFSET_NORM) - (CAM_DATA_SCALED(R_CAM_YOFFSET_NORM) * (68.0f / playerHeight));

        roData->yOffset = GET_NEXT_SCALED_RO_DATA(values) * playerHeight * yNormal;
        roData->fov = GET_NEXT_RO_DATA(values);
        roData->interfaceField = GET_NEXT_RO_DATA(values);
    }

    if (R_RELOAD_CAM_PARAMS != 0) {
        Camera_CopyPREGToModeValues(camera);
    }

    sCameraInterfaceField = roData->interfaceField;

    switch (camera->animState) {
        case 0:
            func_80043B60(camera);
            camera->stateFlags &= ~(CAM_STATE_2 | CAM_STATE_3);
            rwData->initialFov = camera->fov;
            rwData->initialDist = OLib_Vec3fDist(at, &camera->eye);
            camera->animState++;
            FALLTHROUGH;
        case 1:
            if (doorParams->timer1-- > 0) {
                break;
            }

            bgCamFuncData = (BgCamFuncData*)Camera_GetBgCamFuncData(camera);
            Camera_Vec3sToVec3f(&camera->eyeNext, &bgCamFuncData->pos);
            camera->eye = camera->eyeNext;
            bgCamRot = bgCamFuncData->rot;

            sp60.r = 100.0f;
            sp60.yaw = bgCamRot.y;
            sp60.pitch = -bgCamRot.x;

            Camera_AddVecGeoToVec3f(at, &camera->eye, &sp60);
            camera->animState++;
            FALLTHROUGH;
        case 2:
            if (roData->interfaceField & UNIQUE3_FLAG_2) {
                camera->at = cameraPlayerPosRot->pos;
                camera->at.y += playerHeight + roData->yOffset;
            }
            if (doorParams->timer2-- > 0) {
                break;
            }
            camera->animState++;
            FALLTHROUGH;
        case 3:
            camera->stateFlags |= (CAM_STATE_4 | CAM_STATE_10);
            if (camera->stateFlags & CAM_STATE_3) {
                camera->animState++;
            } else {
                break;
            }
            FALLTHROUGH;
        case 4:
            if (roData->interfaceField & UNIQUE3_FLAG_1) {
                camera->stateFlags |= CAM_STATE_2;
                camera->stateFlags &= ~CAM_STATE_3;
                Camera_ChangeSettingFlags(camera, CAM_SET_PIVOT_IN_FRONT, 2);
                break;
            }
            doorParams->timer3 = 5;
            if (camera->xzSpeed > 0.001f || CHECK_BTN_ALL(D_8015BD7C->state.input[0].press.button, BTN_A) ||
                CHECK_BTN_ALL(D_8015BD7C->state.input[0].press.button, BTN_B) ||
                CHECK_BTN_ALL(D_8015BD7C->state.input[0].press.button, BTN_CLEFT) ||
                CHECK_BTN_ALL(D_8015BD7C->state.input[0].press.button, BTN_CDOWN) ||
                CHECK_BTN_ALL(D_8015BD7C->state.input[0].press.button, BTN_CUP) ||
                CHECK_BTN_ALL(D_8015BD7C->state.input[0].press.button, BTN_CRIGHT) ||
                CHECK_BTN_ALL(D_8015BD7C->state.input[0].press.button, BTN_R) ||
                CHECK_BTN_ALL(D_8015BD7C->state.input[0].press.button, BTN_Z)) {
                camera->animState++;
            } else {
                break;
            }
            FALLTHROUGH;
        case 5:
            camera->fov = Camera_LERPCeilF(rwData->initialFov, camera->fov, 0.4f, 0.1f);
            OLib_Vec3fDiffToVecGeo(&sp60, at, &camera->eye);
            sp60.r = Camera_LERPCeilF(100.0f, sp60.r, 0.4f, 0.1f);
            Camera_AddVecGeoToVec3f(&camera->eyeNext, at, &sp60);
            camera->eye = camera->eyeNext;
            if (doorParams->timer3-- > 0) {
                break;
            }
            camera->animState++;
            FALLTHROUGH;
        default:
            camera->stateFlags |= CAM_STATE_2;
            camera->stateFlags &= ~CAM_STATE_3;
            camera->fov = roData->fov;
            Camera_ChangeSettingFlags(camera, camera->prevSetting, 2);
            camera->atLERPStepScale = 0.0f;
            camera->posOffset.x = camera->at.x - cameraPlayerPosRot->pos.x;
            camera->posOffset.y = camera->at.y - cameraPlayerPosRot->pos.y;
            camera->posOffset.z = camera->at.z - cameraPlayerPosRot->pos.z;
            break;
    }

    return true;
}

/**
 * Camera's eye is specified by scene camera data, at point is generated at the intersection
 * of the eye to the player
 */
s32 Camera_Unique0(Camera* camera) {
    f32 yOffset;
    CameraModeValue* values;
    Player* player;
    Vec3f playerPosWithOffset;
    VecGeo atPlayerOffset;
    BgCamFuncData* bgCamFuncData;
    Vec3s bgCamRot;
    PosRot* playerPosRot = &camera->playerPosRot;
    DoorParams* doorParams = &camera->paramData.doorParams;
    Unique0ReadOnlyData* roData = &camera->paramData.uniq0.roData;
    Unique0ReadWriteData* rwData = &camera->paramData.uniq0.rwData;
    Vec3f* eye = &camera->eye;
    s16 fov;

    yOffset = Player_GetHeight(camera->player);
    player = camera->player;

    if (RELOAD_PARAMS(camera) || R_RELOAD_CAM_PARAMS) {
        values = sCameraSettings[camera->setting].cameraModes[camera->mode].values;
        roData->interfaceField = GET_NEXT_RO_DATA(values);
    }

    if (R_RELOAD_CAM_PARAMS) {
        Camera_CopyPREGToModeValues(camera);
    }

    playerPosWithOffset = playerPosRot->pos;
    playerPosWithOffset.y += yOffset;

    sCameraInterfaceField = roData->interfaceField;

    if (camera->animState == 0) {
        func_80043B60(camera);
        camera->stateFlags &= ~CAM_STATE_2;

        bgCamFuncData = (BgCamFuncData*)Camera_GetBgCamFuncData(camera);
        Camera_Vec3sToVec3f(&rwData->eyeAndDirection.point, &bgCamFuncData->pos);

        *eye = camera->eyeNext = rwData->eyeAndDirection.point;
        bgCamRot = bgCamFuncData->rot;
        fov = bgCamFuncData->fov;
        if (fov != -1) {
            camera->fov = fov <= 360 ? fov : CAM_DATA_SCALED(fov);
        }
        rwData->animTimer = bgCamFuncData->timer;
        if (rwData->animTimer == -1) {
            rwData->animTimer = doorParams->timer1 + doorParams->timer2;
        }
        atPlayerOffset.r = OLib_Vec3fDist(&playerPosWithOffset, eye);
        atPlayerOffset.yaw = bgCamRot.y;
        atPlayerOffset.pitch = -bgCamRot.x;
        OLib_VecGeoToVec3f(&rwData->eyeAndDirection.dir, &atPlayerOffset);
        Math3D_LineClosestToPoint(&rwData->eyeAndDirection, &playerPosRot->pos, &camera->at);
        rwData->initalPos = playerPosRot->pos;
        camera->animState++;
    }

    if (player->stateFlags1 & PLAYER_STATE1_29) {
        rwData->initalPos = playerPosRot->pos;
    }

    if (roData->interfaceField & UNIQUE0_FLAG_0) {
        if (rwData->animTimer > 0) {
            rwData->animTimer--;
            rwData->initalPos = playerPosRot->pos;
        } else if (!(player->stateFlags1 & PLAYER_STATE1_29) &&
                   ((OLib_Vec3fDistXZ(&playerPosRot->pos, &rwData->initalPos) >= 10.0f) ||
                    CHECK_BTN_ALL(D_8015BD7C->state.input[0].press.button, BTN_A) ||
                    CHECK_BTN_ALL(D_8015BD7C->state.input[0].press.button, BTN_B) ||
                    CHECK_BTN_ALL(D_8015BD7C->state.input[0].press.button, BTN_CLEFT) ||
                    CHECK_BTN_ALL(D_8015BD7C->state.input[0].press.button, BTN_CDOWN) ||
                    CHECK_BTN_ALL(D_8015BD7C->state.input[0].press.button, BTN_CUP) ||
                    CHECK_BTN_ALL(D_8015BD7C->state.input[0].press.button, BTN_CRIGHT) ||
                    CHECK_BTN_ALL(D_8015BD7C->state.input[0].press.button, BTN_R) ||
                    CHECK_BTN_ALL(D_8015BD7C->state.input[0].press.button, BTN_Z))) {
            camera->dist = OLib_Vec3fDist(&camera->at, eye);
            camera->posOffset.x = camera->at.x - playerPosRot->pos.x;
            camera->posOffset.y = camera->at.y - playerPosRot->pos.y;
            camera->posOffset.z = camera->at.z - playerPosRot->pos.z;
            camera->atLERPStepScale = 0.0f;
            camera->stateFlags |= CAM_STATE_2;
            Camera_ChangeSettingFlags(camera, camera->prevSetting, 2);
        }
    } else {
        if (rwData->animTimer > 0) {
            rwData->animTimer--;
            if (rwData->animTimer == 0) {
                sCameraInterfaceField = CAM_INTERFACE_FIELD(CAM_LETTERBOX_NONE, CAM_HUD_VISIBILITY_ALL, 0);
            }
        } else {
            rwData->initalPos = playerPosRot->pos;
        }

        if (!(player->stateFlags1 & PLAYER_STATE1_29) &&
            ((0.001f < camera->xzSpeed) || CHECK_BTN_ALL(D_8015BD7C->state.input[0].press.button, BTN_A) ||
             CHECK_BTN_ALL(D_8015BD7C->state.input[0].press.button, BTN_B) ||
             CHECK_BTN_ALL(D_8015BD7C->state.input[0].press.button, BTN_CLEFT) ||
             CHECK_BTN_ALL(D_8015BD7C->state.input[0].press.button, BTN_CDOWN) ||
             CHECK_BTN_ALL(D_8015BD7C->state.input[0].press.button, BTN_CUP) ||
             CHECK_BTN_ALL(D_8015BD7C->state.input[0].press.button, BTN_CRIGHT) ||
             CHECK_BTN_ALL(D_8015BD7C->state.input[0].press.button, BTN_R) ||
             CHECK_BTN_ALL(D_8015BD7C->state.input[0].press.button, BTN_Z))) {
            camera->dist = OLib_Vec3fDist(&camera->at, &camera->eye);
            camera->posOffset.x = camera->at.x - playerPosRot->pos.x;
            camera->posOffset.y = camera->at.y - playerPosRot->pos.y;
            camera->posOffset.z = camera->at.z - playerPosRot->pos.z;
            camera->atLERPStepScale = 0.0f;
            Camera_ChangeSettingFlags(camera, camera->prevSetting, 2);
            camera->stateFlags |= CAM_STATE_2;
        }
    }
    return true;
}

s32 Camera_Unique4(Camera* camera) {
    return Camera_Noop(camera);
}

/**
 * Was setup to be used by the camera setting "FOREST_UNUSED"
 */
s32 Camera_Unique5(Camera* camera) {
    return Camera_Noop(camera);
}

/**
 * This function doesn't really update much.
 * Eye/at positions are updated via Camera_SetViewParam
 */
s32 Camera_Unique6(Camera* camera) {
    Unique6ReadOnlyData* roData = &camera->paramData.uniq6.roData;
    CameraModeValue* values;
    Vec3f sp2C;
    PosRot* playerPosRot = &camera->playerPosRot;
    f32 offset;

    if (RELOAD_PARAMS(camera) || R_RELOAD_CAM_PARAMS) {
        values = sCameraSettings[camera->setting].cameraModes[camera->mode].values;
        roData->interfaceField = GET_NEXT_RO_DATA(values);
    }

    if (R_RELOAD_CAM_PARAMS) {
        Camera_CopyPREGToModeValues(camera);
    }

    sCameraInterfaceField = roData->interfaceField;

    if (camera->animState == 0) {
        camera->animState++;
        func_80043ABC(camera);
    }

    if (camera->player != NULL) {
        offset = Player_GetHeight(camera->player);
        sp2C = playerPosRot->pos;
        sp2C.y += offset;
        camera->dist = OLib_Vec3fDist(&sp2C, &camera->eye);
        camera->posOffset.x = camera->at.x - playerPosRot->pos.x;
        camera->posOffset.y = camera->at.y - playerPosRot->pos.y;
        camera->posOffset.z = camera->at.z - playerPosRot->pos.z;
    } else {
        camera->dist = OLib_Vec3fDist(&camera->at, &camera->eye);
    }

    if ((roData->interfaceField & UNIQUE6_FLAG_0) && (camera->timer > 0)) {
        camera->timer--;
    }

    return true;
}

/**
 * Camera is at a fixed point specified by the scene's camera data,
 * camera rotates to follow player
 */
s32 Camera_Unique7(Camera* camera) {
    s32 pad;
    Unique7ReadOnlyData* roData = &camera->paramData.uniq7.roData;
    CameraModeValue* values;
    VecGeo playerPosEyeOffset;
    s16 fov;
    BgCamFuncData* bgCamFuncData;
    UNUSED Vec3s bgCamRot;
    Vec3f* at = &camera->at;
    PosRot* playerPosRot = &camera->playerPosRot;
    Vec3f* eye = &camera->eye;
    Vec3f* eyeNext = &camera->eyeNext;
    Unique7ReadWriteData* rwData = &camera->paramData.uniq7.rwData;

    if (RELOAD_PARAMS(camera) || R_RELOAD_CAM_PARAMS) {
        values = sCameraSettings[camera->setting].cameraModes[camera->mode].values;
        roData->fov = GET_NEXT_RO_DATA(values);
        roData->interfaceField = (s16)GET_NEXT_RO_DATA(values);
    }
    if (R_RELOAD_CAM_PARAMS) {
        Camera_CopyPREGToModeValues(camera);
    }

    bgCamFuncData = (BgCamFuncData*)Camera_GetBgCamFuncData(camera);

    Camera_Vec3sToVec3f(eyeNext, &bgCamFuncData->pos);
    *eye = *eyeNext;
    bgCamRot = bgCamFuncData->rot;

    OLib_Vec3fDiffToVecGeo(&playerPosEyeOffset, eye, &playerPosRot->pos);

    // fov actually goes unused since it's hard set later on.
    fov = bgCamFuncData->fov;
    if (fov == -1) {
        fov = roData->fov * 100.0f;
    }

    if (fov <= 360) {
        fov *= 100;
    }

    sCameraInterfaceField = roData->interfaceField;

    if (camera->animState == 0) {
        camera->animState++;
        camera->fov = CAM_DATA_SCALED(fov);
        camera->atLERPStepScale = 0.0f;
        camera->roll = 0;
        rwData->unk_00.x = playerPosEyeOffset.yaw;
    }

    camera->fov = 60.0f;

    // 0x7D0 ~ 10.98 degres.
    rwData->unk_00.x = Camera_LERPFloorS(playerPosEyeOffset.yaw, rwData->unk_00.x, 0.4f, 0x7D0);
    playerPosEyeOffset.pitch = -bgCamFuncData->rot.x * Math_CosS(playerPosEyeOffset.yaw - bgCamFuncData->rot.y);
    Camera_AddVecGeoToVec3f(at, eye, &playerPosEyeOffset);
    camera->stateFlags |= CAM_STATE_10;
    return true;
}

s32 Camera_Unique8(Camera* camera) {
    return Camera_Noop(camera);
}

s32 Camera_Unique9(Camera* camera) {
    Vec3f atTarget;
    Vec3f eyeTarget;
    Unique9ReadOnlyData* roData = &camera->paramData.uniq9.roData;
    Unique9ReadWriteData* rwData = &camera->paramData.uniq9.rwData;
    f32 invKeyFrameTimer;
    VecGeo eyeNextAtOffset;
    VecGeo scratchGeo;
    VecGeo playerTargetOffset;
    s16 action;
    s16 atInitFlags;
    s16 eyeInitFlags;
    s16 pad2;
    PosRot targethead;
    PosRot playerhead;
    PosRot playerPosRot;
    Vec3f* eyeNext = &camera->eyeNext;
    Vec3f* at = &camera->at;
    Vec3f* eye = &camera->eye;
    Player* player = camera->player;
    Actor* focusActor;
    f32 spB4;
    PosRot atFocusPosRot;
    Vec3f eyeLookAtPos;
    CameraModeValue* values;
    PosRot eyeFocusPosRot;

    if (RELOAD_PARAMS(camera) || R_RELOAD_CAM_PARAMS) {
        values = sCameraSettings[camera->setting].cameraModes[camera->mode].values;
        roData->interfaceField = GET_NEXT_RO_DATA(values);
    }

    if (R_RELOAD_CAM_PARAMS) {
        Camera_CopyPREGToModeValues(camera);
    }

    sCameraInterfaceField = roData->interfaceField;

    Actor_GetWorld(&playerPosRot, &camera->player->actor);

    if (camera->animState == 0) {
        camera->animState++;
        rwData->curKeyFrameIdx = -1;
        rwData->keyFrameTimer = 1;
        rwData->unk_38 = 0;
        rwData->playerPos.x = playerPosRot.pos.x;
        rwData->playerPos.y = playerPosRot.pos.y;
        rwData->playerPos.z = playerPosRot.pos.z;
        camera->atLERPStepScale = 0.0f;
        func_80043B60(camera);
    }

    if (rwData->unk_38 == 0 && rwData->keyFrameTimer > 0) {
        rwData->keyFrameTimer--;
    }

    if (rwData->keyFrameTimer == 0) {
        rwData->isNewKeyFrame = true;
        rwData->curKeyFrameIdx++;
        if (rwData->curKeyFrameIdx < ONEPOINT_CS_INFO(camera)->keyFrameCnt) {
            rwData->curKeyFrame = &ONEPOINT_CS_INFO(camera)->keyFrames[rwData->curKeyFrameIdx];
            rwData->keyFrameTimer = rwData->curKeyFrame->timerInit;

            if (rwData->curKeyFrame->unk_01 != 0xFF) {
                if ((rwData->curKeyFrame->unk_01 & 0xF0) == 0x80) {
                    D_8011D3AC = rwData->curKeyFrame->unk_01 & 0xF;
                } else if ((rwData->curKeyFrame->unk_01 & 0xF0) == 0xC0) {
                    Camera_UpdateInterface(
                        CAM_INTERFACE_FIELD(CAM_LETTERBOX_IGNORE, CAM_HUD_VISIBILITY(rwData->curKeyFrame->unk_01), 0));
                } else if (camera->player->stateFlags1 & PLAYER_STATE1_27 &&
                           player->currentBoots != PLAYER_BOOTS_IRON) {
                    func_8002DF38(camera->play, camera->target, 8);
                    osSyncPrintf("camera: demo: player demo set WAIT\n");
                } else {
                    osSyncPrintf("camera: demo: player demo set %d\n", rwData->curKeyFrame->unk_01);
                    func_8002DF38(camera->play, camera->target, rwData->curKeyFrame->unk_01);
                }
            }
        } else {
            // We've gone through all the keyframes.
            if (camera->camId != CAM_ID_MAIN) {
                camera->timer = 0;
            }
            return true;
        }
    } else {
        rwData->isNewKeyFrame = false;
    }

    atInitFlags = rwData->curKeyFrame->initFlags & 0xFF;
    if (atInitFlags == 1) {
        rwData->atTarget = rwData->curKeyFrame->atTargetInit;
    } else if (atInitFlags == 2) {
        if (rwData->isNewKeyFrame) {
            rwData->atTarget.x = camera->play->view.at.x + rwData->curKeyFrame->atTargetInit.x;
            rwData->atTarget.y = camera->play->view.at.y + rwData->curKeyFrame->atTargetInit.y;
            rwData->atTarget.z = camera->play->view.at.z + rwData->curKeyFrame->atTargetInit.z;
        }
    } else if (atInitFlags == 3) {
        if (rwData->isNewKeyFrame) {
            rwData->atTarget.x = camera->at.x + rwData->curKeyFrame->atTargetInit.x;
            rwData->atTarget.y = camera->at.y + rwData->curKeyFrame->atTargetInit.y;
            rwData->atTarget.z = camera->at.z + rwData->curKeyFrame->atTargetInit.z;
        }
    } else if (atInitFlags == 4 || atInitFlags == 0x84) {
        if (camera->target != NULL && camera->target->update != NULL) {
            Actor_GetFocus(&targethead, camera->target);
            Actor_GetFocus(&playerhead, &camera->player->actor);
            playerhead.pos.x = playerPosRot.pos.x;
            playerhead.pos.z = playerPosRot.pos.z;
            OLib_Vec3fDiffToVecGeo(&playerTargetOffset, &targethead.pos, &playerhead.pos);
            if (atInitFlags & (s16)0x8080) {
                scratchGeo.pitch = CAM_DEG_TO_BINANG(rwData->curKeyFrame->atTargetInit.x);
                scratchGeo.yaw = CAM_DEG_TO_BINANG(rwData->curKeyFrame->atTargetInit.y);
                scratchGeo.r = rwData->curKeyFrame->atTargetInit.z;
            } else {
                OLib_Vec3fToVecGeo(&scratchGeo, &rwData->curKeyFrame->atTargetInit);
            }
            scratchGeo.yaw += playerTargetOffset.yaw;
            scratchGeo.pitch += playerTargetOffset.pitch;
            Camera_AddVecGeoToVec3f(&rwData->atTarget, &targethead.pos, &scratchGeo);
        } else {
            if (camera->target == NULL) {
                osSyncPrintf(VT_COL(YELLOW, BLACK) "camera: warning: demo C: actor is not valid\n" VT_RST);
            }

            camera->target = NULL;
            rwData->atTarget = camera->at;
        }
    } else if (atInitFlags & 0x6060) {
        if (!(atInitFlags & 4) || rwData->isNewKeyFrame) {
            if (atInitFlags & 0x2020) {
                focusActor = &camera->player->actor;
            } else if (camera->target != NULL && camera->target->update != NULL) {
                focusActor = camera->target;
            } else {
                camera->target = NULL;
                focusActor = NULL;
            }

            if (focusActor != NULL) {
                if ((atInitFlags & 0xF) == 1) {
                    Actor_GetFocus(&atFocusPosRot, focusActor);
                } else if ((atInitFlags & 0xF) == 2) {
                    Actor_GetWorld(&atFocusPosRot, focusActor);
                } else {
                    Actor_GetWorldPosShapeRot(&atFocusPosRot, focusActor);
                }

                if (atInitFlags & (s16)0x8080) {
                    scratchGeo.pitch = CAM_DEG_TO_BINANG(rwData->curKeyFrame->atTargetInit.x);
                    scratchGeo.yaw = CAM_DEG_TO_BINANG(rwData->curKeyFrame->atTargetInit.y);
                    scratchGeo.r = rwData->curKeyFrame->atTargetInit.z;
                } else {
                    OLib_Vec3fToVecGeo(&scratchGeo, &rwData->curKeyFrame->atTargetInit);
                }

                scratchGeo.yaw += atFocusPosRot.rot.y;
                scratchGeo.pitch -= atFocusPosRot.rot.x;
                Camera_AddVecGeoToVec3f(&rwData->atTarget, &atFocusPosRot.pos, &scratchGeo);
            } else {
                if (camera->target == NULL) {
                    osSyncPrintf(VT_COL(YELLOW, BLACK) "camera: warning: demo C: actor is not valid\n" VT_RST);
                }
                rwData->atTarget = *at;
            }
        }
    } else {
        rwData->atTarget = *at;
    }

    eyeInitFlags = rwData->curKeyFrame->initFlags & 0xFF00;
    if (eyeInitFlags == 0x100) {
        rwData->eyeTarget = rwData->curKeyFrame->eyeTargetInit;
    } else if (eyeInitFlags == 0x200) {
        if (rwData->isNewKeyFrame) {
            rwData->eyeTarget.x = camera->play->view.eye.x + rwData->curKeyFrame->eyeTargetInit.x;
            rwData->eyeTarget.y = camera->play->view.eye.y + rwData->curKeyFrame->eyeTargetInit.y;
            rwData->eyeTarget.z = camera->play->view.eye.z + rwData->curKeyFrame->eyeTargetInit.z;
        }
    } else if (eyeInitFlags == 0x300) {
        if (rwData->isNewKeyFrame) {
            rwData->eyeTarget.x = camera->eyeNext.x + rwData->curKeyFrame->eyeTargetInit.x;
            rwData->eyeTarget.y = camera->eyeNext.y + rwData->curKeyFrame->eyeTargetInit.y;
            rwData->eyeTarget.z = camera->eyeNext.z + rwData->curKeyFrame->eyeTargetInit.z;
        }
    } else if (eyeInitFlags == 0x400 || eyeInitFlags == (s16)0x8400 || eyeInitFlags == 0x500 ||
               eyeInitFlags == (s16)0x8500) {
        if (camera->target != NULL && camera->target->update != NULL) {
            Actor_GetFocus(&targethead, camera->target);
            Actor_GetFocus(&playerhead, &camera->player->actor);
            playerhead.pos.x = playerPosRot.pos.x;
            playerhead.pos.z = playerPosRot.pos.z;
            OLib_Vec3fDiffToVecGeo(&playerTargetOffset, &targethead.pos, &playerhead.pos);
            if (eyeInitFlags == 0x400 || eyeInitFlags == (s16)0x8400) {
                eyeLookAtPos = targethead.pos;
            } else {
                eyeLookAtPos = rwData->atTarget;
            }

            if (eyeInitFlags & (s16)0x8080) {
                scratchGeo.pitch = CAM_DEG_TO_BINANG(rwData->curKeyFrame->eyeTargetInit.x);
                scratchGeo.yaw = CAM_DEG_TO_BINANG(rwData->curKeyFrame->eyeTargetInit.y);
                scratchGeo.r = rwData->curKeyFrame->eyeTargetInit.z;
            } else {
                OLib_Vec3fToVecGeo(&scratchGeo, &rwData->curKeyFrame->eyeTargetInit);
            }

            scratchGeo.yaw += playerTargetOffset.yaw;
            scratchGeo.pitch += playerTargetOffset.pitch;
            Camera_AddVecGeoToVec3f(&rwData->eyeTarget, &eyeLookAtPos, &scratchGeo);
        } else {
            if (camera->target == NULL) {
                osSyncPrintf(VT_COL(YELLOW, BLACK) "camera: warning: demo C: actor is not valid\n" VT_RST);
            }
            camera->target = NULL;
            rwData->eyeTarget = *eyeNext;
        }
    } else if (eyeInitFlags & 0x6060) {
        if (!(eyeInitFlags & 0x400) || rwData->isNewKeyFrame) {
            if (eyeInitFlags & 0x2020) {
                focusActor = &camera->player->actor;
            } else if (camera->target != NULL && camera->target->update != NULL) {
                focusActor = camera->target;
            } else {
                camera->target = NULL;
                focusActor = NULL;
            }

            if (focusActor != NULL) {
                if ((eyeInitFlags & 0xF00) == 0x100) {
                    // head
                    Actor_GetFocus(&eyeFocusPosRot, focusActor);
                } else if ((eyeInitFlags & 0xF00) == 0x200) {
                    // world
                    Actor_GetWorld(&eyeFocusPosRot, focusActor);
                } else {
                    // world, shapeRot
                    Actor_GetWorldPosShapeRot(&eyeFocusPosRot, focusActor);
                }

                if (eyeInitFlags & (s16)0x8080) {
                    scratchGeo.pitch = CAM_DEG_TO_BINANG(rwData->curKeyFrame->eyeTargetInit.x);
                    scratchGeo.yaw = CAM_DEG_TO_BINANG(rwData->curKeyFrame->eyeTargetInit.y);
                    scratchGeo.r = rwData->curKeyFrame->eyeTargetInit.z;
                } else {
                    OLib_Vec3fToVecGeo(&scratchGeo, &rwData->curKeyFrame->eyeTargetInit);
                }

                scratchGeo.yaw += eyeFocusPosRot.rot.y;
                scratchGeo.pitch -= eyeFocusPosRot.rot.x;
                Camera_AddVecGeoToVec3f(&rwData->eyeTarget, &eyeFocusPosRot.pos, &scratchGeo);
            } else {
                if (camera->target == NULL) {
                    osSyncPrintf(VT_COL(YELLOW, BLACK) "camera: warning: demo C: actor is not valid\n" VT_RST);
                }
                camera->target = NULL;
                rwData->eyeTarget = *eyeNext;
            }
        }
    } else {
        rwData->eyeTarget = *eyeNext;
    }

    if (rwData->curKeyFrame->initFlags == 2) {
        rwData->fovTarget = camera->play->view.fovy;
        rwData->rollTarget = 0;
    } else if (rwData->curKeyFrame->initFlags == 0) {
        rwData->fovTarget = camera->fov;
        rwData->rollTarget = camera->roll;
    } else {
        rwData->fovTarget = rwData->curKeyFrame->fovTargetInit;
        rwData->rollTarget = CAM_DEG_TO_BINANG(rwData->curKeyFrame->rollTargetInit);
    }

    action = ONEPOINT_CS_GET_ACTION(rwData->curKeyFrame);
    switch (action) {
        case ONEPOINT_CS_ACTION_ID_15:
            // static copy to at/eye/fov/roll
            *at = rwData->atTarget;
            *eyeNext = rwData->eyeTarget;
            camera->fov = rwData->fovTarget;
            camera->roll = rwData->rollTarget;
            camera->stateFlags |= CAM_STATE_10;
            break;

        case ONEPOINT_CS_ACTION_ID_21:
            // same as 15, but with unk_38 ?
            if (rwData->unk_38 == 0) {
                rwData->unk_38 = 1;
            } else if (camera->stateFlags & CAM_STATE_3) {
                rwData->unk_38 = 0;
                camera->stateFlags &= ~CAM_STATE_3;
            }
            *at = rwData->atTarget;
            *eyeNext = rwData->eyeTarget;
            camera->fov = rwData->fovTarget;
            camera->roll = rwData->rollTarget;
            break;

        case ONEPOINT_CS_ACTION_ID_16:
            // same as 21, but don't unset CAM_STATE_3 on stateFlags
            if (rwData->unk_38 == 0) {
                rwData->unk_38 = 1;
            } else if (camera->stateFlags & CAM_STATE_3) {
                rwData->unk_38 = 0;
            }

            *at = rwData->atTarget;
            *eyeNext = rwData->eyeTarget;
            camera->fov = rwData->fovTarget;
            camera->roll = rwData->rollTarget;
            break;

        case ONEPOINT_CS_ACTION_ID_1:
            // linear interpolation of eye/at using the geographic coordinates
            OLib_Vec3fDiffToVecGeo(&eyeNextAtOffset, at, eyeNext);
            OLib_Vec3fDiffToVecGeo(&rwData->atEyeOffsetTarget, &rwData->atTarget, &rwData->eyeTarget);
            invKeyFrameTimer = 1.0f / rwData->keyFrameTimer;
            scratchGeo.r = F32_LERPIMP(eyeNextAtOffset.r, rwData->atEyeOffsetTarget.r, invKeyFrameTimer);
            scratchGeo.pitch = eyeNextAtOffset.pitch +
                               ((s16)(rwData->atEyeOffsetTarget.pitch - eyeNextAtOffset.pitch) * invKeyFrameTimer);
            scratchGeo.yaw =
                eyeNextAtOffset.yaw + ((s16)(rwData->atEyeOffsetTarget.yaw - eyeNextAtOffset.yaw) * invKeyFrameTimer);
            Camera_AddVecGeoToVec3f(&eyeTarget, at, &scratchGeo);
            goto setEyeNext;

        case ONEPOINT_CS_ACTION_ID_2:
            // linear interpolation of eye/at using the eyeTarget
            invKeyFrameTimer = 1.0f / rwData->keyFrameTimer;
            eyeTarget.x = F32_LERPIMP(camera->eyeNext.x, rwData->eyeTarget.x, invKeyFrameTimer);
            eyeTarget.y = F32_LERPIMP(camera->eyeNext.y, rwData->eyeTarget.y, invKeyFrameTimer);
            eyeTarget.z = F32_LERPIMP(camera->eyeNext.z, rwData->eyeTarget.z, invKeyFrameTimer);

        setEyeNext:
            camera->eyeNext.x =
                Camera_LERPFloorF(eyeTarget.x, camera->eyeNext.x, rwData->curKeyFrame->lerpStepScale, 1.0f);
            camera->eyeNext.y =
                Camera_LERPFloorF(eyeTarget.y, camera->eyeNext.y, rwData->curKeyFrame->lerpStepScale, 1.0f);
            camera->eyeNext.z =
                Camera_LERPFloorF(eyeTarget.z, camera->eyeNext.z, rwData->curKeyFrame->lerpStepScale, 1.0f);
            FALLTHROUGH;
        case ONEPOINT_CS_ACTION_ID_9:
        case ONEPOINT_CS_ACTION_ID_10:
            // linear interpolation of at/fov/roll
            invKeyFrameTimer = 1.0f / rwData->keyFrameTimer;
            atTarget.x = F32_LERPIMP(camera->at.x, rwData->atTarget.x, invKeyFrameTimer);
            atTarget.y = F32_LERPIMP(camera->at.y, rwData->atTarget.y, invKeyFrameTimer);
            atTarget.z = F32_LERPIMP(camera->at.z, rwData->atTarget.z, invKeyFrameTimer);
            camera->at.x = Camera_LERPFloorF(atTarget.x, camera->at.x, rwData->curKeyFrame->lerpStepScale, 1.0f);
            camera->at.y = Camera_LERPFloorF(atTarget.y, camera->at.y, rwData->curKeyFrame->lerpStepScale, 1.0f);
            camera->at.z = Camera_LERPFloorF(atTarget.z, camera->at.z, rwData->curKeyFrame->lerpStepScale, 1.0f);
            camera->fov = Camera_LERPFloorF(F32_LERPIMP(camera->fov, rwData->fovTarget, invKeyFrameTimer), camera->fov,
                                            rwData->curKeyFrame->lerpStepScale, 0.01f);
            camera->roll = Camera_LERPFloorS(BINANG_LERPIMPINV(camera->roll, rwData->rollTarget, rwData->keyFrameTimer),
                                             camera->roll, rwData->curKeyFrame->lerpStepScale, 0xA);
            break;

        case ONEPOINT_CS_ACTION_ID_4:
            // linear interpolation of eye/at/fov/roll using the step scale, and spherical coordinates
            OLib_Vec3fDiffToVecGeo(&eyeNextAtOffset, at, eyeNext);
            OLib_Vec3fDiffToVecGeo(&rwData->atEyeOffsetTarget, &rwData->atTarget, &rwData->eyeTarget);
            scratchGeo.r = Camera_LERPCeilF(rwData->atEyeOffsetTarget.r, eyeNextAtOffset.r,
                                            rwData->curKeyFrame->lerpStepScale, 0.1f);
            scratchGeo.pitch = Camera_LERPCeilS(rwData->atEyeOffsetTarget.pitch, eyeNextAtOffset.pitch,
                                                rwData->curKeyFrame->lerpStepScale, 1);
            scratchGeo.yaw = Camera_LERPCeilS(rwData->atEyeOffsetTarget.yaw, eyeNextAtOffset.yaw,
                                              rwData->curKeyFrame->lerpStepScale, 1);
            Camera_AddVecGeoToVec3f(eyeNext, at, &scratchGeo);
            goto setAtFOVRoll;

        case ONEPOINT_CS_ACTION_ID_3:
            // linear interplation of eye/at/fov/roll using the step scale using eyeTarget
            camera->eyeNext.x =
                Camera_LERPCeilF(rwData->eyeTarget.x, camera->eyeNext.x, rwData->curKeyFrame->lerpStepScale, 1.0f);
            camera->eyeNext.y =
                Camera_LERPCeilF(rwData->eyeTarget.y, camera->eyeNext.y, rwData->curKeyFrame->lerpStepScale, 1.0f);
            camera->eyeNext.z =
                Camera_LERPCeilF(rwData->eyeTarget.z, camera->eyeNext.z, rwData->curKeyFrame->lerpStepScale, 1.0f);
            FALLTHROUGH;
        case ONEPOINT_CS_ACTION_ID_11:
        case ONEPOINT_CS_ACTION_ID_12:
        setAtFOVRoll:
            // linear interpolation of at/fov/roll using the step scale.
            camera->at.x = Camera_LERPCeilF(rwData->atTarget.x, camera->at.x, rwData->curKeyFrame->lerpStepScale, 1.0f);
            camera->at.y = Camera_LERPCeilF(rwData->atTarget.y, camera->at.y, rwData->curKeyFrame->lerpStepScale, 1.0f);
            camera->at.z = Camera_LERPCeilF(rwData->atTarget.z, camera->at.z, rwData->curKeyFrame->lerpStepScale, 1.0f);
            camera->fov = Camera_LERPCeilF(rwData->fovTarget, camera->fov, rwData->curKeyFrame->lerpStepScale, 1.0f);
            camera->roll = Camera_LERPCeilS(rwData->rollTarget, camera->roll, rwData->curKeyFrame->lerpStepScale, 1);
            break;

        case ONEPOINT_CS_ACTION_ID_13:
            // linear interpolation of at, with rotation around eyeTargetInit.y
            camera->at.x = Camera_LERPCeilF(rwData->atTarget.x, camera->at.x, rwData->curKeyFrame->lerpStepScale, 1.0f);
            camera->at.y += camera->playerPosDelta.y * rwData->curKeyFrame->lerpStepScale;
            camera->at.z = Camera_LERPCeilF(rwData->atTarget.z, camera->at.z, rwData->curKeyFrame->lerpStepScale, 1.0f);
            OLib_Vec3fDiffToVecGeo(&scratchGeo, at, eyeNext);
            scratchGeo.yaw += CAM_DEG_TO_BINANG(rwData->curKeyFrame->eyeTargetInit.y);

            // 3A98 ~ 82.40 degrees
            if (scratchGeo.pitch >= 0x3A99) {
                scratchGeo.pitch = 0x3A98;
            }

            if (scratchGeo.pitch < -0x3A98) {
                scratchGeo.pitch = -0x3A98;
            }

            spB4 = scratchGeo.r;
            if (1) {}
            scratchGeo.r = !(spB4 < rwData->curKeyFrame->eyeTargetInit.z)
                               ? Camera_LERPCeilF(rwData->curKeyFrame->eyeTargetInit.z, spB4,
                                                  rwData->curKeyFrame->lerpStepScale, 1.0f)
                               : scratchGeo.r;

            Camera_AddVecGeoToVec3f(eyeNext, at, &scratchGeo);
            camera->fov =
                Camera_LERPCeilF(F32_LERPIMPINV(camera->fov, rwData->curKeyFrame->fovTargetInit, rwData->keyFrameTimer),
                                 camera->fov, rwData->curKeyFrame->lerpStepScale, 1.0f);
            camera->roll = Camera_LERPCeilS(rwData->rollTarget, camera->roll, rwData->curKeyFrame->lerpStepScale, 1);
            break;

        case ONEPOINT_CS_ACTION_ID_24:
            // Set current keyframe to the roll target?
            rwData->curKeyFrameIdx = rwData->rollTarget;
            break;

        case ONEPOINT_CS_ACTION_ID_19: {
            // Change the parent camera (or default)'s mode to normal
            s32 camIdx = camera->parentCamId <= CAM_ID_NONE ? CAM_ID_MAIN : camera->parentCamId;

            Camera_ChangeModeFlags(camera->play->cameraPtrs[camIdx], CAM_MODE_NORMAL, 1);
        }
            FALLTHROUGH;
        case ONEPOINT_CS_ACTION_ID_18: {
            // copy the current camera to the parent (or default)'s camera.
            s32 camIdx = camera->parentCamId <= CAM_ID_NONE ? CAM_ID_MAIN : camera->parentCamId;
            Camera* cam = camera->play->cameraPtrs[camIdx];

            *eye = *eyeNext;
            Camera_Copy(cam, camera);
        }
            FALLTHROUGH;
        default:
            if (camera->camId != CAM_ID_MAIN) {
                camera->timer = 0;
            }
            break;
    }

    *eye = *eyeNext;

    if (rwData->curKeyFrame->actionFlags & ONEPOINT_CS_ACTION_FLAG_BGCHECK) {
        Camera_BGCheck(camera, at, eye);
    }

    if (rwData->curKeyFrame->actionFlags & ONEPOINT_CS_ACTION_FLAG_40) {
        // Set the player's position
        camera->player->actor.world.pos.x = rwData->playerPos.x;
        camera->player->actor.world.pos.z = rwData->playerPos.z;
        if (camera->player->stateFlags1 & PLAYER_STATE1_27 && player->currentBoots != PLAYER_BOOTS_IRON) {
            camera->player->actor.world.pos.y = rwData->playerPos.y;
        }
    } else {
        rwData->playerPos.x = playerPosRot.pos.x;
        rwData->playerPos.y = playerPosRot.pos.y;
        rwData->playerPos.z = playerPosRot.pos.z;
    }

    if (rwData->unk_38 == 0 && camera->timer > 0) {
        camera->timer--;
    }

    if (camera->player != NULL) {
        camera->posOffset.x = camera->at.x - camera->playerPosRot.pos.x;
        camera->posOffset.y = camera->at.y - camera->playerPosRot.pos.y;
        camera->posOffset.z = camera->at.z - camera->playerPosRot.pos.z;
    }

    camera->dist = OLib_Vec3fDist(at, eye);
    return true;
}

void Camera_DebugPrintSplineArray(char* name, s16 length, CutsceneCameraPoint cameraPoints[]) {
    s32 i;

    osSyncPrintf("static SplinedatZ  %s[] = {\n", name);
    for (i = 0; i < length; i++) {
        osSyncPrintf("    /* key frame %2d */ {\n", i);
        osSyncPrintf("    /*     code     */ %d,\n", cameraPoints[i].continueFlag);
        osSyncPrintf("    /*     z        */ %d,\n", cameraPoints[i].cameraRoll);
        osSyncPrintf("    /*     T        */ %d,\n", cameraPoints[i].nextPointFrame);
        osSyncPrintf("    /*     zoom     */ %f,\n", cameraPoints[i].viewAngle);
        osSyncPrintf("    /*     pos      */ { %d, %d, %d }\n", cameraPoints[i].pos.x, cameraPoints[i].pos.y,
                     cameraPoints[i].pos.z);
        osSyncPrintf("    },\n");
    }
    osSyncPrintf("};\n\n");
}

/**
 * Copies `src` to `dst`, used in Camera_Demo1
 * Name from AC map: Camera2_SetPos_Demo
 */
void Camera_Vec3fCopy(Vec3f* src, Vec3f* dst) {
    dst->x = src->x;
    dst->y = src->y;
    dst->z = src->z;
}

/**
 * Calculates new position from `at` to `pos`, outputs to `dst
 * Name from AC map: Camera2_CalcPos_Demo
 */
void Camera_RotateAroundPoint(PosRot* at, Vec3f* pos, Vec3f* dst) {
    VecGeo posGeo;
    Vec3f posCopy;

    Camera_Vec3fCopy(pos, &posCopy);
    OLib_Vec3fToVecGeo(&posGeo, &posCopy);
    posGeo.yaw += at->rot.y;
    Camera_AddVecGeoToVec3f(dst, &at->pos, &posGeo);
}

/**
 * Camera follows points specified at pointers to CutsceneCameraPoints,
 * camera->data0 for camera at positions, and camera->data1 for camera eye positions
 * until all keyFrames have been exhausted.
 */
s32 Camera_Demo1(Camera* camera) {
    s32 pad;
    Demo1ReadOnlyData* roData = &camera->paramData.demo1.roData;
    CameraModeValue* values;
    Vec3f* at = &camera->at;
    CutsceneCameraPoint* csAtPoints = (CutsceneCameraPoint*)camera->data0;
    CutsceneCameraPoint* csEyePoints = (CutsceneCameraPoint*)camera->data1;
    Vec3f* eye = &camera->eye;
    PosRot curPlayerPosRot;
    Vec3f csEyeUpdate;
    Vec3f csAtUpdate;
    f32 newRoll;
    Vec3f* eyeNext = &camera->eyeNext;
    f32* cameraFOV = &camera->fov;
    s16* relativeToPlayer = &camera->data2;
    Demo1ReadWriteData* rwData = &camera->paramData.demo1.rwData;

    if (RELOAD_PARAMS(camera) || R_RELOAD_CAM_PARAMS) {
        values = sCameraSettings[camera->setting].cameraModes[camera->mode].values;
        roData->interfaceField = GET_NEXT_RO_DATA(values);
    }

    sCameraInterfaceField = roData->interfaceField;

    switch (camera->animState) {
        case 0:
            // initialize camera state
            rwData->keyframe = 0;
            rwData->curFrame = 0.0f;
            camera->animState++;
            // "absolute" : "relative"
            osSyncPrintf(VT_SGR("1") "%06u:" VT_RST " camera: spline demo: start %s \n", camera->play->state.frames,
                         *relativeToPlayer == 0 ? "絶対" : "相対");

            if (PREG(93)) {
                Camera_DebugPrintSplineArray("CENTER", 5, csAtPoints);
                Camera_DebugPrintSplineArray("   EYE", 5, csEyePoints);
            }
            FALLTHROUGH;
        case 1:
            // follow CutsceneCameraPoints.  function returns 1 if at the end.
            if (func_800BB2B4(&csEyeUpdate, &newRoll, cameraFOV, csEyePoints, &rwData->keyframe, &rwData->curFrame) ||
                func_800BB2B4(&csAtUpdate, &newRoll, cameraFOV, csAtPoints, &rwData->keyframe, &rwData->curFrame)) {
                camera->animState++;
            }
            if (*relativeToPlayer) {
                // if the camera is set to be relative to the player, move the interpolated points
                // relative to the player's position
                if (camera->player != NULL && camera->player->actor.update != NULL) {
                    Actor_GetWorld(&curPlayerPosRot, &camera->player->actor);
                    Camera_RotateAroundPoint(&curPlayerPosRot, &csEyeUpdate, eyeNext);
                    Camera_RotateAroundPoint(&curPlayerPosRot, &csAtUpdate, at);
                } else {
                    osSyncPrintf(VT_COL(RED, WHITE) "camera: spline demo: owner dead\n" VT_RST);
                }
            } else {
                // simply copy the interpolated values to the eye and at
                Camera_Vec3fCopy(&csEyeUpdate, eyeNext);
                Camera_Vec3fCopy(&csAtUpdate, at);
            }
            *eye = *eyeNext;
            camera->roll = newRoll * 256.0f;
            camera->dist = OLib_Vec3fDist(at, eye);
            break;
    }
    return true;
}

s32 Camera_Demo2(Camera* camera) {
    return Camera_Noop(camera);
}

/**
 * Opening large chests.
 * The camera position will be at a fixed point, and rotate around at different intervals.
 * The direction, and initial position is dependent on when the camera was started.
 */
s32 Camera_Demo3(Camera* camera) {
    Vec3f* eye = &camera->eye;
    Vec3f* at = &camera->at;
    Vec3f* eyeNext = &camera->eyeNext;
    PosRot* camPlayerPosRot = &camera->playerPosRot;
    VecGeo eyeAtOffset;
    VecGeo eyeOffset;
    VecGeo atOffset;
    Vec3f sp74;
    Vec3f sp68;
    Vec3f sp5C;
    f32 temp_f0;
    s32 pad;
    u8 skipUpdateEye = false;
    f32 yOffset = Player_GetHeight(camera->player);
    s16 angle;
    Demo3ReadOnlyData* roData = &camera->paramData.demo3.roData;
    Demo3ReadWriteData* rwData = &camera->paramData.demo3.rwData;
    s32 pad2;

    camera->stateFlags &= ~CAM_STATE_4;

    if (RELOAD_PARAMS(camera) || R_RELOAD_CAM_PARAMS) {
        CameraModeValue* values = sCameraSettings[camera->setting].cameraModes[camera->mode].values;

        roData->fov = GET_NEXT_RO_DATA(values);
        roData->unk_04 = GET_NEXT_RO_DATA(values); // unused.
        roData->interfaceField = GET_NEXT_RO_DATA(values);
    }

    if (R_RELOAD_CAM_PARAMS) {
        Camera_CopyPREGToModeValues(camera);
    }

    OLib_Vec3fDiffToVecGeo(&eyeAtOffset, at, eye);

    sCameraInterfaceField = roData->interfaceField;

    switch (camera->animState) {
        case 0:
            camera->stateFlags &= ~(CAM_STATE_2 | CAM_STATE_3);
            func_80043B60(camera);
            camera->fov = roData->fov;
            camera->roll = rwData->animFrame = 0;
            rwData->initialAt = camPlayerPosRot->pos;
            if (camera->playerGroundY != BGCHECK_Y_MIN) {
                rwData->initialAt.y = camera->playerGroundY;
            }
            angle = camPlayerPosRot->rot.y;
            sp68.x = rwData->initialAt.x + (Math_SinS(angle) * 40.0f);
            sp68.y = rwData->initialAt.y + 40.0f;
            sp68.z = rwData->initialAt.z + (Math_CosS(angle) * 40.0f);
            if (camera->play->state.frames & 1) {
                angle -= 0x3FFF;
                rwData->yawDir = 1;
            } else {
                angle += 0x3FFF;
                rwData->yawDir = -1;
            }
            sp74.x = sp68.x + (D_8011D658[1].r * Math_SinS(angle));
            sp74.y = rwData->initialAt.y + 5.0f;
            sp74.z = sp68.z + (D_8011D658[1].r * Math_CosS(angle));
            if (Camera_BGCheck(camera, &sp68, &sp74)) {
                rwData->yawDir = -rwData->yawDir;
            }
            OLib_Vec3fToVecGeo(&atOffset, &D_8011D678[0]);
            atOffset.yaw += camPlayerPosRot->rot.y;
            Camera_AddVecGeoToVec3f(at, &rwData->initialAt, &atOffset);
            eyeOffset.r = D_8011D658[0].r;
            eyeOffset.pitch = D_8011D658[0].pitch;
            eyeOffset.yaw = (D_8011D658[0].yaw * rwData->yawDir) + camPlayerPosRot->rot.y;
            rwData->unk_0C = 1.0f;
            break;
        case 1:
            temp_f0 = (rwData->animFrame - 2) * (1.0f / 146.0f);

            sp5C.x = F32_LERPIMP(D_8011D678[0].x, D_8011D678[1].x, temp_f0);
            sp5C.y = F32_LERPIMP(D_8011D678[0].y, D_8011D678[1].y, temp_f0);
            sp5C.z = F32_LERPIMP(D_8011D678[0].z, D_8011D678[1].z, temp_f0);

            OLib_Vec3fToVecGeo(&atOffset, &sp5C);
            atOffset.yaw = (atOffset.yaw * rwData->yawDir) + camPlayerPosRot->rot.y;
            Camera_AddVecGeoToVec3f(at, &rwData->initialAt, &atOffset);

            atOffset.r = F32_LERPIMP(D_8011D658[0].r, D_8011D658[1].r, temp_f0);
            atOffset.pitch = BINANG_LERPIMP(D_8011D658[0].pitch, D_8011D658[1].pitch, temp_f0);
            atOffset.yaw = BINANG_LERPIMP(D_8011D658[0].yaw, D_8011D658[1].yaw, temp_f0);

            eyeOffset.r = atOffset.r;
            eyeOffset.pitch = atOffset.pitch;
            eyeOffset.yaw = (atOffset.yaw * rwData->yawDir) + camPlayerPosRot->rot.y;

            rwData->unk_0C -= (1.0f / 365.0f);
            break;
        case 2:
            temp_f0 = (rwData->animFrame - 0x94) * 0.1f;

            sp5C.x = F32_LERPIMP(D_8011D678[1].x, D_8011D678[2].x, temp_f0);
            sp5C.y = F32_LERPIMP((D_8011D678[1].y - yOffset), D_8011D678[2].y, temp_f0);
            sp5C.y += yOffset;
            sp5C.z = F32_LERPIMP(D_8011D678[1].z, D_8011D678[2].z, temp_f0);

            OLib_Vec3fToVecGeo(&atOffset, &sp5C);
            atOffset.yaw = (atOffset.yaw * rwData->yawDir) + camPlayerPosRot->rot.y;
            Camera_AddVecGeoToVec3f(at, &rwData->initialAt, &atOffset);

            atOffset.r = F32_LERPIMP(D_8011D658[1].r, D_8011D658[2].r, temp_f0);
            atOffset.pitch = BINANG_LERPIMP(D_8011D658[1].pitch, D_8011D658[2].pitch, temp_f0);
            atOffset.yaw = BINANG_LERPIMP(D_8011D658[1].yaw, D_8011D658[2].yaw, temp_f0);

            eyeOffset.r = atOffset.r;
            eyeOffset.pitch = atOffset.pitch;
            eyeOffset.yaw = (atOffset.yaw * rwData->yawDir) + camPlayerPosRot->rot.y;
            rwData->unk_0C -= 0.04f;
            break;
        case 3:
            temp_f0 = (rwData->animFrame - 0x9F) * (1.0f / 9.0f);

            sp5C.x = F32_LERPIMP(D_8011D678[2].x, D_8011D678[3].x, temp_f0);
            sp5C.y = F32_LERPIMP(D_8011D678[2].y, D_8011D678[3].y, temp_f0);
            sp5C.y += yOffset;
            sp5C.z = F32_LERPIMP(D_8011D678[2].z, D_8011D678[3].z, temp_f0);

            OLib_Vec3fToVecGeo(&atOffset, &sp5C);
            atOffset.yaw = (atOffset.yaw * rwData->yawDir) + camPlayerPosRot->rot.y;
            Camera_AddVecGeoToVec3f(at, &rwData->initialAt, &atOffset);

            atOffset.r = F32_LERPIMP(D_8011D658[2].r, D_8011D658[3].r, temp_f0);
            atOffset.pitch = BINANG_LERPIMP(D_8011D658[2].pitch, D_8011D658[3].pitch, temp_f0);
            atOffset.yaw = BINANG_LERPIMP(D_8011D658[2].yaw, D_8011D658[3].yaw, temp_f0);

            eyeOffset.r = atOffset.r;
            eyeOffset.pitch = atOffset.pitch;
            eyeOffset.yaw = (atOffset.yaw * rwData->yawDir) + camPlayerPosRot->rot.y;
            rwData->unk_0C += (4.0f / 45.0f);
            break;
        case 30:
            camera->stateFlags |= CAM_STATE_10;
            if (camera->stateFlags & CAM_STATE_3) {
                camera->animState = 4;
            }
            FALLTHROUGH;
        case 10:
        case 20:
            skipUpdateEye = true;
            break;
        case 4:
            eyeOffset.r = 80.0f;
            eyeOffset.pitch = 0;
            eyeOffset.yaw = eyeAtOffset.yaw;
            rwData->unk_0C = 0.1f;
            sCameraInterfaceField = CAM_INTERFACE_FIELD(CAM_LETTERBOX_LARGE, CAM_HUD_VISIBILITY_A, 0);

            if (!((rwData->animFrame < 0 || camera->xzSpeed > 0.001f ||
                   CHECK_BTN_ALL(D_8015BD7C->state.input[0].press.button, BTN_A) ||
                   CHECK_BTN_ALL(D_8015BD7C->state.input[0].press.button, BTN_B) ||
                   CHECK_BTN_ALL(D_8015BD7C->state.input[0].press.button, BTN_CLEFT) ||
                   CHECK_BTN_ALL(D_8015BD7C->state.input[0].press.button, BTN_CDOWN) ||
                   CHECK_BTN_ALL(D_8015BD7C->state.input[0].press.button, BTN_CUP) ||
                   CHECK_BTN_ALL(D_8015BD7C->state.input[0].press.button, BTN_CRIGHT) ||
                   CHECK_BTN_ALL(D_8015BD7C->state.input[0].press.button, BTN_R) ||
                   CHECK_BTN_ALL(D_8015BD7C->state.input[0].press.button, BTN_Z)) &&
                  (camera->stateFlags & CAM_STATE_3))) {
                goto skipeyeUpdate;
            }
            FALLTHROUGH;
        default:
            camera->stateFlags |= (CAM_STATE_2 | CAM_STATE_4);
            camera->stateFlags &= ~CAM_STATE_3;
            if (camera->prevBgCamIndex < 0) {
                Camera_ChangeSettingFlags(camera, camera->prevSetting, 2);
            } else {
                Camera_ChangeBgCamIndex(camera, camera->prevBgCamIndex);
                camera->prevBgCamIndex = -1;
            }
            sCameraInterfaceField = CAM_INTERFACE_FIELD(CAM_LETTERBOX_NONE, CAM_HUD_VISIBILITY_ALL, 0);
        skipeyeUpdate:
            skipUpdateEye = true;
            break;
    }

    rwData->animFrame++;

    if (rwData->animFrame == 1) {
        camera->animState = 10;
    } else if (rwData->animFrame == 2) {
        camera->animState = 1;
    } else if (rwData->animFrame == 148) {
        camera->animState = 2;
    } else if (rwData->animFrame == 158) {
        camera->animState = 20;
    } else if (rwData->animFrame == 159) {
        camera->animState = 3;
    } else if (rwData->animFrame == 168) {
        camera->animState = 30;
    } else if (rwData->animFrame == 228) {
        camera->animState = 4;
    }

    if (!skipUpdateEye) {
        eyeOffset.r = Camera_LERPCeilF(eyeOffset.r, eyeAtOffset.r, rwData->unk_0C, 2.0f);
        eyeOffset.pitch = Camera_LERPCeilS(eyeOffset.pitch, eyeAtOffset.pitch, rwData->unk_0C, 0xA);
        eyeOffset.yaw = Camera_LERPCeilS(eyeOffset.yaw, eyeAtOffset.yaw, rwData->unk_0C, 0xA);
        Camera_AddVecGeoToVec3f(eyeNext, at, &eyeOffset);
        *eye = *eyeNext;
    }

    camera->dist = OLib_Vec3fDist(at, eye);
    camera->atLERPStepScale = 0.1f;
    camera->posOffset.x = camera->at.x - camPlayerPosRot->pos.x;
    camera->posOffset.y = camera->at.y - camPlayerPosRot->pos.y;
    camera->posOffset.z = camera->at.z - camPlayerPosRot->pos.z;
    return true;
}

s32 Camera_Demo4(Camera* camera) {
    return Camera_Noop(camera);
}

/**
 * Sets up a cutscene for Camera_Uniq9
 */
s32 Camera_Demo5(Camera* camera) {
    f32 eyeTargetDist;
    f32 sp90;
    VecGeo playerTargetGeo;
    VecGeo eyePlayerGeo;
    s16 targetScreenPosX;
    s16 targetScreenPosY;
    s32 pad1;
    PosRot playerhead;
    PosRot targethead;
    Player* player;
    s16 sp4A;
    s32 framesDiff;
    s32 temp_v0;
    s16 t;
    s32 pad2;

    Actor_GetFocus(&playerhead, &camera->player->actor);
    player = camera->player;
    sCameraInterfaceField = CAM_INTERFACE_FIELD(CAM_LETTERBOX_LARGE, CAM_HUD_VISIBILITY_NOTHING_ALT, 0);
    if ((camera->target == NULL) || (camera->target->update == NULL)) {
        if (camera->target == NULL) {
            osSyncPrintf(VT_COL(YELLOW, BLACK) "camera: warning: attention: target is not valid, stop!\n" VT_RST);
        }
        camera->target = NULL;
        return true;
    }
    Actor_GetFocus(&camera->targetPosRot, camera->target);
    OLib_Vec3fDiffToVecGeo(&playerTargetGeo, &camera->targetPosRot.pos, &camera->playerPosRot.pos);
    D_8011D3AC = camera->target->category;
    Actor_GetScreenPos(camera->play, camera->target, &targetScreenPosX, &targetScreenPosY);
    eyeTargetDist = OLib_Vec3fDist(&camera->targetPosRot.pos, &camera->eye);
    OLib_Vec3fDiffToVecGeo(&eyePlayerGeo, &playerhead.pos, &camera->eyeNext);
    sp4A = eyePlayerGeo.yaw - playerTargetGeo.yaw;
    if (camera->target->category == ACTORCAT_PLAYER) {
        // camera is targeting a(the) player actor
        if (eyePlayerGeo.r > 30.0f) {
            D_8011D6AC[1].timerInit = camera->timer - 1;
            D_8011D6AC[1].atTargetInit.z = Rand_ZeroOne() * 10.0f;
            D_8011D6AC[1].eyeTargetInit.x = Rand_ZeroOne() * 10.0f;
            ONEPOINT_CS_INFO(camera)->keyFrames = D_8011D6AC;
            ONEPOINT_CS_INFO(camera)->keyFrameCnt = ARRAY_COUNT(D_8011D6AC);
            if (camera->parentCamId != CAM_ID_MAIN) {
                ONEPOINT_CS_INFO(camera)->keyFrameCnt--;
            } else {
                camera->timer += D_8011D6AC[2].timerInit;
            }
        } else {
            D_8011D724[1].eyeTargetInit.x = Rand_ZeroOne() * 10.0f;
            D_8011D724[1].timerInit = camera->timer - 1;
            ONEPOINT_CS_INFO(camera)->keyFrames = D_8011D724;
            ONEPOINT_CS_INFO(camera)->keyFrameCnt = ARRAY_COUNT(D_8011D724);
            if (camera->parentCamId != CAM_ID_MAIN) {
                ONEPOINT_CS_INFO(camera)->keyFrameCnt--;
            } else {
                camera->timer += D_8011D724[2].timerInit;
            }
        }
    } else if (playerTargetGeo.r < 30.0f) {
        // distance between player and target is less than 30 units.
        ONEPOINT_CS_INFO(camera)->keyFrames = D_8011D79C;
        ONEPOINT_CS_INFO(camera)->keyFrameCnt = ARRAY_COUNT(D_8011D79C);
        if ((targetScreenPosX < 0x15) || (targetScreenPosX >= 0x12C) || (targetScreenPosY < 0x29) ||
            (targetScreenPosY >= 0xC8)) {
            D_8011D79C[0].actionFlags = ONEPOINT_CS_ACTION(ONEPOINT_CS_ACTION_ID_1, true, false);
            D_8011D79C[0].atTargetInit.y = -30.0f;
            D_8011D79C[0].atTargetInit.x = 0.0f;
            D_8011D79C[0].atTargetInit.z = 0.0f;
            D_8011D79C[0].eyeTargetInit.y = 0.0f;
            D_8011D79C[0].eyeTargetInit.x = 10.0f;
            D_8011D79C[0].eyeTargetInit.z = -50.0f;
        }

        D_8011D79C[1].timerInit = camera->timer - 1;

        if (camera->parentCamId != CAM_ID_MAIN) {
            ONEPOINT_CS_INFO(camera)->keyFrameCnt -= 2;
        } else {
            camera->timer += D_8011D79C[2].timerInit + D_8011D79C[3].timerInit;
        }
    } else if (eyeTargetDist < 300.0f && eyePlayerGeo.r < 30.0f) {
        // distance from the camera's current positon and the target is less than 300 units
        // and the distance fromthe camera's current position to the player is less than 30 units
        D_8011D83C[0].timerInit = camera->timer;
        ONEPOINT_CS_INFO(camera)->keyFrames = D_8011D83C;
        ONEPOINT_CS_INFO(camera)->keyFrameCnt = ARRAY_COUNT(D_8011D83C);
        if (camera->parentCamId != CAM_ID_MAIN) {
            ONEPOINT_CS_INFO(camera)->keyFrameCnt--;
        } else {
            camera->timer += D_8011D83C[1].timerInit;
        }
    } else if (eyeTargetDist < 700.0f && ABS(sp4A) < 0x36B0) {
        // The distance between the camera's current position and the target is less than 700 units
        // and the angle between the camera's position and the player, and the player to the target
        // is less than ~76.9 degrees
        if (targetScreenPosX >= 0x15 && targetScreenPosX < 0x12C && targetScreenPosY >= 0x29 &&
            targetScreenPosY < 0xC8 && eyePlayerGeo.r > 30.0f) {
            D_8011D88C[0].timerInit = camera->timer;
            ONEPOINT_CS_INFO(camera)->keyFrames = D_8011D88C;
            ONEPOINT_CS_INFO(camera)->keyFrameCnt = ARRAY_COUNT(D_8011D88C);
            if (camera->parentCamId != CAM_ID_MAIN) {
                ONEPOINT_CS_INFO(camera)->keyFrameCnt--;
            } else {
                camera->timer += D_8011D88C[1].timerInit;
            }
        } else {
            D_8011D8DC[0].atTargetInit.z = eyeTargetDist * 0.6f;
            D_8011D8DC[0].eyeTargetInit.z = eyeTargetDist + 50.0f;
            D_8011D8DC[0].eyeTargetInit.x = Rand_ZeroOne() * 10.0f;
            if ((s16)(eyePlayerGeo.yaw - playerTargetGeo.yaw) > 0) {
                D_8011D8DC[0].atTargetInit.x = -D_8011D8DC[0].atTargetInit.x;
                D_8011D8DC[0].eyeTargetInit.x = -D_8011D8DC[0].eyeTargetInit.x;
                D_8011D8DC[0].rollTargetInit = -D_8011D8DC[0].rollTargetInit;
            }
            D_8011D8DC[0].timerInit = camera->timer;
            D_8011D8DC[1].timerInit = (s16)(eyeTargetDist * 0.005f) + 8;
            ONEPOINT_CS_INFO(camera)->keyFrames = D_8011D8DC;
            ONEPOINT_CS_INFO(camera)->keyFrameCnt = ARRAY_COUNT(D_8011D8DC);
            if (camera->parentCamId != CAM_ID_MAIN) {
                ONEPOINT_CS_INFO(camera)->keyFrameCnt -= 2;
            } else {
                camera->timer += D_8011D8DC[1].timerInit + D_8011D8DC[2].timerInit;
            }
        }
    } else if (camera->target->category == ACTORCAT_DOOR) {
        // the target is a door.
        D_8011D954[0].timerInit = camera->timer - 5;
        sp4A = 0;
        if (!func_800C0D34(camera->play, camera->target, &sp4A)) {
            osSyncPrintf(VT_COL(YELLOW, BLACK) "camera: attention demo: this door is dummy door!\n" VT_RST);
            if (ABS(playerTargetGeo.yaw - camera->target->shape.rot.y) >= 0x4000) {
                sp4A = camera->target->shape.rot.y;
            } else {
                sp4A = camera->target->shape.rot.y - 0x7FFF;
            }
        }

        D_8011D954[0].atTargetInit.y = D_8011D954[0].eyeTargetInit.y = D_8011D954[1].atTargetInit.y =
            camera->target->shape.rot.y == sp4A ? 180.0f : 0.0f;
        sp90 = ((s16)(playerTargetGeo.yaw - sp4A) < 0 ? 20.0f : -20.0f) * Rand_ZeroOne();
        D_8011D954[0].eyeTargetInit.y = D_8011D954->eyeTargetInit.y + sp90;
        temp_v0 = Rand_ZeroOne() * (sp90 * -0.2f);
        D_8011D954[1].rollTargetInit = temp_v0;
        D_8011D954[0].rollTargetInit = temp_v0;
        Actor_GetFocus(&targethead, camera->target);
        targethead.pos.x += 50.0f * Math_SinS(sp4A - 0x7FFF);
        targethead.pos.z += 50.0f * Math_CosS(sp4A - 0x7FFF);
        if (Camera_BGCheck(camera, &playerhead.pos, &targethead.pos)) {
            D_8011D954[1].actionFlags = ONEPOINT_CS_ACTION(ONEPOINT_CS_ACTION_ID_1, true, true);
            D_8011D954[2].actionFlags = ONEPOINT_CS_ACTION(ONEPOINT_CS_ACTION_ID_15, false, true);
        } else {
            D_8011D954[2].timerInit = (s16)(eyeTargetDist * 0.004f) + 6;
        }
        ONEPOINT_CS_INFO(camera)->keyFrames = D_8011D954;
        ONEPOINT_CS_INFO(camera)->keyFrameCnt = ARRAY_COUNT(D_8011D954);
        if (camera->parentCamId != CAM_ID_MAIN) {
            ONEPOINT_CS_INFO(camera)->keyFrameCnt -= 2;
        } else {
            camera->timer += D_8011D954[2].timerInit + D_8011D954[3].timerInit;
        }
    } else {
        if (playerTargetGeo.r < 200.0f) {
            D_8011D9F4[0].eyeTargetInit.z = playerTargetGeo.r;
            D_8011D9F4[0].atTargetInit.z = playerTargetGeo.r * 0.25f;
        }
        if (playerTargetGeo.r < 400.0f) {
            D_8011D9F4[0].eyeTargetInit.x = Rand_ZeroOne() * 25.0f;
        }
        Player_GetHeight(camera->player);
        D_8011D9F4[0].timerInit = camera->timer;
        Actor_GetFocus(&targethead, camera->target);
        if (Camera_BGCheck(camera, &playerhead.pos, &targethead.pos)) {
            D_8011D9F4[1].timerInit = 4;
            D_8011D9F4[1].actionFlags = ONEPOINT_CS_ACTION(ONEPOINT_CS_ACTION_ID_15, false, true);
        } else {
            t = eyeTargetDist * 0.005f;
            D_8011D9F4[1].timerInit = t + 8;
        }
        ONEPOINT_CS_INFO(camera)->keyFrames = D_8011D9F4;
        ONEPOINT_CS_INFO(camera)->keyFrameCnt = ARRAY_COUNT(D_8011D9F4);
        if (camera->parentCamId != CAM_ID_MAIN) {
            if (camera->play->state.frames & 1) {
                D_8011D9F4[0].rollTargetInit = -D_8011D9F4[0].rollTargetInit;
                D_8011D9F4[1].rollTargetInit = -D_8011D9F4[1].rollTargetInit;
            }
            ONEPOINT_CS_INFO(camera)->keyFrameCnt -= 2;
        } else {
            camera->timer += D_8011D9F4[1].timerInit + D_8011D9F4[2].timerInit;
            D_8011D9F4[0].rollTargetInit = D_8011D9F4[1].rollTargetInit = 0;
        }
    }

    framesDiff = sDemo5PrevSfxFrame - camera->play->state.frames;
    if ((framesDiff > 50) || (framesDiff < -50)) {
        func_80078884((u32)camera->data1);
    }

    sDemo5PrevSfxFrame = camera->play->state.frames;

    if (camera->player->stateFlags1 & PLAYER_STATE1_27 && (player->currentBoots != PLAYER_BOOTS_IRON)) {
        // swimming, and not iron boots
        player->stateFlags1 |= PLAYER_STATE1_29;
        // env frozen
        player->actor.freezeTimer = camera->timer;
    } else {
        sp4A = playerhead.rot.y - playerTargetGeo.yaw;
        if (camera->target->category == ACTORCAT_PLAYER) {
            framesDiff = camera->play->state.frames - sDemo5PrevAction12Frame;
            if (player->stateFlags1 & PLAYER_STATE1_11) {
                // holding object over head.
                func_8002DF54(camera->play, camera->target, 8);
            } else if (ABS(framesDiff) > 3000) {
                func_8002DF54(camera->play, camera->target, 12);
            } else {
                func_8002DF54(camera->play, camera->target, 69);
            }
        } else {
            func_8002DF54(camera->play, camera->target, 1);
        }
    }

    sDemo5PrevAction12Frame = camera->play->state.frames;
    Camera_ChangeSettingFlags(camera, CAM_SET_CS_C, (4 | 1));
    Camera_Unique9(camera);
    return true;
}

/**
 * Used in Forest Temple when poes are defeated, follows the flames to the torches.
 * Fixed position, rotates to follow the target
 */
s32 Camera_Demo6(Camera* camera) {
    Camera* mainCam;
    Demo6ReadOnlyData* roData = &camera->paramData.demo6.roData;
    Vec3f* eyeNext = &camera->eyeNext;
    CameraModeValue* values;
    VecGeo eyeOffset;
    Actor* camFocus;
    PosRot focusPosRot;
    s16 stateTimers[4];
    Demo6ReadWriteData* rwData = &camera->paramData.demo6.rwData;

    mainCam = Play_GetCamera(camera->play, CAM_ID_MAIN);
    camFocus = camera->target;
    stateTimers[1] = 55;
    stateTimers[2] = 70;
    stateTimers[3] = 90;

    if (RELOAD_PARAMS(camera) || R_RELOAD_CAM_PARAMS) {
        values = sCameraSettings[camera->setting].cameraModes[camera->mode].values;
        roData->interfaceField = GET_NEXT_RO_DATA(values);
    }

    if (R_RELOAD_CAM_PARAMS) {
        Camera_CopyPREGToModeValues(camera);
    }

    switch (camera->animState) {
        case 0:
            // initializes the camera state.
            rwData->animTimer = 0;
            camera->fov = 60.0f;
            Actor_GetWorld(&focusPosRot, camFocus);
            camera->at.x = focusPosRot.pos.x;
            camera->at.y = focusPosRot.pos.y + 20.0f;
            camera->at.z = focusPosRot.pos.z;
            eyeOffset.r = 200.0f;
            // 0x7D0 ~10.99 degrees
            eyeOffset.yaw = Camera_XZAngle(&focusPosRot.pos, &mainCam->playerPosRot.pos) + 0x7D0;
            // -0x3E8 ~5.49 degrees
            eyeOffset.pitch = -0x3E8;
            Camera_AddVecGeoToVec3f(eyeNext, &camera->at, &eyeOffset);
            camera->eye = *eyeNext;
            camera->animState++;
            FALLTHROUGH;
        case 1:
            if (stateTimers[camera->animState] < rwData->animTimer) {
                func_8002DF54(camera->play, &camera->player->actor, 8);
                Actor_GetWorld(&focusPosRot, camFocus);
                rwData->atTarget.x = focusPosRot.pos.x;
                rwData->atTarget.y = focusPosRot.pos.y - 20.0f;
                rwData->atTarget.z = focusPosRot.pos.z;
                camera->animState++;
            } else {
                break;
            }
            FALLTHROUGH;
        case 2:
            Camera_LERPCeilVec3f(&rwData->atTarget, &camera->at, 0.1f, 0.1f, 8.0f);
            if (stateTimers[camera->animState] < rwData->animTimer) {
                camera->animState++;
            } else {
                break;
            }
            FALLTHROUGH;
        case 3:
            camera->fov = Camera_LERPCeilF(50.0f, camera->fov, 0.2f, 0.01f);
            if (stateTimers[camera->animState] < rwData->animTimer) {
                camera->timer = 0;
                return true;
            }
            break;
    }

    rwData->animTimer++;
    Actor_GetWorld(&focusPosRot, camFocus);

    return true;
}

s32 Camera_Demo7(Camera* camera) {
    if (camera->animState == 0) {
        camera->stateFlags &= ~CAM_STATE_2;
        camera->stateFlags |= CAM_STATE_12;
        camera->animState++;
    }
    //! @bug doesn't return
}

s32 Camera_Demo8(Camera* camera) {
    return Camera_Noop(camera);
}

/**
 * Camera follows points specified by demo9.atPoints and demo9.eyePoints, allows finer control
 * over the final eye and at points than Camera_Demo1, by allowing the interpolated at and eye points
 * to be relative to the main camera's player, the current camera's player, or the main camera's target
 */
s32 Camera_Demo9(Camera* camera) {
    s32 pad;
    s32 finishAction;
    s16 onePointTimer;
    OnePointCamData* onePointCamData = &camera->paramData.demo9.onePointCamData;
    Vec3f csEyeUpdate;
    Vec3f csAtUpdate;
    Vec3f newEye;
    Vec3f newAt;
    f32 newRoll;
    CameraModeValue* values;
    Camera* mainCam;
    Vec3f* eye = &camera->eye;
    PosRot* mainCamPlayerPosRot;
    PosRot focusPosRot;
    s32 pad3;
    Vec3f* eyeNext = &camera->eyeNext;
    Demo9ReadOnlyData* roData = &camera->paramData.demo9.roData;
    Vec3f* at = &camera->at;
    f32* camFOV = &camera->fov;
    Demo9ReadWriteData* rwData = &camera->paramData.demo9.rwData;

    mainCam = Play_GetCamera(camera->play, CAM_ID_MAIN);
    mainCamPlayerPosRot = &mainCam->playerPosRot;
    if (RELOAD_PARAMS(camera) || R_RELOAD_CAM_PARAMS) {
        values = sCameraSettings[camera->setting].cameraModes[camera->mode].values;
        roData->interfaceField = GET_NEXT_RO_DATA(values);
    }

    if (R_RELOAD_CAM_PARAMS) {
        Camera_CopyPREGToModeValues(camera);
    }

    sCameraInterfaceField = roData->interfaceField;

    switch (camera->animState) {
        case 0:
            // initialize the camera state
            rwData->keyframe = 0;
            rwData->finishAction = 0;
            rwData->curFrame = 0.0f;
            camera->animState++;
            rwData->doLERPAt = false;
            finishAction = onePointCamData->actionParameters & 0xF000;
            if (finishAction != 0) {
                rwData->finishAction = finishAction;

                // Clear finish parameters
                onePointCamData->actionParameters &= 0xFFF;
            }
            rwData->animTimer = onePointCamData->initTimer;
            FALLTHROUGH;
        case 1:
            // Run the camera state
            if (rwData->animTimer > 0) {
                // if the animation timer is still running, run the demo logic
                // if it is not, then the case will fallthrough to the finish logic.

                // Run the at and eye cs interpolation functions, if either of them returns 1 (that no more points
                // exist) change the animation state to 2 (standby)
                if (func_800BB2B4(&csEyeUpdate, &newRoll, camFOV, onePointCamData->eyePoints, &rwData->keyframe,
                                  &rwData->curFrame) != 0 ||
                    func_800BB2B4(&csAtUpdate, &newRoll, camFOV, onePointCamData->atPoints, &rwData->keyframe,
                                  &rwData->curFrame) != 0) {
                    camera->animState = 2;
                }

                if (onePointCamData->actionParameters == 1) {
                    // rotate around mainCam's player
                    Camera_RotateAroundPoint(mainCamPlayerPosRot, &csEyeUpdate, &newEye);
                    Camera_RotateAroundPoint(mainCamPlayerPosRot, &csAtUpdate, &newAt);
                } else if (onePointCamData->actionParameters == 4) {
                    // rotate around the current camera's player
                    Actor_GetWorld(&focusPosRot, &camera->player->actor);
                    Camera_RotateAroundPoint(&focusPosRot, &csEyeUpdate, &newEye);
                    Camera_RotateAroundPoint(&focusPosRot, &csAtUpdate, &newAt);
                } else if (onePointCamData->actionParameters == 8) {
                    // rotate around the current camera's target
                    if (camera->target != NULL && camera->target->update != NULL) {
                        Actor_GetWorld(&focusPosRot, camera->target);
                        Camera_RotateAroundPoint(&focusPosRot, &csEyeUpdate, &newEye);
                        Camera_RotateAroundPoint(&focusPosRot, &csAtUpdate, &newAt);
                    } else {
                        camera->target = NULL;
                        newEye = *eye;
                        newAt = *at;
                    }
                } else {
                    // simple copy
                    Camera_Vec3fCopy(&csEyeUpdate, &newEye);
                    Camera_Vec3fCopy(&csAtUpdate, &newAt);
                }

                *eyeNext = newEye;
                *eye = *eyeNext;
                if (rwData->doLERPAt) {
                    Camera_LERPCeilVec3f(&newAt, at, 0.5f, 0.5f, 0.1f);
                } else {
                    *at = newAt;
                    rwData->doLERPAt = true;
                }
                camera->roll = newRoll * 256.0f;
                rwData->animTimer--;
                break;
            }
            FALLTHROUGH;
        case 3:
            // the cs is finished, decide the next action
            camera->timer = 0;
            if (rwData->finishAction != 0) {
                if (rwData->finishAction != 0x1000) {
                    if (rwData->finishAction == 0x2000) {
                        // finish action = 0x2000, run OnePointCs 0x3FC (Dramatic Return to Link)
                        onePointTimer = onePointCamData->initTimer < 50 ? 5 : onePointCamData->initTimer / 5;
                        OnePointCutscene_Init(camera->play, 1020, onePointTimer, NULL, camera->parentCamId);
                    }
                } else {
                    // finish action = 0x1000, copy the current camera's values to the
                    // default camera.
                    Camera_Copy(mainCam, camera);
                }
            }
            break;
        case 2:
            // standby while the timer finishes, change the animState to finish when
            // the timer runs out.
            rwData->animTimer--;
            if (rwData->animTimer < 0) {
                camera->animState++;
            }
            break;
        case 4:
            // do nothing.
            break;
    }

    return true;
}

s32 Camera_Demo0(Camera* camera) {
    return Camera_Noop(camera);
}

s32 Camera_Special0(Camera* camera) {
    PosRot* playerPosRot = &camera->playerPosRot;
    Special0ReadOnlyData* roData = &camera->paramData.spec0.roData;

    if (RELOAD_PARAMS(camera) || R_RELOAD_CAM_PARAMS) {
        CameraModeValue* values = sCameraSettings[camera->setting].cameraModes[camera->mode].values;

        roData->lerpAtScale = GET_NEXT_SCALED_RO_DATA(values);
        roData->interfaceField = GET_NEXT_RO_DATA(values);
    }

    if (R_RELOAD_CAM_PARAMS) {
        Camera_CopyPREGToModeValues(camera);
    }

    sCameraInterfaceField = roData->interfaceField;

    if (camera->animState == 0) {
        camera->animState++;
    }

    if ((camera->target == NULL) || (camera->target->update == NULL)) {
        if (camera->target == NULL) {
            osSyncPrintf(VT_COL(YELLOW, BLACK) "camera: warning: circle: target is not valid, stop!\n" VT_RST);
        }
        camera->target = NULL;
        return true;
    }

    Actor_GetFocus(&camera->targetPosRot, camera->target);
    Camera_LERPCeilVec3f(&camera->targetPosRot.pos, &camera->at, roData->lerpAtScale, roData->lerpAtScale, 0.1f);

    camera->posOffset.x = camera->at.x - playerPosRot->pos.x;
    camera->posOffset.y = camera->at.y - playerPosRot->pos.y;
    camera->posOffset.z = camera->at.z - playerPosRot->pos.z;

    camera->dist = OLib_Vec3fDist(&camera->at, &camera->eye);
    camera->xzSpeed = 0.0f;
    if (camera->timer > 0) {
        camera->timer--;
    }
    return true;
}

s32 Camera_Special1(Camera* camera) {
    return Camera_Noop(camera);
}

s32 Camera_Special2(Camera* camera) {
    return Camera_Unique2(camera);
}

s32 Camera_Special3(Camera* camera) {
    return Camera_Noop(camera);
}

s32 Camera_Special4(Camera* camera) {
    PosRot curTargetPosRot;
    s16 sp3A;
    s16* timer = &camera->timer;
    Special4ReadWriteData* rwData = &camera->paramData.spec4.rwData;

    if (camera->animState == 0) {
        sCameraInterfaceField = CAM_INTERFACE_FIELD(CAM_LETTERBOX_LARGE, CAM_HUD_VISIBILITY_NOTHING_ALT, 0);
        camera->fov = 40.0f;
        camera->animState++;
        rwData->initalTimer = camera->timer;
    }

    camera->fov = Camera_LERPCeilF(80.0f, camera->fov, 1.0f / *timer, 0.1f);
    if ((rwData->initalTimer - *timer) < 0xF) {
        (*timer)--;
        return false;
    } else {
        camera->roll = -0x1F4;
        Actor_GetWorld(&curTargetPosRot, camera->target);

        camera->at = curTargetPosRot.pos;
        camera->at.y -= 150.0f;

        // 0x3E8 ~ 5.49 degrees
        sp3A = (s16)(curTargetPosRot.rot.y - 0x7FFF) + 0x3E8;
        camera->eye.x = camera->eyeNext.x = (Math_SinS(sp3A) * 780.0f) + camera->at.x;
        camera->eyeNext.y = camera->at.y;
        camera->eye.z = camera->eyeNext.z = (Math_CosS(sp3A) * 780.0f) + camera->at.z;
        camera->eye.y = curTargetPosRot.pos.y;
        camera->eye.y = Camera_GetFloorY(camera, &camera->eye) + 20.0f;
        (*timer)--;
        return true;
    }
}

/**
 * Flying with hookshot
 */
s32 Camera_Special5(Camera* camera) {
    Vec3f* eye = &camera->eye;
    Vec3f* at = &camera->at;
    Vec3f* eyeNext = &camera->eyeNext;
    PosRot spA8;
    s16 pad;
    s16 spA4;
    CamColChk sp7C;
    VecGeo sp74;
    VecGeo sp6C;
    VecGeo sp64;
    VecGeo sp5C;
    PosRot* playerPosRot = &camera->playerPosRot;
    Special5ReadOnlyData* roData = &camera->paramData.spec5.roData;
    Special5ReadWriteData* rwData = &camera->paramData.spec5.rwData;
    f32 temp_f0_2;
    f32 yOffset;

    yOffset = Player_GetHeight(camera->player);
    if (RELOAD_PARAMS(camera) || R_RELOAD_CAM_PARAMS) {
        CameraModeValue* values = sCameraSettings[camera->setting].cameraModes[camera->mode].values;
        f32 yNormal =
            1.0f + CAM_DATA_SCALED(R_CAM_YOFFSET_NORM) - (CAM_DATA_SCALED(R_CAM_YOFFSET_NORM) * (68.0f / yOffset));

        roData->yOffset = (GET_NEXT_SCALED_RO_DATA(values) * yOffset) * yNormal;
        roData->eyeDist = GET_NEXT_RO_DATA(values);
        roData->minDistForRot = GET_NEXT_RO_DATA(values);
        roData->timerInit = GET_NEXT_RO_DATA(values);
        roData->pitch = CAM_DEG_TO_BINANG(GET_NEXT_RO_DATA(values));
        roData->fovTarget = GET_NEXT_RO_DATA(values);
        roData->atMaxLERPScale = GET_NEXT_SCALED_RO_DATA(values);
        roData->interfaceField = GET_NEXT_RO_DATA(values);
    }

    if (R_RELOAD_CAM_PARAMS) {
        Camera_CopyPREGToModeValues(camera);
    }

    OLib_Vec3fDiffToVecGeo(&sp64, at, eye);
    OLib_Vec3fDiffToVecGeo(&sp5C, at, eyeNext);
    Actor_GetWorld(&spA8, camera->target);

    sCameraInterfaceField = roData->interfaceField;

    if (camera->animState == 0) {
        camera->animState++;
        rwData->animTimer = roData->timerInit;
    }

    if (rwData->animTimer > 0) {
        rwData->animTimer--;
    } else if (rwData->animTimer == 0) {
        if (camera->target == NULL || camera->target->update == NULL) {
            camera->target = NULL;
            return true;
        }

        rwData->animTimer--;
        if (roData->minDistForRot < OLib_Vec3fDist(&spA8.pos, &playerPosRot->pos)) {
            sp6C.yaw = playerPosRot->rot.y;
            sp6C.pitch = -playerPosRot->rot.x;
            sp6C.r = 20.0f;
            Camera_AddVecGeoToVec3f(&sp7C.pos, &spA8.pos, &sp6C);
            Camera_BGCheckInfo(camera, at, &sp7C);
            OLib_Vec3fToVecGeo(&sp6C, &sp7C.norm);
            spA4 = playerPosRot->rot.y - sp6C.yaw;
            sp74.r = roData->eyeDist;
            temp_f0_2 = Rand_ZeroOne();
            sp74.yaw =
                (s16)(playerPosRot->rot.y - 0x7FFF) + (s16)(spA4 < 0 ? -(s16)(0x1553 + (s16)(temp_f0_2 * 2730.0f))
                                                                     : (s16)(0x1553 + (s16)(temp_f0_2 * 2730.0f)));
            sp74.pitch = roData->pitch;
            Camera_AddVecGeoToVec3f(eyeNext, &spA8.pos, &sp74);
            *eye = *eyeNext;
            Camera_BGCheck(camera, &spA8.pos, eye);
        }
    }

    Camera_CalcAtDefault(camera, &sp5C, roData->yOffset, 0);
    camera->fov = Camera_LERPCeilF(roData->fovTarget, camera->fov,
                                   camera->atLERPStepScale * CAM_DATA_SCALED(R_CAM_FOV_UPDATE_RATE), 1.0f);
    camera->roll = Camera_LERPCeilS(0, camera->roll, 0.5f, 0xA);
    camera->atLERPStepScale = Camera_ClampLERPScale(camera, roData->atMaxLERPScale);
    return true;
}

/**
 * Camera's eye is fixed at points specified at lower or upper points depending on the player's position.
 * Designed around 4 specific elevator platforms, 1 in spirit temple and 3 in fire temple.
 * Used by `CAM_SET_ELEVATOR_PLATFORM`
 */
s32 Camera_Special7(Camera* camera) {
    Special7ReadWriteData* rwData = &camera->paramData.spec7.rwData;
    PosRot* playerPosRot = &camera->playerPosRot;
    Vec3f atTarget;
    f32 yOffset = Player_GetHeight(camera->player);
    f32 fovRollParam;

    if (camera->animState == 0) {
        // Use sceneIds and hardcoded positions in the fire temple to identify the 4 platforms
        if (camera->play->sceneId == SCENE_SPIRIT_TEMPLE) {
            rwData->index = CAM_ELEVATOR_PLATFORM_SPIRIT_TEMPLE_ENTRANCE;
        } else {
            // Hardcoded positions in the fire temple
            if (playerPosRot->pos.x < 1500.0f) {
                rwData->index = CAM_ELEVATOR_PLATFORM_FIRE_TEMPLE_WEST_TOWER;
            } else if (playerPosRot->pos.y < 3000.0f) {
                rwData->index = CAM_ELEVATOR_PLATFORM_FIRE_TEMPLE_LOWER_FLOOR;
            } else {
                rwData->index = CAM_ELEVATOR_PLATFORM_FIRE_TEMPLE_EAST_TOWER;
            }
        }
        camera->animState++;
        camera->roll = 0;
    }

    if (camera->at.y < sCamElevatorPlatformTogglePosY[rwData->index]) {
        // Cam at lower position

        // look at player
        atTarget = playerPosRot->pos;
        atTarget.y -= 20.0f;
        Camera_LERPCeilVec3f(&atTarget, &camera->at, 0.4f, 0.4f, 0.10f);

        // place camera based on hard-coded positions
        camera->eye = camera->eyeNext = sCamElevatorPlatformLowerEyePoints[rwData->index];

        fovRollParam =
            (playerPosRot->pos.y - sCamElevatorPlatformFovRollParam[rwData->index]) /
            (sCamElevatorPlatformTogglePosY[rwData->index] - sCamElevatorPlatformFovRollParam[rwData->index]);
        camera->roll = sCamElevatorPlatformRolls[rwData->index] * fovRollParam;
        camera->fov = 60.0f + (20.0f * fovRollParam);
    } else {
        // Cam at upper position

        // look at player
        atTarget = playerPosRot->pos;
        atTarget.y += yOffset;
        Camera_LERPCeilVec3f(&atTarget, &camera->at, 0.4f, 0.4f, 0.1f);

        camera->roll = 0;
        // place camera based on hard-coded positions
        camera->eye = camera->eyeNext = sCamElevatorPlatformUpperEyePoints[rwData->index];
        camera->fov = 70.0f;
    }

    camera->dist = OLib_Vec3fDist(&camera->at, &camera->eye);
    camera->atLERPStepScale = 0.0f;
    camera->posOffset.x = camera->at.x - playerPosRot->pos.x;
    camera->posOffset.y = camera->at.y - playerPosRot->pos.y;
    camera->posOffset.z = camera->at.z - playerPosRot->pos.z;
    return true;
}

/**
 * Courtyard.
 * Camera's eye is fixed on the z plane, slides on the xy plane with link
 * When the camera's scene data changes the animation to the next "screen"
 * happens for 12 frames.  The camera's eyeNext is the scene's camera data's position
 */
s32 Camera_Special6(Camera* camera) {
    Vec3f* eye = &camera->eye;
    Vec3f* at = &camera->at;
    Vec3f* eyeNext = &camera->eyeNext;
    VecGeo atOffset;
    Vec3f bgCamPos;
    Vec3f eyePosCalc;
    Vec3f eyeAnim;
    Vec3f atAnim;
    VecGeo eyeAtOffset;
    PosRot* playerPosRot = &camera->playerPosRot;
    BgCamFuncData* bgCamFuncData;
    Vec3s bgCamRot;
    s16 fov;
    f32 sp54;
    f32 timerF;
    f32 timerDivisor;
    Special6ReadOnlyData* roData = &camera->paramData.spec6.roData;
    Special6ReadWriteData* rwData = &camera->paramData.spec6.rwData;
    s32 pad;

    if (RELOAD_PARAMS(camera) || R_RELOAD_CAM_PARAMS) {
        CameraModeValue* values = sCameraSettings[camera->setting].cameraModes[camera->mode].values;

        roData->interfaceField = GET_NEXT_RO_DATA(values);
    }

    if (R_RELOAD_CAM_PARAMS) {
        Camera_CopyPREGToModeValues(camera);
    }

    OLib_Vec3fDiffToVecGeo(&eyeAtOffset, eye, at);

    bgCamFuncData = (BgCamFuncData*)Camera_GetBgCamFuncData(camera);
    Camera_Vec3sToVec3f(&bgCamPos, &bgCamFuncData->pos);
    bgCamRot = bgCamFuncData->rot;
    fov = bgCamFuncData->fov;
    if (fov == -1) {
        fov = 6000;
    }

    if (fov <= 360) {
        fov *= 100;
    }

    sCameraInterfaceField = roData->interfaceField;

    if (eyeNext->x != bgCamPos.x || eyeNext->y != bgCamPos.y || eyeNext->z != bgCamPos.z || camera->animState == 0) {
        // A change in the current scene's camera positon has been detected,
        // Change "screens"
        camera->player->actor.freezeTimer = 12;
        // Overwrite hud visibility to CAM_HUD_VISIBILITY_HEARTS_FORCE
        sCameraInterfaceField =
            (sCameraInterfaceField & (u16)~CAM_HUD_VISIBILITY_MASK) | CAM_HUD_VISIBILITY_HEARTS_FORCE;
        rwData->initalPlayerY = playerPosRot->pos.y;
        rwData->animTimer = 12;
        *eyeNext = bgCamPos;
        if (camera->animState == 0) {
            camera->animState++;
        }
    }

    if (rwData->animTimer > 0) {
        // In transition between "screens"
        timerF = rwData->animTimer;
        eyePosCalc = *eyeNext;
        eyePosCalc.x += (playerPosRot->pos.x - eyePosCalc.x) * 0.5f;
        eyePosCalc.y += (playerPosRot->pos.y - rwData->initalPlayerY) * 0.2f;
        eyeAnim = eyePosCalc;
        eyeAnim.y = Camera_LERPCeilF(eyePosCalc.y, eye->y, 0.5f, 0.01f);

        // set the at point to be 100 units from the eye looking at the
        // direction specified in the scene's camera data.
        atOffset.r = 100.0f;
        atOffset.yaw = bgCamRot.y;
        atOffset.pitch = -bgCamRot.x;
        Camera_AddVecGeoToVec3f(&atAnim, &eyeAnim, &atOffset);
        timerDivisor = 1.0f / timerF;
        eye->x += (eyeAnim.x - eye->x) * timerDivisor;
        eye->y += (eyeAnim.y - eye->y) * timerDivisor;
        eye->z += (eyeAnim.z - eye->z) * timerDivisor;
        at->x += (atAnim.x - at->x) * timerDivisor;
        at->y += (atAnim.y - at->y) * timerDivisor;
        at->z += (atAnim.z - at->z) * timerDivisor;
        camera->fov += (CAM_DATA_SCALED(fov) - camera->fov) / rwData->animTimer;
        rwData->animTimer--;
    } else {
        // Camera following player on the x axis.
        // Overwrite hud visibility to CAM_HUD_VISIBILITY_ALL
        sCameraInterfaceField = (sCameraInterfaceField & (u16)~CAM_HUD_VISIBILITY_MASK) | CAM_HUD_VISIBILITY_ALL;
        eyePosCalc = *eyeNext;
        eyePosCalc.x += (playerPosRot->pos.x - eyePosCalc.x) * 0.5f;
        eyePosCalc.y += (playerPosRot->pos.y - rwData->initalPlayerY) * 0.2f;
        *eye = eyePosCalc;
        eye->y = Camera_LERPCeilF(eyePosCalc.y, eye->y, 0.5f, 0.01f);

        // set the at point to be 100 units from the eye looking at the
        // direction specified in the scene's camera data.
        atOffset.r = 100.0f;
        atOffset.yaw = bgCamRot.y;
        atOffset.pitch = -bgCamRot.x;
        Camera_AddVecGeoToVec3f(at, eye, &atOffset);
    }
    return true;
}

s32 Camera_Special8(Camera* camera) {
    return Camera_Noop(camera);
}

s32 Camera_Special9(Camera* camera) {
    s32 pad;
    Vec3f* eye = &camera->eye;
    Vec3f* at = &camera->at;
    Vec3f* eyeNext = &camera->eyeNext;
    Vec3f spAC;
    VecGeo eyeAdjustment;
    VecGeo atEyeOffsetGeo;
    f32 playerYOffset;
    s32 pad3;
    PosRot* playerPosRot = &camera->playerPosRot;
    PosRot adjustedPlayerPosRot;
    f32 yNormal;
    DoorParams* doorParams = &camera->paramData.doorParams;
    Special9ReadOnlyData* roData = &camera->paramData.spec9.roData;
    Special9ReadWriteData* rwData = &camera->paramData.spec9.rwData;
    s32 pad4;
    BgCamFuncData* bgCamFuncData;

    playerYOffset = Player_GetHeight(camera->player);
    camera->stateFlags &= ~CAM_STATE_4;
    yNormal =
        1.0f + CAM_DATA_SCALED(R_CAM_YOFFSET_NORM) - (CAM_DATA_SCALED(R_CAM_YOFFSET_NORM) * (68.0f / playerYOffset));

    if (RELOAD_PARAMS(camera) || R_RELOAD_CAM_PARAMS) {
        CameraModeValue* values = sCameraSettings[camera->setting].cameraModes[camera->mode].values;

        roData->yOffset = GET_NEXT_SCALED_RO_DATA(values) * playerYOffset * yNormal;
        roData->unk_04 = GET_NEXT_RO_DATA(values);
        roData->interfaceField = GET_NEXT_RO_DATA(values);
    }

    if (R_RELOAD_CAM_PARAMS) {
        Camera_CopyPREGToModeValues(camera);
    }

    if (doorParams->doorActor != NULL) {
        Actor_GetWorldPosShapeRot(&adjustedPlayerPosRot, doorParams->doorActor);
    } else {
        adjustedPlayerPosRot = *playerPosRot;
        adjustedPlayerPosRot.pos.y += playerYOffset + roData->yOffset;
        adjustedPlayerPosRot.rot.x = 0;
    }

    OLib_Vec3fDiffToVecGeo(&atEyeOffsetGeo, at, eye);

    sCameraInterfaceField = roData->interfaceField;

    switch (camera->animState) {
        if (1) {}

        case 0:
            camera->stateFlags &= ~(CAM_STATE_1 | CAM_STATE_2);
            camera->animState++;
            rwData->targetYaw = ABS(playerPosRot->rot.y - adjustedPlayerPosRot.rot.y) >= 0x4000
                                    ? adjustedPlayerPosRot.rot.y - 0x7FFF
                                    : adjustedPlayerPosRot.rot.y;
            FALLTHROUGH;
        case 1:
            doorParams->timer1--;
            if (doorParams->timer1 <= 0) {
                camera->animState++;
                if (roData->interfaceField & SPECIAL9_FLAG_0) {
                    bgCamFuncData = (BgCamFuncData*)Camera_GetBgCamFuncData(camera);
                    Camera_Vec3sToVec3f(eyeNext, &bgCamFuncData->pos);
                    spAC = *eye = *eyeNext;
                } else {
                    s16 yaw;

                    // 0xE38 ~ 20 degrees
                    eyeAdjustment.pitch = 0xE38;
                    // 0xAAA ~ 15 degrees.
                    yaw = 0xAAA * ((camera->play->state.frames & 1) ? 1 : -1);
                    eyeAdjustment.yaw = rwData->targetYaw + yaw;
                    eyeAdjustment.r = 200.0f * yNormal;
                    Camera_AddVecGeoToVec3f(eyeNext, at, &eyeAdjustment);
                    spAC = *eye = *eyeNext;
                    if (Camera_CheckOOB(camera, &spAC, &playerPosRot->pos)) {
                        yaw = -yaw;
                        eyeAdjustment.yaw = rwData->targetYaw + yaw;
                        Camera_AddVecGeoToVec3f(eyeNext, at, &eyeAdjustment);
                        *eye = *eyeNext;
                    }
                }
            } else {
                break;
            }
            FALLTHROUGH;
        case 2:
            spAC = playerPosRot->pos;
            spAC.y += playerYOffset + roData->yOffset;

            Camera_LERPCeilVec3f(&spAC, at, 0.25f, 0.25f, 0.1f);
            doorParams->timer2--;
            if (doorParams->timer2 <= 0) {
                camera->animState++;
                rwData->targetYaw = rwData->targetYaw - 0x7FFF;
            } else {
                break;
            }
            FALLTHROUGH;
        case 3:
            spAC = playerPosRot->pos;
            spAC.y += (playerYOffset + roData->yOffset);
            Camera_LERPCeilVec3f(&spAC, at, 0.5f, 0.5f, 0.1f);
            eyeAdjustment.pitch = Camera_LERPCeilS(0xAAA, atEyeOffsetGeo.pitch, 0.3f, 0xA);
            eyeAdjustment.yaw = Camera_LERPCeilS(rwData->targetYaw, atEyeOffsetGeo.yaw, 0.3f, 0xA);
            eyeAdjustment.r = Camera_LERPCeilF(60.0f, atEyeOffsetGeo.r, 0.3f, 1.0f);
            Camera_AddVecGeoToVec3f(eyeNext, at, &eyeAdjustment);
            *eye = *eyeNext;
            doorParams->timer3--;
            if (doorParams->timer3 <= 0) {
                camera->animState++;
            } else {
                break;
            }
            FALLTHROUGH;
        case 4:
            camera->animState++;
            FALLTHROUGH;
        default:
            camera->stateFlags |= (CAM_STATE_4 | CAM_STATE_10);
            sCameraInterfaceField = CAM_INTERFACE_FIELD(CAM_LETTERBOX_NONE, CAM_HUD_VISIBILITY_ALL, 0);

            if (camera->xzSpeed > 0.001f || CHECK_BTN_ALL(D_8015BD7C->state.input[0].press.button, BTN_A) ||
                CHECK_BTN_ALL(D_8015BD7C->state.input[0].press.button, BTN_B) ||
                CHECK_BTN_ALL(D_8015BD7C->state.input[0].press.button, BTN_CLEFT) ||
                CHECK_BTN_ALL(D_8015BD7C->state.input[0].press.button, BTN_CDOWN) ||
                CHECK_BTN_ALL(D_8015BD7C->state.input[0].press.button, BTN_CUP) ||
                CHECK_BTN_ALL(D_8015BD7C->state.input[0].press.button, BTN_CRIGHT) ||
                CHECK_BTN_ALL(D_8015BD7C->state.input[0].press.button, BTN_R) ||
                CHECK_BTN_ALL(D_8015BD7C->state.input[0].press.button, BTN_Z) ||
                (roData->interfaceField & SPECIAL9_FLAG_3)) {

                Camera_ChangeSettingFlags(camera, camera->prevSetting, 2);
                camera->stateFlags |= (CAM_STATE_1 | CAM_STATE_2);
            }
            break;
    }
    if (1) {}
    spAC = playerPosRot->pos;
    spAC.y += playerYOffset;
    camera->dist = OLib_Vec3fDist(&spAC, eye);
    camera->posOffset.x = camera->at.x - playerPosRot->pos.x;
    camera->posOffset.y = camera->at.y - playerPosRot->pos.y;
    camera->posOffset.z = camera->at.z - playerPosRot->pos.z;
    return true;
}

Camera* Camera_Create(View* view, CollisionContext* colCtx, PlayState* play) {
    Camera* newCamera = ZeldaArena_MallocDebug(sizeof(*newCamera), "../z_camera.c", 9370);

    if (newCamera != NULL) {
        osSyncPrintf(VT_FGCOL(BLUE) "camera: create --- allocate %d byte" VT_RST "\n", sizeof(*newCamera) * 4);
        Camera_Init(newCamera, view, colCtx, play);
    } else {
        osSyncPrintf(VT_COL(RED, WHITE) "camera: create: not enough memory\n" VT_RST);
    }
    return newCamera;
}

void Camera_Destroy(Camera* camera) {
    if (camera != NULL) {
        osSyncPrintf(VT_FGCOL(BLUE) "camera: destroy ---" VT_RST "\n");
        ZeldaArena_FreeDebug(camera, "../z_camera.c", 9391);
    } else {
        osSyncPrintf(VT_COL(YELLOW, BLACK) "camera: destroy: already cleared\n" VT_RST);
    }
}

void Camera_Init(Camera* camera, View* view, CollisionContext* colCtx, PlayState* play) {
    Camera* camP;
    s32 i;
    s16 curUID;
    s16 j;

    __osMemset(camera, 0, sizeof(Camera));
    if (sInitRegs) {
        for (i = 0; i < sOREGInitCnt; i++) {
            OREG(i) = sOREGInit[i];
        }

        for (i = 0; i < sCamDataRegsInitCount; i++) {
            R_CAM_DATA(i) = sCamDataRegsInit[i];
        }

        DbCamera_Reset(camera, &D_8015BD80);
        sInitRegs = false;
        PREG(88) = -1;
    }
    camera->play = D_8015BD7C = play;
    DbCamera_Init(&D_8015BD80, camera);
    curUID = sNextUID;
    sNextUID++;
    while (curUID != 0) {
        if (curUID == 0) {
            sNextUID++;
        }

        for (j = 0; j < NUM_CAMS; j++) {
            camP = camera->play->cameraPtrs[j];
            if (camP != NULL && curUID == camP->uid) {
                break;
            }
        }

        if (j == 4) {
            break;
        }

        curUID = sNextUID++;
    }

    // ~ 90 degrees
    camera->inputDir.y = 0x3FFF;
    camera->uid = curUID;
    camera->camDir = camera->inputDir;
    camera->rUpdateRateInv = 10.0f;
    camera->yawUpdateRateInv = 10.0f;
    camera->up.x = 0.0f;
    camera->up.y = 1.0f;
    camera->up.z = 0.0f;
    camera->fov = 60.0f;
    camera->pitchUpdateRateInv = R_CAM_PITCH_UPDATE_RATE_INV;
    camera->xzOffsetUpdateRate = CAM_DATA_SCALED(R_CAM_XZ_OFFSET_UPDATE_RATE);
    camera->yOffsetUpdateRate = CAM_DATA_SCALED(R_CAM_Y_OFFSET_UPDATE_RATE);
    camera->fovUpdateRate = CAM_DATA_SCALED(R_CAM_FOV_UPDATE_RATE);
    sCameraLetterboxSize = 32;
    sCameraHudVisibilityMode = HUD_VISIBILITY_NO_CHANGE;
    camera->stateFlags = 0;
    camera->setting = camera->prevSetting = CAM_SET_FREE0;
    camera->bgCamIndex = camera->prevBgCamIndex = -1;
    camera->mode = 0;
    camera->bgId = BGCHECK_SCENE;
    camera->csId = 0x7FFF;
    camera->timer = -1;
    camera->stateFlags |= CAM_STATE_14;

    camera->up.y = 1.0f;
    camera->up.z = camera->up.x = 0.0f;
    camera->quakeOffset.x = camera->quakeOffset.y = camera->quakeOffset.z = 0;
    camera->atLERPStepScale = 1;
    sCameraInterfaceField = CAM_INTERFACE_FIELD(CAM_LETTERBOX_IGNORE, CAM_HUD_VISIBILITY_IGNORE, 0);
    sDbgModeIdx = -1;
    D_8011D3F0 = 3;
    osSyncPrintf(VT_FGCOL(BLUE) "camera: initialize --- " VT_RST " UID %d\n", camera->uid);
}

void func_80057FC4(Camera* camera) {
    if (camera != &camera->play->mainCamera) {
        camera->prevSetting = camera->setting = CAM_SET_FREE0;
        camera->stateFlags &= ~CAM_STATE_2;
    } else if (camera->play->roomCtx.curRoom.roomShape->base.type != ROOM_SHAPE_TYPE_IMAGE) {
        switch (camera->play->roomCtx.curRoom.behaviorType1) {
            case ROOM_BEHAVIOR_TYPE1_1:
                Camera_ChangeDoorCam(camera, NULL, -99, 0, 0, 18, 10);
                camera->prevSetting = camera->setting = CAM_SET_DUNGEON0;
                break;
            case ROOM_BEHAVIOR_TYPE1_0:
                osSyncPrintf("camera: room type: default set field\n");
                Camera_ChangeDoorCam(camera, NULL, -99, 0, 0, 18, 10);
                camera->prevSetting = camera->setting = CAM_SET_NORMAL0;
                break;
            default:
                osSyncPrintf("camera: room type: default set etc (%d)\n", camera->play->roomCtx.curRoom.behaviorType1);
                Camera_ChangeDoorCam(camera, NULL, -99, 0, 0, 18, 10);
                camera->prevSetting = camera->setting = CAM_SET_NORMAL0;
                camera->stateFlags |= CAM_STATE_2;
                break;
        }
    } else {
        osSyncPrintf("camera: room type: prerender\n");
        camera->prevSetting = camera->setting = CAM_SET_FREE0;
        camera->stateFlags &= ~CAM_STATE_2;
    }
}

void Camera_Stub80058140(Camera* camera) {
}

void Camera_InitPlayerSettings(Camera* camera, Player* player) {
    PosRot playerPosShape;
    VecGeo eyeNextAtOffset;
    s32 bgId;
    Vec3f floorPos;
    s32 upXZ;
    f32 playerYOffset;
    Vec3f* eye = &camera->eye;
    Vec3f* at = &camera->at;
    Vec3f* eyeNext = &camera->eyeNext;

    Actor_GetWorldPosShapeRot(&playerPosShape, &player->actor);
    playerYOffset = Player_GetHeight(player);
    camera->player = player;
    camera->playerPosRot = playerPosShape;
    camera->dist = eyeNextAtOffset.r = 180.0f;
    camera->inputDir.y = playerPosShape.rot.y;
    eyeNextAtOffset.yaw = camera->inputDir.y - 0x7FFF;
    camera->inputDir.x = eyeNextAtOffset.pitch = 0x71C;
    camera->inputDir.z = 0;
    camera->camDir = camera->inputDir;
    camera->xzSpeed = 0.0f;
    camera->playerPosDelta.y = 0.0f;
    camera->at = playerPosShape.pos;
    camera->at.y += playerYOffset;

    camera->posOffset.x = 0;
    camera->posOffset.y = playerYOffset;
    camera->posOffset.z = 0;

    Camera_AddVecGeoToVec3f(eyeNext, at, &eyeNextAtOffset);
    *eye = *eyeNext;
    camera->roll = 0;

    upXZ = 0;
    camera->up.z = upXZ;
    camera->up.y = 1.0f;
    camera->up.x = upXZ;

    if (Camera_GetFloorYNorm(camera, &floorPos, at, &bgId) != BGCHECK_Y_MIN) {
        camera->bgId = bgId;
    }

    camera->bgCamIndexBeforeUnderwater = -1;
    camera->waterCamSetting = -1;
    camera->stateFlags |= CAM_STATE_2;

    if (camera == &camera->play->mainCamera) {
        sCameraInterfaceField =
            CAM_INTERFACE_FIELD(CAM_LETTERBOX_LARGE | CAM_LETTERBOX_INSTANT, CAM_HUD_VISIBILITY_NOTHING_ALT, 0);
    } else {
        sCameraInterfaceField = CAM_INTERFACE_FIELD(CAM_LETTERBOX_NONE, CAM_HUD_VISIBILITY_ALL, 0);
    }

    func_80057FC4(camera);
    camera->behaviorFlags = 0;
    camera->viewFlags = 0;
    camera->nextBgCamIndex = -1;
    camera->atLERPStepScale = 1.0f;
    Camera_CopyDataToRegs(camera, camera->mode);
    Camera_QRegInit();
    osSyncPrintf(VT_FGCOL(BLUE) "camera: personalize ---" VT_RST "\n");

    if (camera->camId == CAM_ID_MAIN) {
        Camera_UpdateWater(camera);
    }
}

s16 Camera_ChangeStatus(Camera* camera, s16 status) {
    CameraModeValue* values;
    CameraModeValue* valueP;
    s32 i;

    if (PREG(82)) {
        osSyncPrintf("camera: change camera status: cond %c%c\n", status == CAM_STAT_ACTIVE ? 'o' : 'x',
                     camera->status != CAM_STAT_ACTIVE ? 'o' : 'x');
    }

    if (PREG(82)) {
        osSyncPrintf("camera: res: stat (%d/%d/%d)\n", camera->camId, camera->setting, camera->mode);
    }

    if (status == CAM_STAT_ACTIVE && camera->status != CAM_STAT_ACTIVE) {
        values = sCameraSettings[camera->setting].cameraModes[camera->mode].values;
        for (i = 0; i < sCameraSettings[camera->setting].cameraModes[camera->mode].valueCnt; i++) {
            valueP = &values[i];
            R_CAM_DATA(valueP->dataType) = valueP->val;
            if (PREG(82)) {
                osSyncPrintf("camera: change camera status: PREG(%02d) = %d\n", valueP->dataType, valueP->val);
            }
        }
    }
    camera->status = status;
    return camera->status;
}

void Camera_PrintSettings(Camera* camera) {
    char sp58[8];
    char sp50[8];
    char sp48[8];
    s32 i;

    if ((OREG(0) & 1) && (camera->play->activeCamId == camera->camId) && !gDbgCamEnabled) {
        for (i = 0; i < NUM_CAMS; i++) {
            if (camera->play->cameraPtrs[i] == NULL) {
                sp58[i] = '-';
                sp48[i] = ' ';
            } else {
                switch (camera->play->cameraPtrs[i]->status) {
                    case 0:
                        sp58[i] = 'c';
                        break;
                    case 1:
                        sp58[i] = 'w';
                        break;
                    case 3:
                        sp58[i] = 's';
                        break;
                    case 7:
                        sp58[i] = 'a';
                        break;
                    case 0x100:
                        sp58[i] = 'd';
                        break;
                    default:
                        sp58[i] = '*';
                        break;
                }
            }
            sp48[i] = ' ';
        }
        sp58[i] = '\0';
        sp48[i] = '\0';

        sp48[camera->play->activeCamId] = 'a';
        func_8006376C(3, 0x16, 5, sp58);
        func_8006376C(3, 0x16, 1, sp48);
        func_8006376C(3, 0x17, 5, "S:");
        func_8006376C(5, 0x17, 4, sCameraSettingNames[camera->setting]);
        func_8006376C(3, 0x18, 5, "M:");
        func_8006376C(5, 0x18, 4, sCameraModeNames[camera->mode]);
        func_8006376C(3, 0x19, 5, "F:");
        func_8006376C(5, 0x19, 4,
                      sCameraFunctionNames[sCameraSettings[camera->setting].cameraModes[camera->mode].funcIdx]);

        i = 0;
        if (camera->bgCamIndex < 0) {
            sp50[i++] = '-';
        }

        //! @bug: this code was clearly meaning to print `abs(camera->bgCamIndex)` as a
        //! one-or-two-digit number, instead of `i`.
        // "sp50[i++] = ..." matches here, but is undefined behavior due to conflicting
        // reads/writes between sequence points, triggering warnings. Work around by
        // putting i++ afterwards while on the same line.
        // clang-format off
        if (camera->bgCamIndex / 10 != 0) {
            sp50[i] = i / 10 + '0'; i++;
        }
        sp50[i] = i % 10 + '0'; i++;
        // clang-format on

        sp50[i++] = ' ';
        sp50[i++] = ' ';
        sp50[i++] = ' ';
        sp50[i++] = ' ';
        sp50[i] = '\0';
        func_8006376C(3, 26, 5, "I:");
        func_8006376C(5, 26, 4, sp50);
    }
}

s32 Camera_UpdateWater(Camera* camera) {
    f32 waterY;
    s16 quakeIndex;
    s32 waterLightsIndex;
    s32* waterCamSetting = &camera->waterCamSetting;
    s16 waterBgCamIndex;
    s16* waterQuakeIndex = (s16*)&camera->waterQuakeIndex;
    Player* player = camera->player;
    s16 prevBgId;

    if (!(camera->stateFlags & CAM_STATE_1) || sCameraSettings[camera->setting].unk_00 & 0x40000000) {
        return 0;
    }

    if (camera->stateFlags & CAM_STATE_9) {
        if (player->stateFlags2 & PLAYER_STATE2_11) {
            Camera_ChangeSettingFlags(camera, CAM_SET_PIVOT_WATER_SURFACE, 6);
            camera->stateFlags |= CAM_STATE_15;
        } else if (camera->stateFlags & CAM_STATE_15) {
            Camera_ChangeSettingFlags(camera, *waterCamSetting, 6);
            camera->stateFlags &= ~CAM_STATE_15;
        }
    }
    if (!(camera->stateFlags & CAM_STATE_15)) {
        if (waterBgCamIndex = Camera_GetWaterBoxBgCamIndex(camera, &waterY), waterBgCamIndex == -2) {
            // No camera data index
            if (!(camera->stateFlags & CAM_STATE_9)) {
                camera->stateFlags |= CAM_STATE_9;
                camera->waterYPos = waterY;
                camera->bgCamIndexBeforeUnderwater = camera->bgCamIndex;
                *waterQuakeIndex = -1;
            }
            if (camera->playerGroundY != camera->playerPosRot.pos.y) {
                prevBgId = camera->bgId;
                camera->bgId = BGCHECK_SCENE;
                Camera_ChangeSettingFlags(camera, CAM_SET_NORMAL3, 2);
                *waterCamSetting = camera->setting;
                camera->bgId = prevBgId;
                camera->bgCamIndex = -2;
            }
        } else if (waterBgCamIndex != -1) {
            // player is in a water box
            if (!(camera->stateFlags & CAM_STATE_9)) {
                camera->stateFlags |= CAM_STATE_9;
                camera->waterYPos = waterY;
                camera->bgCamIndexBeforeUnderwater = camera->bgCamIndex;
                *waterQuakeIndex = -1;
            }
            if (camera->playerGroundY != camera->playerPosRot.pos.y) {
                prevBgId = camera->bgId;
                camera->bgId = BGCHECK_SCENE;
                Camera_ChangeBgCamIndex(camera, waterBgCamIndex);
                *waterCamSetting = camera->setting;
                camera->bgId = prevBgId;
            }
        } else if (camera->stateFlags & CAM_STATE_9) {
            // player is out of a water box.
            osSyncPrintf("camera: water: off\n");
            camera->stateFlags &= ~CAM_STATE_9;
            prevBgId = camera->bgId;
            camera->bgId = BGCHECK_SCENE;
            if (camera->bgCamIndexBeforeUnderwater < 0) {
                func_80057FC4(camera);
                camera->bgCamIndex = -1;
            } else {
                Camera_ChangeBgCamIndex(camera, camera->bgCamIndexBeforeUnderwater);
            }
            camera->bgId = prevBgId;
        }
    }

    if (waterY = Camera_GetWaterSurface(camera, &camera->eye, &waterLightsIndex), waterY != BGCHECK_Y_MIN) {
        camera->waterYPos = waterY;
        if (!(camera->stateFlags & CAM_STATE_8)) {
            camera->stateFlags |= CAM_STATE_8;
            osSyncPrintf("kankyo changed water, sound on\n");
            Environment_EnableUnderwaterLights(camera->play, waterLightsIndex);
            camera->waterDistortionTimer = 80;
        }

        Audio_SetExtraFilter(0x20);

        if (PREG(81)) {
            Quake_RemoveRequest(*waterQuakeIndex);
            *waterQuakeIndex = -1;
            PREG(81) = 0;
        }

        if ((*waterQuakeIndex == -1) || (Quake_GetTimeLeft(*waterQuakeIndex) == 10)) {
            quakeIndex = Quake_Request(camera, QUAKE_TYPE_5);

            *waterQuakeIndex = quakeIndex;
            if (quakeIndex != 0) {
                Quake_SetSpeed(*waterQuakeIndex, 550);
                Quake_SetPerturbations(*waterQuakeIndex, 1, 1, 180, 0);
                Quake_SetDuration(*waterQuakeIndex, 1000);
            }
        }

        if (camera->waterDistortionTimer > 0) {
            camera->waterDistortionTimer--;
            camera->distortionFlags |= DISTORTION_UNDERWATER_STRONG;
        } else if (camera->play->sceneId == SCENE_FISHING_POND) {
            camera->distortionFlags |= DISTORTION_UNDERWATER_FISHING;
        } else {
            camera->distortionFlags |= DISTORTION_UNDERWATER_WEAK;
        }
    } else {
        if (camera->stateFlags & CAM_STATE_8) {
            camera->stateFlags &= ~CAM_STATE_8;
            osSyncPrintf("kankyo changed water off, sound off\n");
            Environment_DisableUnderwaterLights(camera->play);
            if (*waterQuakeIndex != 0) {
                Quake_RemoveRequest(*waterQuakeIndex);
            }
            camera->waterDistortionTimer = 0;
            camera->distortionFlags = 0;
        }
        Audio_SetExtraFilter(0);
    }
    //! @bug: doesn't always return a value, but sometimes does.
}

s32 Camera_UpdateHotRoom(Camera* camera) {
    camera->distortionFlags &= ~DISTORTION_HOT_ROOM;
    if (camera->play->roomCtx.curRoom.behaviorType2 == ROOM_BEHAVIOR_TYPE2_3) {
        camera->distortionFlags |= DISTORTION_HOT_ROOM;
    }

    return 1;
}

s32 Camera_DbgChangeMode(Camera* camera) {
    s32 changeDir = 0;

    if (!gDbgCamEnabled && camera->play->activeCamId == CAM_ID_MAIN) {
        if (CHECK_BTN_ALL(D_8015BD7C->state.input[2].press.button, BTN_CUP)) {
            osSyncPrintf("attention sound URGENCY\n");
            func_80078884(NA_SE_SY_ATTENTION_URGENCY);
        }
        if (CHECK_BTN_ALL(D_8015BD7C->state.input[2].press.button, BTN_CDOWN)) {
            osSyncPrintf("attention sound NORMAL\n");
            func_80078884(NA_SE_SY_ATTENTION_ON);
        }

        if (CHECK_BTN_ALL(D_8015BD7C->state.input[2].press.button, BTN_CRIGHT)) {
            changeDir = 1;
        }
        if (CHECK_BTN_ALL(D_8015BD7C->state.input[2].press.button, BTN_CLEFT)) {
            changeDir = -1;
        }
        if (changeDir != 0) {
            sDbgModeIdx = (sDbgModeIdx + changeDir) % 6;
            if (Camera_ChangeSetting(camera, D_8011DAFC[sDbgModeIdx]) > 0) {
                osSyncPrintf("camera: force change SET to %s!\n", sCameraSettingNames[D_8011DAFC[sDbgModeIdx]]);
            }
        }
    }
    return true;
}

void Camera_UpdateDistortion(Camera* camera) {
    static s16 depthPhase = 0x3F0;
    static s16 screenPlanePhase = 0x156;
    f32 scaleFactor;
    f32 speedFactor;
    f32 depthPhaseStep;
    f32 screenPlanePhaseStep;
    s32 pad[5];
    f32 xScale;
    f32 yScale;
    f32 zScale;
    f32 speed;

    if (camera->distortionFlags != 0) {
        if (camera->distortionFlags & DISTORTION_UNDERWATER_MEDIUM) {
            depthPhaseStep = 0.0f;
            screenPlanePhaseStep = 170.0f;

            xScale = -0.01f;
            yScale = 0.01f;
            zScale = 0.0f;

            speed = 0.6f;
            scaleFactor = camera->waterDistortionTimer / 60.0f;
            speedFactor = 1.0f;
        } else if (camera->distortionFlags & DISTORTION_UNDERWATER_STRONG) {
            depthPhaseStep = 248.0f;
            screenPlanePhaseStep = -90.0f;

            xScale = -0.3f;
            yScale = 0.3f;
            zScale = 0.2f;

            speed = 0.2f;
            scaleFactor = camera->waterDistortionTimer / 80.0f;
            speedFactor = 1.0f;
        } else if (camera->distortionFlags & DISTORTION_UNDERWATER_WEAK) {
            depthPhaseStep = 359.2f;
            screenPlanePhaseStep = -18.5f;

            xScale = 0.09f;
            yScale = 0.09f;
            zScale = 0.01f;

            speed = 0.08f;
            scaleFactor =
                (((camera->waterYPos - camera->eye.y) > 150.0f ? 1.0f : (camera->waterYPos - camera->eye.y) / 150.0f) *
                 0.45f) +
                (camera->speedRatio * 0.45f);
            speedFactor = scaleFactor;
        } else if (camera->distortionFlags & DISTORTION_HOT_ROOM) {
            // Gives the hot-room a small mirage-like appearance
            depthPhaseStep = 0.0f;
            screenPlanePhaseStep = 150.0f;

            xScale = -0.01f;
            yScale = 0.01f;
            zScale = 0.01f;

            speed = 0.6f;
            speedFactor = 1.0f;
            scaleFactor = 1.0f;
        } else {
            // DISTORTION_UNDERWATER_FISHING
            return;
        }

        depthPhase += CAM_DEG_TO_BINANG(depthPhaseStep);
        screenPlanePhase += CAM_DEG_TO_BINANG(screenPlanePhaseStep);

        View_SetDistortionOrientation(&camera->play->view, Math_CosS(depthPhase) * 0.0f, Math_SinS(depthPhase) * 0.0f,
                                      Math_SinS(screenPlanePhase) * 0.0f);
        View_SetDistortionScale(&camera->play->view, Math_SinS(screenPlanePhase) * (xScale * scaleFactor) + 1.0f,
                                Math_CosS(screenPlanePhase) * (yScale * scaleFactor) + 1.0f,
                                Math_CosS(depthPhase) * (zScale * scaleFactor) + 1.0f);
        View_SetDistortionSpeed(&camera->play->view, speed * speedFactor);

        camera->stateFlags |= CAM_STATE_6;

    } else if (camera->stateFlags & CAM_STATE_6) {
        View_ClearDistortion(&camera->play->view);
        camera->stateFlags &= ~CAM_STATE_6;
    }
}

Vec3s Camera_Update(Camera* camera) {
    static s32 sOOBTimer = 0;
    Vec3f viewAt;
    Vec3f viewEye;
    Vec3f viewUp;
    f32 viewFov;
    Vec3f pos;
    s32 bgId;
    f32 playerGroundY;
    f32 playerXZSpeed;
    VecGeo eyeAtAngle;
    s16 bgCamIndex;
    s16 numQuakesApplied;
    PosRot curPlayerPosRot;
    ShakeInfo camShake;
    Player* player;

    player = camera->play->cameraPtrs[CAM_ID_MAIN]->player;

    if (R_DBG_CAM_UPDATE) {
        osSyncPrintf("camera: in %x\n", camera);
    }

    if (camera->status == CAM_STAT_CUT) {
        if (R_DBG_CAM_UPDATE) {
            osSyncPrintf("camera: cut out %x\n", camera);
        }
        return camera->inputDir;
    }

    sUpdateCameraDirection = false;

    if (camera->player != NULL) {
        Actor_GetWorldPosShapeRot(&curPlayerPosRot, &camera->player->actor);
        camera->xzSpeed = playerXZSpeed = OLib_Vec3fDistXZ(&curPlayerPosRot.pos, &camera->playerPosRot.pos);

        camera->speedRatio =
            OLib_ClampMaxDist(playerXZSpeed / (func_8002DCE4(camera->player) * CAM_DATA_SCALED(OREG(8))), 1.0f);
        camera->playerPosDelta.x = curPlayerPosRot.pos.x - camera->playerPosRot.pos.x;
        camera->playerPosDelta.y = curPlayerPosRot.pos.y - camera->playerPosRot.pos.y;
        camera->playerPosDelta.z = curPlayerPosRot.pos.z - camera->playerPosRot.pos.z;
        pos = curPlayerPosRot.pos;
        pos.y += Player_GetHeight(camera->player);

        playerGroundY = BgCheck_EntityRaycastDown5(camera->play, &camera->play->colCtx, &playerFloorPoly, &bgId,
                                                   &camera->player->actor, &pos);
        if (playerGroundY != BGCHECK_Y_MIN) {
            // player is above ground.
            sOOBTimer = 0;
            camera->floorNorm.x = COLPOLY_GET_NORMAL(playerFloorPoly->normal.x);
            camera->floorNorm.y = COLPOLY_GET_NORMAL(playerFloorPoly->normal.y);
            camera->floorNorm.z = COLPOLY_GET_NORMAL(playerFloorPoly->normal.z);
            camera->bgId = bgId;
            camera->playerGroundY = playerGroundY;
        } else {
            // player is not above ground.
            sOOBTimer++;
            camera->floorNorm.x = 0.0;
            camera->floorNorm.y = 1.0f;
            camera->floorNorm.z = 0.0;
        }

        camera->playerPosRot = curPlayerPosRot;

        if (sOOBTimer < 200) {
            if (camera->status == CAM_STAT_ACTIVE) {
                Camera_UpdateWater(camera);
                Camera_UpdateHotRoom(camera);
            }

            if (!(camera->stateFlags & CAM_STATE_2)) {
                camera->nextBgCamIndex = -1;
            }

            if ((camera->stateFlags & CAM_STATE_0) && (camera->stateFlags & CAM_STATE_2) &&
                !(camera->stateFlags & CAM_STATE_10) &&
                (!(camera->stateFlags & CAM_STATE_9) || (player->currentBoots == PLAYER_BOOTS_IRON)) &&
                !(camera->stateFlags & CAM_STATE_15) && (playerGroundY != BGCHECK_Y_MIN)) {
                bgCamIndex = Camera_GetBgCamIndex(camera, &bgId, playerFloorPoly);
                if (bgCamIndex != -1) {
                    camera->nextBgId = bgId;
                    if (bgId == BGCHECK_SCENE) {
                        camera->nextBgCamIndex = bgCamIndex;
                    }
                }
            }

            if (camera->nextBgCamIndex != -1 && (fabsf(curPlayerPosRot.pos.y - playerGroundY) < 2.0f) &&
                (!(camera->stateFlags & CAM_STATE_9) || (player->currentBoots == PLAYER_BOOTS_IRON))) {
                camera->bgId = camera->nextBgId;
                Camera_ChangeBgCamIndex(camera, camera->nextBgCamIndex);
                camera->nextBgCamIndex = -1;
            }
        }
    }
    Camera_PrintSettings(camera);
    Camera_DbgChangeMode(camera);

    if (camera->status == CAM_STAT_WAIT) {
        if (R_DBG_CAM_UPDATE) {
            osSyncPrintf("camera: wait out %x\n", camera);
        }
        return camera->inputDir;
    }

    camera->behaviorFlags = 0;
    camera->stateFlags &= ~(CAM_STATE_10 | CAM_STATE_5);
    camera->stateFlags |= CAM_STATE_4;

    if (R_DBG_CAM_UPDATE) {
        osSyncPrintf("camera: engine (%d %d %d) %04x \n", camera->setting, camera->mode,
                     sCameraSettings[camera->setting].cameraModes[camera->mode].funcIdx, camera->stateFlags);
    }

    if (sOOBTimer < 200) {
        sCameraFunctions[sCameraSettings[camera->setting].cameraModes[camera->mode].funcIdx](camera);
    } else if (camera->player != NULL) {
        OLib_Vec3fDiffToVecGeo(&eyeAtAngle, &camera->at, &camera->eye);
        Camera_CalcAtDefault(camera, &eyeAtAngle, 0.0f, 0);
    }

    if (camera->status == CAM_STAT_ACTIVE) {
        if ((gSaveContext.gameMode != GAMEMODE_NORMAL) && (gSaveContext.gameMode != GAMEMODE_END_CREDITS)) {
            sCameraInterfaceField = CAM_INTERFACE_FIELD(CAM_LETTERBOX_NONE, CAM_HUD_VISIBILITY_ALL, 0);
            Camera_UpdateInterface(sCameraInterfaceField);
        } else if ((D_8011D3F0 != 0) && (camera->camId == CAM_ID_MAIN)) {
            D_8011D3F0--;
            sCameraInterfaceField = CAM_INTERFACE_FIELD(CAM_LETTERBOX_LARGE, CAM_HUD_VISIBILITY_NOTHING_ALT, 0);
            Camera_UpdateInterface(sCameraInterfaceField);
        } else if (camera->play->transitionMode != TRANS_MODE_OFF) {
            sCameraInterfaceField = CAM_INTERFACE_FIELD(CAM_LETTERBOX_IGNORE, CAM_HUD_VISIBILITY_NOTHING_ALT, 0);
            Camera_UpdateInterface(sCameraInterfaceField);
        } else if (camera->play->csCtx.state != CS_STATE_IDLE) {
            sCameraInterfaceField = CAM_INTERFACE_FIELD(CAM_LETTERBOX_LARGE, CAM_HUD_VISIBILITY_NOTHING_ALT, 0);
            Camera_UpdateInterface(sCameraInterfaceField);
        } else {
            Camera_UpdateInterface(sCameraInterfaceField);
        }
    }

    if (R_DBG_CAM_UPDATE) {
        osSyncPrintf("camera: shrink_and_bitem %x(%d)\n", sCameraInterfaceField, camera->play->transitionMode);
    }

    if (R_DBG_CAM_UPDATE) {
        osSyncPrintf("camera: engine (%s(%d) %s(%d) %s(%d)) ok!\n", &sCameraSettingNames[camera->setting],
                     camera->setting, &sCameraModeNames[camera->mode], camera->mode,
                     &sCameraFunctionNames[sCameraSettings[camera->setting].cameraModes[camera->mode].funcIdx],
                     sCameraSettings[camera->setting].cameraModes[camera->mode].funcIdx);
    }

    // enable/disable debug cam
    if (CHECK_BTN_ALL(D_8015BD7C->state.input[2].press.button, BTN_START)) {
        gDbgCamEnabled ^= 1;
        if (gDbgCamEnabled) {
            DbgCamera_Enable(&D_8015BD80, camera);
        } else if (camera->play->csCtx.state != CS_STATE_IDLE) {
            func_80064534(camera->play, &camera->play->csCtx);
        }
    }

    // Debug cam update
    if (gDbgCamEnabled) {
        camera->play->view.fovy = D_8015BD80.fov;
        DbCamera_Update(&D_8015BD80, camera);
        View_LookAt(&camera->play->view, &D_8015BD80.eye, &D_8015BD80.at, &D_8015BD80.unk_1C);
        if (R_DBG_CAM_UPDATE) {
            osSyncPrintf("camera: debug out\n");
        }
        return D_8015BD80.sub.unk_104A;
    }

    OREG(0) &= ~8;

    if (camera->status == CAM_STAT_UNK3) {
        return camera->inputDir;
    }

    numQuakesApplied = Quake_Update(camera, &camShake);

    bgId = numQuakesApplied; // required to match

    if ((numQuakesApplied != 0) && (camera->setting != CAM_SET_TURN_AROUND)) {
        viewAt.x = camera->at.x + camShake.atOffset.x;
        viewAt.y = camera->at.y + camShake.atOffset.y;
        viewAt.z = camera->at.z + camShake.atOffset.z;

        viewEye.x = camera->eye.x + camShake.eyeOffset.x;
        viewEye.y = camera->eye.y + camShake.eyeOffset.y;
        viewEye.z = camera->eye.z + camShake.eyeOffset.z;

        OLib_Vec3fDiffToVecGeo(&eyeAtAngle, &viewEye, &viewAt);
        Camera_CalcUpFromPitchYawRoll(&viewUp, eyeAtAngle.pitch + camShake.upPitchOffset,
                                      eyeAtAngle.yaw + camShake.upYawOffset, camera->roll);
        viewFov = camera->fov + CAM_BINANG_TO_DEG(camShake.fovOffset);
    } else {
        viewAt = camera->at;
        viewEye = camera->eye;
        OLib_Vec3fDiffToVecGeo(&eyeAtAngle, &viewEye, &viewAt);
        Camera_CalcUpFromPitchYawRoll(&viewUp, eyeAtAngle.pitch, eyeAtAngle.yaw, camera->roll);
        viewFov = camera->fov;
    }

    if (camera->viewFlags & CAM_VIEW_UP) {
        camera->viewFlags &= ~CAM_VIEW_UP;
        viewUp = camera->up;
    } else {
        camera->up = viewUp;
    }

    camera->quakeOffset = camShake.eyeOffset;

    Camera_UpdateDistortion(camera);

    if ((camera->play->sceneId == SCENE_HYRULE_FIELD) && (camera->fov < 59.0f)) {
        View_SetScale(&camera->play->view, 0.79f);
    } else {
        View_SetScale(&camera->play->view, 1.0f);
    }
    camera->play->view.fovy = viewFov;
    View_LookAt(&camera->play->view, &viewEye, &viewAt, &viewUp);
    camera->camDir.x = eyeAtAngle.pitch;
    camera->camDir.y = eyeAtAngle.yaw;
    camera->camDir.z = 0;

    if (sUpdateCameraDirection == 0) {
        camera->inputDir.x = eyeAtAngle.pitch;
        camera->inputDir.y = eyeAtAngle.yaw;
        camera->inputDir.z = 0;
    }

    if (PREG(81)) {
        osSyncPrintf("dir  (%d) %d(%f) %d(%f) 0(0) \n", sUpdateCameraDirection, camera->inputDir.x,
                     CAM_BINANG_TO_DEG(camera->inputDir.x), camera->inputDir.y, CAM_BINANG_TO_DEG(camera->inputDir.y));
        osSyncPrintf("real (%d) %d(%f) %d(%f) 0(0) \n", sUpdateCameraDirection, camera->camDir.x,
                     CAM_BINANG_TO_DEG(camera->camDir.x), camera->camDir.y, CAM_BINANG_TO_DEG(camera->camDir.y));
    }

    if (camera->timer != -1 && CHECK_BTN_ALL(D_8015BD7C->state.input[0].press.button, BTN_DRIGHT)) {
        camera->timer = 0;
    }

    if (R_DBG_CAM_UPDATE) {
        osSyncPrintf("camera: out (%f %f %f) (%f %f %f)\n", camera->at.x, camera->at.y, camera->at.z, camera->eye.x,
                     camera->eye.y, camera->eye.z);
        osSyncPrintf("camera: dir (%f %d(%f) %d(%f)) (%f)\n", eyeAtAngle.r, eyeAtAngle.pitch,
                     CAM_BINANG_TO_DEG(eyeAtAngle.pitch), eyeAtAngle.yaw, CAM_BINANG_TO_DEG(eyeAtAngle.yaw),
                     camera->fov);
        if (camera->player != NULL) {
            osSyncPrintf("camera: foot(%f %f %f) dist (%f)\n", curPlayerPosRot.pos.x, curPlayerPosRot.pos.y,
                         curPlayerPosRot.pos.z, camera->dist);
        }
    }

    return camera->inputDir;
}

/**
 * When the camera's timer is 0, change the camera to its parent
 */
void Camera_Finish(Camera* camera) {
    Camera* mainCam = camera->play->cameraPtrs[CAM_ID_MAIN];
    Player* player = GET_PLAYER(camera->play);

    if (camera->timer == 0) {
        Play_ChangeCameraStatus(camera->play, camera->parentCamId, CAM_STAT_ACTIVE);

        if ((camera->parentCamId == CAM_ID_MAIN) && (camera->csId != 0)) {
            player->actor.freezeTimer = 0;
            player->stateFlags1 &= ~PLAYER_STATE1_29;

            if (player->csMode != 0) {
                func_8002DF54(camera->play, &player->actor, 7);
                osSyncPrintf("camera: player demo end!!\n");
            }

            mainCam->stateFlags |= CAM_STATE_3;
        }

        if (CHILD_CAM(camera)->parentCamId == camera->camId) {
            CHILD_CAM(camera)->parentCamId = camera->parentCamId;
        }

        if (PARENT_CAM(camera)->childCamId == camera->camId) {
            PARENT_CAM(camera)->childCamId = camera->childCamId;
        }

        if (PARENT_CAM(camera)->camId == CAM_ID_MAIN) {
            PARENT_CAM(camera)->animState = 0;
        }

        camera->childCamId = camera->parentCamId = CAM_ID_MAIN;
        camera->timer = -1;
        camera->play->envCtx.fillScreen = false;

        Play_ClearCamera(camera->play, camera->camId);
    }
}

s32 func_8005A02C(Camera* camera) {
    camera->stateFlags |= (CAM_STATE_2 | CAM_STATE_3);
    camera->stateFlags &= ~(CAM_STATE_3 | CAM_STATE_12);
    return true;
}

s32 Camera_ChangeModeFlags(Camera* camera, s16 mode, u8 flags) {
    static s32 modeChangeFlags = 0;

    if (QREG(89)) {
        osSyncPrintf("+=+(%d)+=+ recive request -> %s\n", camera->play->state.frames, sCameraModeNames[mode]);
    }

    if ((camera->stateFlags & CAM_STATE_5) && (flags == 0)) {
        camera->behaviorFlags |= CAM_BEHAVIOR_MODE_2;
        return -1;
    }

    if (!((sCameraSettings[camera->setting].unk_00 & 0x3FFFFFFF) & (1 << mode))) {
        if (mode == CAM_MODE_FIRST_PERSON) {
            osSyncPrintf("camera: error sound\n");
            func_80078884(NA_SE_SY_ERROR);
        }

        if (camera->mode != CAM_MODE_NORMAL) {
            osSyncPrintf(VT_COL(YELLOW, BLACK) "camera: change camera mode: force NORMAL: %s %s refused\n" VT_RST,
                         sCameraSettingNames[camera->setting], sCameraModeNames[mode]);
            camera->mode = CAM_MODE_NORMAL;
            Camera_CopyDataToRegs(camera, camera->mode);
            func_8005A02C(camera);
            return 0xC0000000 | mode;
        } else {
            camera->behaviorFlags |= CAM_BEHAVIOR_MODE_2;
            camera->behaviorFlags |= CAM_BEHAVIOR_MODE_1;
            return 0;
        }
    } else {
        if (mode == camera->mode && flags == 0) {
            camera->behaviorFlags |= CAM_BEHAVIOR_MODE_2;
            camera->behaviorFlags |= CAM_BEHAVIOR_MODE_1;
            return -1;
        }
        camera->behaviorFlags |= CAM_BEHAVIOR_MODE_2;
        camera->behaviorFlags |= CAM_BEHAVIOR_MODE_1;
        Camera_CopyDataToRegs(camera, mode);
        modeChangeFlags = 0;
        switch (mode) {
            case CAM_MODE_FIRST_PERSON:
                modeChangeFlags = 0x20;
                break;
            case CAM_MODE_Z_TARGET_UNFRIENDLY:
                modeChangeFlags = 4;
                break;
            case CAM_MODE_Z_TARGET_FRIENDLY:
                if (camera->target != NULL && camera->target->id != ACTOR_EN_BOOM) {
                    modeChangeFlags = 8;
                }
                break;
            case CAM_MODE_Z_PARALLEL:
            case CAM_MODE_TALK:
            case CAM_MODE_Z_AIM:
            case CAM_MODE_Z_LEDGE_HANG:
            case CAM_MODE_PUSH_PULL:
                modeChangeFlags = 2;
                break;
        }

        switch (camera->mode) {
            case CAM_MODE_FIRST_PERSON:
                if (modeChangeFlags & 0x20) {
                    camera->animState = 10;
                }
                break;
            case CAM_MODE_Z_PARALLEL:
                if (modeChangeFlags & 0x10) {
                    camera->animState = 10;
                }
                modeChangeFlags |= 1;
                break;
            case CAM_MODE_CHARGE:
                modeChangeFlags |= 1;
                break;
            case CAM_MODE_Z_TARGET_FRIENDLY:
                if (modeChangeFlags & 8) {
                    camera->animState = 10;
                }
                modeChangeFlags |= 1;
                break;
            case CAM_MODE_Z_TARGET_UNFRIENDLY:
                if (modeChangeFlags & 4) {
                    camera->animState = 10;
                }
                modeChangeFlags |= 1;
                break;
            case CAM_MODE_Z_AIM:
            case CAM_MODE_Z_LEDGE_HANG:
            case CAM_MODE_PUSH_PULL:
                modeChangeFlags |= 1;
                break;
            case CAM_MODE_NORMAL:
                if (modeChangeFlags & 0x10) {
                    camera->animState = 10;
                }
                break;
        }
        modeChangeFlags &= ~0x10;
        if (camera->status == CAM_STAT_ACTIVE) {
            switch (modeChangeFlags) {
                case 1:
                    func_80078884(0);
                    break;
                case 2:
                    if (camera->play->roomCtx.curRoom.behaviorType1 == ROOM_BEHAVIOR_TYPE1_1) {
                        func_80078884(NA_SE_SY_ATTENTION_URGENCY);
                    } else {
                        func_80078884(NA_SE_SY_ATTENTION_ON);
                    }
                    break;
                case 4:
                    func_80078884(NA_SE_SY_ATTENTION_URGENCY);
                    break;
                case 8:
                    func_80078884(NA_SE_SY_ATTENTION_ON);
                    break;
            }
        }
        func_8005A02C(camera);
        camera->mode = mode;
        return 0x80000000 | mode;
    }
}

s32 Camera_ChangeMode(Camera* camera, s16 mode) {
    return Camera_ChangeModeFlags(camera, mode, 0);
}

s32 Camera_CheckValidMode(Camera* camera, s16 mode) {
    if (QREG(89) != 0) {
        osSyncPrintf("+=+=+=+ recive asking -> %s (%s)\n", sCameraModeNames[mode],
                     sCameraSettingNames[camera->setting]);
    }
    if (!(sCameraSettings[camera->setting].validModes & (1 << mode))) {
        return 0;
    } else if (mode == camera->mode) {
        return -1;
    } else {
        return mode | 0x80000000;
    }
}

s16 Camera_ChangeSettingFlags(Camera* camera, s16 setting, s16 flags) {
    if (camera->behaviorFlags & CAM_BEHAVIOR_SETTING_1) {
        if ((u32)((u32)(sCameraSettings[camera->setting].unk_00 & 0xF000000) >> 0x18) >=
            (u32)((u32)(sCameraSettings[setting].unk_00 & 0xF000000) >> 0x18)) {
            camera->behaviorFlags |= CAM_BEHAVIOR_SETTING_2;
            return -2;
        }
    }
    if (((setting == CAM_SET_MEADOW_BIRDS_EYE) || (setting == CAM_SET_MEADOW_UNUSED)) && LINK_IS_ADULT &&
        (camera->play->sceneId == SCENE_SACRED_FOREST_MEADOW)) {
        camera->behaviorFlags |= CAM_BEHAVIOR_SETTING_2;
        return -5;
    }

    if (setting == CAM_SET_NONE || setting >= CAM_SET_MAX) {
        osSyncPrintf(VT_COL(RED, WHITE) "camera: error: illegal camera set (%d) !!!!\n" VT_RST, setting);
        return -99;
    }

    if ((setting == camera->setting) && !(flags & 1)) {
        camera->behaviorFlags |= CAM_BEHAVIOR_SETTING_2;
        if (!(flags & 2)) {
            camera->behaviorFlags |= CAM_BEHAVIOR_SETTING_1;
        }
        return -1;
    }

    camera->behaviorFlags |= CAM_BEHAVIOR_SETTING_2;
    if (!(flags & 2)) {
        camera->behaviorFlags |= CAM_BEHAVIOR_SETTING_1;
    }

    camera->stateFlags |= (CAM_STATE_2 | CAM_STATE_3);
    camera->stateFlags &= ~(CAM_STATE_3 | CAM_STATE_12);

    if (!(sCameraSettings[camera->setting].unk_00 & 0x40000000)) {
        camera->prevSetting = camera->setting;
    }

    if (flags & 8) {
        if (1) {}
        camera->bgCamIndex = camera->prevBgCamIndex;
        camera->prevBgCamIndex = -1;
    } else if (!(flags & 4)) {
        if (!(sCameraSettings[camera->setting].unk_00 & 0x40000000)) {
            camera->prevBgCamIndex = camera->bgCamIndex;
        }
        camera->bgCamIndex = -1;
    }

    camera->setting = setting;

    if (Camera_ChangeModeFlags(camera, camera->mode, 1) >= 0) {
        Camera_CopyDataToRegs(camera, camera->mode);
    }

    osSyncPrintf(VT_SGR("1") "%06u:" VT_RST " camera: change camera[%d] set %s\n", camera->play->state.frames,
                 camera->camId, sCameraSettingNames[camera->setting]);

    return setting;
}

s32 Camera_ChangeSetting(Camera* camera, s16 setting) {
    return Camera_ChangeSettingFlags(camera, setting, 0);
}

s32 Camera_ChangeBgCamIndex(Camera* camera, s32 bgCamIndex) {
    s16 newCameraSetting;
    s16 settingChangeSuccessful;

    if (bgCamIndex == -1 || bgCamIndex == camera->bgCamIndex) {
        camera->behaviorFlags |= CAM_BEHAVIOR_BG_2;
        return -1;
    }

    if (!(camera->behaviorFlags & CAM_BEHAVIOR_BG_2)) {
        newCameraSetting = Camera_GetBgCamSetting(camera, bgCamIndex);
        camera->behaviorFlags |= CAM_BEHAVIOR_BG_2;
        settingChangeSuccessful = Camera_ChangeSettingFlags(camera, newCameraSetting, 5) >= 0;
        if (settingChangeSuccessful || sCameraSettings[camera->setting].unk_00 & 0x80000000) {
            camera->bgCamIndex = bgCamIndex;
            camera->behaviorFlags |= CAM_BEHAVIOR_BG_1;
            Camera_CopyDataToRegs(camera, camera->mode);
        } else if (settingChangeSuccessful < -1) {
            //! @bug: This is likely checking the wrong value. The actual return of Camera_ChangeSettingFlags or
            // bgCamIndex would make more sense.
            osSyncPrintf(VT_COL(RED, WHITE) "camera: error: illegal camera ID (%d) !! (%d|%d|%d)\n" VT_RST, bgCamIndex,
                         camera->camId, BGCHECK_SCENE, newCameraSetting);
        }
        return 0x80000000 | bgCamIndex;
    }
}

Vec3s* Camera_GetInputDir(Vec3s* dst, Camera* camera) {
    if (gDbgCamEnabled) {
        *dst = D_8015BD80.sub.unk_104A;
        return dst;
    } else {
        *dst = camera->inputDir;
        return dst;
    }
}

s16 Camera_GetInputDirPitch(Camera* camera) {
    Vec3s dir;

    Camera_GetInputDir(&dir, camera);
    return dir.x;
}

s16 Camera_GetInputDirYaw(Camera* camera) {
    Vec3s dir;

    Camera_GetInputDir(&dir, camera);
    return dir.y;
}

Vec3s* Camera_GetCamDir(Vec3s* dst, Camera* camera) {
    if (gDbgCamEnabled) {
        *dst = D_8015BD80.sub.unk_104A;
        return dst;
    } else {
        *dst = camera->camDir;
        return dst;
    }
}

s16 Camera_GetCamDirPitch(Camera* camera) {
    Vec3s camDir;

    Camera_GetCamDir(&camDir, camera);
    return camDir.x;
}

s16 Camera_GetCamDirYaw(Camera* camera) {
    Vec3s camDir;

    Camera_GetCamDir(&camDir, camera);
    return camDir.y;
}

s32 Camera_RequestQuake(Camera* camera, s32 unused, s16 y, s32 duration) {
    s16 quakeIndex;

    quakeIndex = Quake_Request(camera, QUAKE_TYPE_3);
    if (quakeIndex == 0) {
        return false;
    }
    Quake_SetSpeed(quakeIndex, 0x61A8);
    Quake_SetPerturbations(quakeIndex, y, 0, 0, 0);
    Quake_SetDuration(quakeIndex, duration);
    return true;
}

s32 Camera_SetViewParam(Camera* camera, s32 viewFlag, void* param) {
    s32 pad[3];

    if (param != NULL) {
        switch (viewFlag) {
            case CAM_VIEW_AT:
                camera->viewFlags &= ~(CAM_VIEW_AT | CAM_VIEW_TARGET | CAM_VIEW_TARGET_POS);
                camera->at = *(Vec3f*)param;
                break;

            case CAM_VIEW_TARGET_POS:
                camera->viewFlags &= ~(CAM_VIEW_AT | CAM_VIEW_TARGET | CAM_VIEW_TARGET_POS);
                camera->targetPosRot.pos = *(Vec3f*)param;
                break;

            case CAM_VIEW_TARGET:
                if (camera->setting != CAM_SET_CS_C && camera->setting != CAM_SET_CS_ATTENTION) {
                    camera->target = (Actor*)param;
                    camera->viewFlags &= ~(CAM_VIEW_AT | CAM_VIEW_TARGET | CAM_VIEW_TARGET_POS);
                }
                break;

            case CAM_VIEW_EYE:
                camera->eye = camera->eyeNext = *(Vec3f*)param;
                break;

            case CAM_VIEW_UP:
                camera->up = *(Vec3f*)param;
                break;

            case CAM_VIEW_ROLL:
                camera->roll = CAM_DEG_TO_BINANG(*(f32*)param);
                break;

            case CAM_VIEW_FOV:
                camera->fov = *(f32*)param;
                break;

            default:
                return false;
        }
        camera->viewFlags |= viewFlag;
    } else {
        return false;
    }
    return true;
}

s32 Camera_UnsetViewFlag(Camera* camera, s16 viewFlag) {
    camera->viewFlags &= ~viewFlag;
    return true;
}

s32 Camera_OverwriteStateFlags(Camera* camera, s16 stateFlags) {
    camera->stateFlags = stateFlags;
    return true;
}

s32 Camera_ResetAnim(Camera* camera) {
    camera->animState = 0;
    return 1;
}

s32 Camera_SetCSParams(Camera* camera, CutsceneCameraPoint* atPoints, CutsceneCameraPoint* eyePoints, Player* player,
                       s16 relativeToPlayer) {
    PosRot playerPosRot;

    camera->data0 = atPoints;
    camera->data1 = eyePoints;
    camera->data2 = relativeToPlayer;

    if (camera->data2 != 0) {
        camera->player = player;
        Actor_GetWorldPosShapeRot(&playerPosRot, &player->actor);
        camera->playerPosRot = playerPosRot;

        camera->nextBgCamIndex = -1;
        camera->xzSpeed = 0.0f;
        camera->speedRatio = 0.0f;
    }

    return 1;
}

s16 Camera_SetStateFlag(Camera* camera, s16 stateFlag) {
    camera->stateFlags |= stateFlag;
    return camera->stateFlags;
}

s16 Camera_UnsetStateFlag(Camera* camera, s16 stateFlag) {
    camera->stateFlags &= ~stateFlag;
    return camera->stateFlags;
}

/**
 * A bgCamIndex of -99 will save the door params without changing the camera setting
 * A bgCamIndex of -1 uses the default door camera setting (CAM_SET_DOORC)
 * Otherwise, change the door camera setting by reading the bgCam indexed at bgCamIndex
 */
s32 Camera_ChangeDoorCam(Camera* camera, Actor* doorActor, s16 bgCamIndex, f32 arg3, s16 timer1, s16 timer2,
                         s16 timer3) {
    DoorParams* doorParams = &camera->paramData.doorParams;

    if ((camera->setting == CAM_SET_CS_ATTENTION) || (camera->setting == CAM_SET_DOORC)) {
        return 0;
    }

    doorParams->doorActor = doorActor;
    doorParams->timer1 = timer1;
    doorParams->timer2 = timer2;
    doorParams->timer3 = timer3;
    doorParams->bgCamIndex = bgCamIndex;

    if (bgCamIndex == -99) {
        Camera_CopyDataToRegs(camera, camera->mode);
        return -99;
    }

    if (bgCamIndex == -1) {
        Camera_ChangeSetting(camera, CAM_SET_DOORC);
        osSyncPrintf(".... change default door camera (set %d)\n", CAM_SET_DOORC);
    } else {
        s32 setting = Camera_GetBgCamSetting(camera, bgCamIndex);

        camera->behaviorFlags |= CAM_BEHAVIOR_BG_2;

        if (Camera_ChangeSetting(camera, setting) >= 0) {
            camera->bgCamIndex = bgCamIndex;
            camera->behaviorFlags |= CAM_BEHAVIOR_BG_1;
        }

        osSyncPrintf("....change door camera ID %d (set %d)\n", camera->bgCamIndex, camera->setting);
    }

    Camera_CopyDataToRegs(camera, camera->mode);
    return -1;
}

s32 Camera_Copy(Camera* dstCamera, Camera* srcCamera) {
    s32 pad;

    dstCamera->posOffset.x = 0.0f;
    dstCamera->posOffset.y = 0.0f;
    dstCamera->posOffset.z = 0.0f;
    dstCamera->atLERPStepScale = 0.1f;
    dstCamera->at = srcCamera->at;

    dstCamera->eye = dstCamera->eyeNext = srcCamera->eye;

    dstCamera->dist = OLib_Vec3fDist(&dstCamera->at, &dstCamera->eye);
    dstCamera->fov = srcCamera->fov;
    dstCamera->roll = srcCamera->roll;
    func_80043B60(dstCamera);

    if (dstCamera->player != NULL) {
        Actor_GetWorld(&dstCamera->playerPosRot, &dstCamera->player->actor);
        dstCamera->posOffset.x = dstCamera->at.x - dstCamera->playerPosRot.pos.x;
        dstCamera->posOffset.y = dstCamera->at.y - dstCamera->playerPosRot.pos.y;
        dstCamera->posOffset.z = dstCamera->at.z - dstCamera->playerPosRot.pos.z;
        dstCamera->dist = OLib_Vec3fDist(&dstCamera->playerPosRot.pos, &dstCamera->eye);
        dstCamera->xzOffsetUpdateRate = 1.0f;
        dstCamera->yOffsetUpdateRate = 1.0f;
    }
    return true;
}

s32 Camera_GetDbgCamEnabled(void) {
    return gDbgCamEnabled;
}

Vec3f* Camera_GetQuakeOffset(Vec3f* quakeOffset, Camera* camera) {
    *quakeOffset = camera->quakeOffset;
    return quakeOffset;
}

void Camera_SetCameraData(Camera* camera, s16 setDataFlags, void* data0, void* data1, s16 data2, s16 data3,
                          UNK_TYPE arg6) {
    if (setDataFlags & 0x1) {
        camera->data0 = data0;
    }

    if (setDataFlags & 0x2) {
        camera->data1 = data1;
    }

    if (setDataFlags & 0x4) {
        camera->data2 = data2;
    }

    if (setDataFlags & 0x8) {
        camera->data3 = data3;
    }

    if (setDataFlags & 0x10) {
        osSyncPrintf(VT_COL(RED, WHITE) "camera: setCameraData: last argument not alive!\n" VT_RST);
    }
}

s32 Camera_QRegInit(void) {
    if (!R_RELOAD_CAM_PARAMS) {
        QREG(2) = 1;
        QREG(10) = -1;
        QREG(11) = 100;
        QREG(12) = 80;
        QREG(20) = 90;
        QREG(21) = 10;
        QREG(22) = 10;
        QREG(23) = 50;
        QREG(24) = 6000;
        QREG(25) = 240;
        QREG(26) = 40;
        QREG(27) = 85;
        QREG(28) = 55;
        QREG(29) = 87;
        QREG(30) = 23;
        QREG(31) = 20;
        QREG(32) = 4;
        QREG(33) = 5;
        QREG(50) = 1;
        QREG(51) = 20;
        QREG(52) = 200;
        QREG(53) = 1;
        QREG(54) = 15;
        QREG(55) = 60;
        QREG(56) = 15;
        QREG(57) = 30;
        QREG(58) = 0;
    }

    QREG(65) = 50;
    return true;
}

s32 func_8005B198(void) {
    return D_8011D3AC;
}

s16 func_8005B1A4(Camera* camera) {
    camera->stateFlags |= CAM_STATE_3;

    if ((camera->camId == CAM_ID_MAIN) && (camera->play->activeCamId != CAM_ID_MAIN)) {
        GET_ACTIVE_CAM(camera->play)->stateFlags |= CAM_STATE_3;
        return camera->play->activeCamId;
    }

    return camera->camId;
}
