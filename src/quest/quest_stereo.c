/*
 * Stereo capture for the Meta Quest build (WC2_QUEST).
 *
 * WC2 draws space as scaled, pre-rendered sprites into a 320x200 buffer and
 * overlays the cockpit art.  For VR this file takes over every space object
 * after the engine's own view transform (Draw_3Space_Frame -> sort), turns it
 * into a camera-facing billboard at its real 3D position in cockpit space,
 * and suppresses its software draw (SdlQuestStereoOwnsObject).  Objects the
 * engine culls because they are outside its ~55 degree view cone are
 * transformed here too, so the pilot can look around.  The engine's fake
 * starfield sprites are replaced by a fixed star sphere on the XR side.
 *
 * What remains in the 320x200 frame is the cockpit art and the HUD.  It is
 * split per pixel: the window region's empty-space colour becomes
 * transparent, HUD pixels inside the window go to a far panel (so reticle
 * and target brackets sit on the targets), everything else is the dashboard
 * panel close to the pilot.  Both panels subtend exactly the engine's
 * projection, so brackets line up with the billboards.
 */
#include "game.h"
#include "../sdl/video_internal.h"
#include "../quest/quest_xr.h"
#include "../quest/quest_scene.h"

#include <math.h>
#include <stdlib.h>

#define FAR_DISTANCE_M 20000.0f
#define STEREO_TIMEOUT_MS 300
#define CACHE_SIZE 2048
#define ATLAS_PAD 1

/* ------------------------------------------------------------------ */
/* Configuration (/sdcard/WingCommander2/vr.cfg, key=value lines)        */
/* ------------------------------------------------------------------ */

static int g_configLoaded;
static int g_stereoEnabled = 1;
static float g_worldScale = 0.25f;  /* metres per game unit */
static float g_dashDistance = 0.9f; /* metres */
static float g_hudDistance = 8.0f;  /* metres */
static float g_stretch = 1.2f;      /* 320x200 shown at 4:3 */
static int g_cockpit3d = 1;
static int g_cockpitWrap = 1;

static void LoadConfig(void)
{
    char *text;
    size_t size = 0;
    char *line;

    g_configLoaded = 1;
    text = (char *)SDL_LoadFile("/sdcard/WingCommander2/vr.cfg", &size);
    if (text != 0) {
        char *copy = (char *)SDL_malloc(size + 1);
        if (copy != 0) {
            memcpy(copy, text, size);
            copy[size] = 0;
            for (line = strtok(copy, "\r\n"); line != 0;
                 line = strtok(0, "\r\n")) {
                float value;
                if (sscanf(line, "world_scale=%f", &value) == 1 && value > 0)
                    g_worldScale = value;
                else if (sscanf(line, "dash_distance=%f", &value) == 1 &&
                         value > 0.1f)
                    g_dashDistance = value;
                else if (sscanf(line, "hud_distance=%f", &value) == 1 &&
                         value > 0.1f)
                    g_hudDistance = value;
                else if (sscanf(line, "stretch=%f", &value) == 1 &&
                         value > 0.5f)
                    g_stretch = value;
                else if (sscanf(line, "stereo=%f", &value) == 1)
                    g_stereoEnabled = value != 0.0f;
                else if (sscanf(line, "cockpit3d=%f", &value) == 1)
                    g_cockpit3d = value != 0.0f;
                else if (sscanf(line, "cockpit_wrap=%f", &value) == 1)
                    g_cockpitWrap = value != 0.0f;
            }
            SDL_free(copy);
        }
        SDL_free(text);
    }
    printf("stereo config: enabled %d world_scale %.3f dash %.2f m hud %.2f m "
           "stretch %.2f cockpit3d %d wrap %d\n", g_stereoEnabled,
           g_worldScale, g_dashDistance, g_hudDistance, g_stretch,
           g_cockpit3d, g_cockpitWrap);
}

/* ------------------------------------------------------------------ */
/* Sprite atlas (game side owns the packing; XR thread owns the texture) */
/* ------------------------------------------------------------------ */

typedef struct AtlasEntry {
    unsigned char *shape;
    int dataOffset;
    unsigned short tableEnd;
    short frame, width, height, left, top;
    short layer, x, y;
} AtlasEntry;

typedef struct AtlasShelf {
    int nextX, nextY, rowHeight;
} AtlasShelf;

static AtlasEntry g_cache[CACHE_SIZE];
static AtlasShelf g_shelves[QUEST_ATLAS_LAYERS];
static int g_resetPending;

static void ResetAtlas(void)
{
    memset(g_cache, 0, sizeof(g_cache));
    memset(g_shelves, 0, sizeof(g_shelves));
    g_resetPending = 0;
}

static int ReserveAtlas(int width, int height, short *layer, short *x,
                        short *y)
{
    int packedW = width + ATLAS_PAD * 2;
    int packedH = height + ATLAS_PAD * 2;
    int l;

    if (packedW > QUEST_ATLAS_SIZE || packedH > QUEST_ATLAS_SIZE)
        return 0;
    for (l = 0; l < QUEST_ATLAS_LAYERS; l++) {
        AtlasShelf *shelf = &g_shelves[l];
        int px = shelf->nextX;
        int py = shelf->nextY;

        if (px + packedW > QUEST_ATLAS_SIZE) {
            px = 0;
            py += shelf->rowHeight;
            shelf->rowHeight = 0;
        }
        if (py + packedH <= QUEST_ATLAS_SIZE) {
            *layer = (short)l;
            *x = (short)(px + ATLAS_PAD);
            *y = (short)(py + ATLAS_PAD);
            shelf->nextX = px + packedW;
            shelf->nextY = py;
            if (packedH > shelf->rowHeight)
                shelf->rowHeight = packedH;
            return 1;
        }
    }
    return 0;
}

static const AtlasEntry *GetAtlasFrame(unsigned char *shape, short frame)
{
    unsigned short tableEnd;
    int dataOffset;
    short width = 0, height = 0, left = 0, top = 0;
    unsigned int slot;
    unsigned int probe;
    uintptr_t ptr;
    AtlasEntry *entry = 0;

    if (shape == 0 || frame < 0 || frame >= GetShapeFrameCount(shape))
        return 0;
    memcpy(&tableEnd, shape + 4, sizeof(tableEnd));
    memcpy(&dataOffset, shape + frame * 4 + 4, sizeof(dataOffset));
    GetShapeFrameExtents(shape, frame, &width, &height, &left, &top);
    if (width <= 0 || height <= 0)
        return 0;

    ptr = (uintptr_t)shape;
    slot = (unsigned int)(ptr ^ (ptr >> 16));
    slot ^= (unsigned int)(unsigned short)frame * 0x9e3779b1U;
    slot ^= (unsigned int)dataOffset * 0x85ebca6bU;
    slot = (slot ^ (slot >> 16)) & (CACHE_SIZE - 1);
    for (probe = 0; probe < CACHE_SIZE; probe++) {
        AtlasEntry *candidate = &g_cache[slot];
        if (candidate->shape == 0) {
            entry = candidate;
            break;
        }
        if (candidate->shape == shape && candidate->frame == frame &&
            candidate->dataOffset == dataOffset &&
            candidate->tableEnd == tableEnd && candidate->width == width &&
            candidate->height == height && candidate->left == left &&
            candidate->top == top)
            return candidate;
        slot = (slot + 1) & (CACHE_SIZE - 1);
    }
    if (entry == 0) {
        g_resetPending = 1;
        return 0;
    }
    {
        size_t pixels = (size_t)width * (size_t)height;
        unsigned char *bitmap = (unsigned char *)malloc(pixels);
        short layer, x, y;

        if (bitmap == 0)
            return 0;
        if (!ReserveAtlas(width, height, &layer, &x, &y)) {
            free(bitmap);
            g_resetPending = 1;
            return 0;
        }
        memset(bitmap, 0xff, pixels);
        DecodeShapeFrame(shape, frame, bitmap, width, height, left, top);
        QuestXr_QueueAtlasUpload(layer, x, y, width, height, bitmap);
        entry->shape = shape;
        entry->dataOffset = dataOffset;
        entry->tableEnd = tableEnd;
        entry->frame = frame;
        entry->width = width;
        entry->height = height;
        entry->left = left;
        entry->top = top;
        entry->layer = layer;
        entry->x = x;
        entry->y = y;
    }
    return entry;
}

/* ------------------------------------------------------------------ */
/* Frame capture                                                        */
/* ------------------------------------------------------------------ */

typedef struct SortedQuad {
    float key;
    QuestSceneQuad quad;
} SortedQuad;

typedef struct Billboard {
    float p[3];       /* centre, cockpit metres */
    float right[3];   /* unit vectors of the billboard plane */
    float up[3];
    float metresPerPixel; /* horizontal; vertical is * stretch */
    int valid;
} Billboard;

static QuestSceneHeader g_header;
static SortedQuad g_sorted[QUEST_SCENE_MAX_QUADS];
static QuestSceneQuad g_quads[QUEST_SCENE_MAX_QUADS];
static int g_sortedCount;
static unsigned char g_mask[SDL_FRAME_WIDTH * SDL_FRAME_HEIGHT];
static unsigned char g_clearColour;
static unsigned char g_owned[SPACE_OBJECT_COUNT];
static Billboard g_billboards[SPACE_OBJECT_COUNT];
static int g_stereoActive;
static Uint32 g_lastCaptureTicks;
static unsigned char g_hud[SDL_FRAME_WIDTH * SDL_FRAME_HEIGHT * 4];
static unsigned char g_dash[SDL_FRAME_WIDTH * SDL_FRAME_HEIGHT * 4];
static unsigned char g_loggedTypes[64];
static int g_captureCount;
static Uint32 g_statsTicks;
static int g_loggedGeometry;

static void BuildMask(const ScreenViewportGeometry *geometry,
                      int viewportMode, int fullViewportCopy)
{
    const short *run;
    int runCount = 0;

    memset(g_mask, 0, sizeof(g_mask));
    if (fullViewportCopy || viewportMode == 5 || geometry == 0) {
        memset(g_mask, 0xff, sizeof(g_mask));
        return;
    }
    run = (const short *)((const unsigned char *)geometry +
                          offsetof(ScreenViewportGeometry, fadeData));
    while (*run != -1 && runCount < 2048) {
        int x = *run++;
        int y = *run++;
        int length = (unsigned short)*run++;
        int start = y * SDL_FRAME_WIDTH + x;

        if (start < 0) {
            length += start;
            start = 0;
        }
        if (start < (int)sizeof(g_mask)) {
            int available = (int)sizeof(g_mask) - start;
            if (length > available)
                length = available;
            if (length > 0)
                memset(g_mask + start, 0xff, (size_t)length);
        }
        runCount++;
    }
}

/* Per column: first row below the canopy window.  Columns without window
 * pixels take the nearest window column; then a box filter smooths jagged
 * window edges so the console surface has no spikes. */
static void ComputeConsoleTop(short *out)
{
    int raw[SDL_FRAME_WIDTH];
    int x, y, any = 0;

    for (x = 0; x < SDL_FRAME_WIDTH; x++) {
        raw[x] = -1;
        for (y = SDL_FRAME_HEIGHT - 1; y >= 0; y--)
            if (g_mask[y * SDL_FRAME_WIDTH + x]) {
                raw[x] = y + 1;
                any = 1;
                break;
            }
    }
    if (!any) {
        for (x = 0; x < SDL_FRAME_WIDTH; x++)
            out[x] = (short)(SDL_FRAME_HEIGHT * 55 / 100);
        return;
    }
    for (x = 0; x < SDL_FRAME_WIDTH; x++)
        if (raw[x] < 0) {
            int d;
            for (d = 1; d < SDL_FRAME_WIDTH; d++) {
                if (x - d >= 0 && raw[x - d] >= 0) {
                    raw[x] = -2 - raw[x - d];
                    break;
                }
                if (x + d < SDL_FRAME_WIDTH && raw[x + d] >= 0) {
                    raw[x] = -2 - raw[x + d];
                    break;
                }
            }
        }
    for (x = 0; x < SDL_FRAME_WIDTH; x++)
        if (raw[x] < -1)
            raw[x] = -2 - raw[x];
    for (x = 0; x < SDL_FRAME_WIDTH; x++) {
        int sum = 0, n = 0, k;
        for (k = x - 12; k <= x + 12; k++)
            if (k >= 0 && k < SDL_FRAME_WIDTH) {
                sum += raw[k];
                n++;
            }
        out[x] = (short)(sum / n);
    }
}

static void Normalize3(float *v)
{
    float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-6f) {
        v[0] /= l;
        v[1] /= l;
        v[2] /= l;
    }
}

/* Billboard frame for an object at cockpit position p, facing the eye. */
static void MakeBillboard(Billboard *b, const float *p, float focal)
{
    float fwd[3];
    float dist;

    b->p[0] = p[0];
    b->p[1] = p[1];
    b->p[2] = p[2];
    dist = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
    fwd[0] = p[0] / dist;
    fwd[1] = p[1] / dist;
    fwd[2] = p[2] / dist;
    /* right = fwd x (0,1,0) */
    b->right[0] = -fwd[2];
    b->right[1] = 0.0f;
    b->right[2] = fwd[0];
    if (b->right[0] * b->right[0] + b->right[2] * b->right[2] < 1e-6f) {
        b->right[0] = 1.0f;
        b->right[2] = 0.0f;
    }
    Normalize3(b->right);
    /* up = right x fwd */
    b->up[0] = b->right[1] * fwd[2] - b->right[2] * fwd[1];
    b->up[1] = b->right[2] * fwd[0] - b->right[0] * fwd[2];
    b->up[2] = b->right[0] * fwd[1] - b->right[1] * fwd[0];
    b->metresPerPixel = dist / focal;
    b->valid = 1;
}

/* Game view vector (units << 8, y down) -> cockpit metres. */
static void ViewToCockpit(const FixedVector *view, float *p)
{
    p[0] = (float)view->x / 256.0f * g_worldScale;
    p[1] = -(float)view->y / 256.0f * g_worldScale * g_stretch;
    p[2] = -(float)view->z / 256.0f * g_worldScale;
}

static void AddSprite(const Billboard *b, float offsetX, float offsetY,
                      unsigned char *shape, short frame, short angle,
                      short scale, short flip, float key)
{
    const AtlasEntry *entry;
    QuestSceneQuad *q;
    float corners[4][2];
    float scaleX, scaleY, cosine, sine;
    float pw, ph;
    int fixedCos, fixedSin;
    int i;

    if (g_sortedCount >= QUEST_SCENE_MAX_QUADS || scale == 0)
        return;
    entry = GetAtlasFrame(shape, frame);
    if (entry == 0)
        return;
    GetRLETransformTrig((int)angle * 10, &fixedCos, &fixedSin);
    cosine = (float)fixedCos / 65536.0f;
    sine = (float)fixedSin / 65536.0f;
    scaleX = (float)scale / 256.0f;
    scaleY = scaleX;
    if (flip & 0x10)
        scaleX = -scaleX;
    if (flip & 0x20)
        scaleY = -scaleY;
    corners[0][0] = -(float)entry->left;
    corners[0][1] = -(float)entry->top;
    corners[1][0] = corners[0][0];
    corners[1][1] = corners[0][1] + entry->height;
    corners[2][0] = corners[0][0] + entry->width;
    corners[2][1] = corners[0][1];
    corners[3][0] = corners[2][0];
    corners[3][1] = corners[1][1];

    pw = b->metresPerPixel;
    ph = b->metresPerPixel * g_stretch;
    q = &g_sorted[g_sortedCount].quad;
    for (i = 0; i < 4; i++) {
        float sx = corners[i][0] * scaleX;
        float sy = corners[i][1] * scaleY;
        float x = offsetX + sx * cosine - sy * sine;  /* screen px, right */
        float y = offsetY + sx * sine + sy * cosine;  /* screen px, down */
        int a;
        for (a = 0; a < 3; a++)
            q->p[i][a] = b->p[a] + b->right[a] * x * pw - b->up[a] * y * ph;
    }
    q->uv[0][0] = entry->x;
    q->uv[0][1] = entry->y;
    q->uv[1][0] = entry->x;
    q->uv[1][1] = entry->y + entry->height;
    q->uv[2][0] = entry->x + entry->width;
    q->uv[2][1] = entry->y;
    q->uv[3][0] = entry->x + entry->width;
    q->uv[3][1] = entry->y + entry->height;
    q->layer = entry->layer;
    g_sorted[g_sortedCount].key = key;
    g_sortedCount++;
}

static int CompareFarFirst(const void *a, const void *b)
{
    float ka = ((const SortedQuad *)a)->key;
    float kb = ((const SortedQuad *)b)->key;
    return ka < kb ? 1 : ka > kb ? -1 : 0;
}

static void LogTypeOnce(int obj, unsigned char *shape, short frame,
                        short scale, float distanceUnits, float focal)
{
    int type = (int)g_asObjectType_00495298[obj];
    short w = 0, h = 0, l = 0, t = 0;

    if (type < 0 || type >= 64 || g_loggedTypes[type] || shape == 0)
        return;
    g_loggedTypes[type] = 1;
    GetShapeFrameExtents(shape, frame, &w, &h, &l, &t);
    printf("stereo object type %d class %d: sprite %dx%d scale %d, "
           "distance %.0f units, width %.1f units, radius %d\n", type,
           (int)g_aeObjectClass_00495328[obj], w, h, scale, distanceUnits,
           (float)w * scale / 256.0f / focal * distanceUnits,
           (int)g_asObjectCollisionRadius_004950e8[obj]);
}

void SdlQuestCaptureSpaceFrame(const ScreenViewportGeometry *geometry,
                               int viewportMode, int fullViewportCopy,
                               unsigned char clearColour)
{
    float focal;
    float centreX, centreY;
    int originX = 0, originY = 0;
    int obj;
    int i;

    if (!g_configLoaded)
        LoadConfig();
    if (!g_stereoEnabled)
        return;
    if (g_resetPending)
        ResetAtlas();

    if (!fullViewportCopy && viewportMode != 5 && geometry != 0) {
        originX = geometry->originX;
        originY = geometry->originY;
    }
    focal = (float)(g_nScreenWidth_0049d4d8 & ~1) * 0.5f;
    if (focal < 1.0f)
        focal = 160.0f;
    centreX = (float)(g_nViewCenterX_005c80d8 + originX);
    centreY = (float)(g_nViewCenterY_005c80da + originY);
    BuildMask(geometry, viewportMode, fullViewportCopy);
    g_clearColour = clearColour;

    memset(&g_header, 0, sizeof(g_header));
    g_header.centreX = centreX;
    g_header.centreY = centreY;
    g_header.focal = focal;
    g_header.stretch = g_stretch;
    g_header.dashDistance = g_dashDistance;
    g_header.hudDistance = g_hudDistance;
    g_header.hasDashboard =
        !(fullViewportCopy || viewportMode == 5 || geometry == 0);
    g_header.cockpit3d = g_cockpit3d;
    g_header.cockpitWrap = g_cockpitWrap;
    if (g_header.hasDashboard)
        ComputeConsoleTop(g_header.consoleTop);
    for (i = 0; i < 3; i++) {
        g_header.worldToCockpit[0][i] =
            (float)(&g_aShipRightVector_00493b78[EYE_OBJECT].x)[i] / 256.0f;
        g_header.worldToCockpit[1][i] =
            -(float)(&g_aShipUpVector_00493ec0[EYE_OBJECT].x)[i] / 256.0f * g_stretch;
        g_header.worldToCockpit[2][i] =
            -(float)(&g_aShipForwardVector_00494208[EYE_OBJECT].x)[i] / 256.0f;
    }
    if (!g_loggedGeometry) {
        g_loggedGeometry = 1;
        printf("stereo geometry: focal %.0f centre %.0f,%.0f origin %d,%d "
               "mode %d full %d clear %d eye radius %d\n", focal, centreX,
               centreY, originX, originY, viewportMode, fullViewportCopy,
               clearColour, (int)g_asObjectCollisionRadius_004950e8[EYE_OBJECT]);
    }

    memset(g_owned, 0, sizeof(g_owned));
    memset(g_billboards, 0, sizeof(g_billboards));
    g_sortedCount = 0;

    for (obj = 0; obj <= SPACE_LAST_MOVING_OBJECT; obj++) {
        enum ObjectClass cls = g_aeObjectClass_00495328[obj];
        FixedVector view;
        FixedVector direction;
        unsigned char *shape;
        short frame, angle, flip, scale;
        float key;
        float p[3];
        int visible;

        if ((int)g_asObjectType_00495298[obj] < 0)
            continue;
        if (cls == OBJECT_CLASS_NULL || cls == OBJECT_CLASS_FUTURION ||
            cls == OBJECT_CLASS_FIXED_OBJECT || cls == OBJECT_CLASS_STAR ||
            obj == g_nNavPointerObject_004931b8 || obj == EYE_OBJECT)
            continue;
        visible = g_asObjectScreenX_00493598[obj] != (short)0x8001;

        if (visible) {
            view = g_aObjectViewPosition_0059afa0[obj];
            frame = g_asObjectViewFrame_00493508[obj];
            angle = g_asObjectScreenAngle_004936b8[obj];
            flip = g_asObjectFlip_004939c8[obj];
            scale = g_asObjectScreenScale_00493a58[obj];
            key = (float)g_asObjectDistance_00493ae8[obj];
        } else {
            long distance;

            if (cls == OBJECT_CLASS_CAPITAL_SHIP && g_apObjectShape_00493868[obj] == 0)
                continue;
            if (cls == OBJECT_CLASS_PLANET)
                direction = g_aShipPosition_00494550[obj];
            else
                ComputeVectorDelta(&g_aShipPosition_00494550[EYE_OBJECT],
                                   &g_aShipPosition_00494550[obj], &direction);
            distance = Vector_magnitude(&direction);
            if (distance < g_asObjectCollisionRadius_004950e8[EYE_OBJECT] * 0x100)
                continue;
            if (cls == OBJECT_CLASS_DUST && distance > (1400 << 8))
                continue;
            transform_to_objects_frame(&direction, &view, EYE_OBJECT);
            frame = g_asObjectViewFrame_00493508[obj];
            angle = g_asObjectScreenAngle_004936b8[obj];
            flip = g_asObjectFlip_004939c8[obj];
            scale = g_asObjectScreenScale_00493a58[obj];
            if (cls > OBJECT_CLASS_DUST && cls != OBJECT_CLASS_PLANET) {
                int radius = g_asObjectCollisionRadius_004950e8[obj] * 0x100;
                long d = distance <= radius ? radius + 1 : distance;
                long factor = DivideFixed((short)(g_nScreenWidth_0049d4d8 & ~1) << 15,
                                          (int)(d - radius));
                scale = (short)(MultiplyFixed(
                                    (unsigned short)g_asObjectScale_00494d90[obj],
                                    (int)factor) >> 8);
                if ((unsigned short)scale > 0x1fff)
                    scale = 0x2000;
                if ((unsigned short)scale < 5)
                    continue;
            }
            if (cls == OBJECT_CLASS_SHIP || cls == OBJECT_CLASS_MISSILE) {
                /* get_right_shape picks the view frame from the viewing
                 * direction only; restore the engine's values afterwards. */
                short savedFrame = g_asObjectViewFrame_00493508[obj];
                short savedAngle = g_asObjectScreenAngle_004936b8[obj];
                short savedFlip = g_asObjectFlip_004939c8[obj];
                FixedVector copy = direction;

                get_right_shape((short)obj, &copy);
                frame = g_asObjectViewFrame_00493508[obj];
                angle = g_asObjectScreenAngle_004936b8[obj];
                flip = g_asObjectFlip_004939c8[obj];
                g_asObjectViewFrame_00493508[obj] = savedFrame;
                g_asObjectScreenAngle_004936b8[obj] = savedAngle;
                g_asObjectFlip_004939c8[obj] = savedFlip;
            } else if (cls == OBJECT_CLASS_DUST) {
                short dustSize = (short)(MultiplyFixed(
                    0x900, (int)DivideFixed(
                               g_asObjectCollisionRadius_004950e8[EYE_OBJECT] << 8,
                               (int)distance)) >> 8);
                if (dustSize > 3)
                    dustSize = 3;
                frame = (short)(((g_asObjectCounter_00494be0[obj] +
                                  g_nSpaceFrame_00493134) & 3) +
                                (g_asObjectScreenAngle_004936b8[obj] & 0x10) +
                                (3 - dustSize) * 4);
            }
            key = (float)(distance >> 8);
        }

        if (cls == OBJECT_CLASS_DUST) {
            shape = g_pConstellationShape_005d2c4c;
            angle = 0;
            scale = 0x100;
            flip = 0;
        } else {
            shape = g_apObjectShape_00493868[obj];
        }
        if (shape == 0)
            continue;

        if (cls == OBJECT_CLASS_PLANET) {
            float l;
            ViewToCockpit(&view, p);
            l = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
            if (l < 1e-6f)
                continue;
            p[0] = p[0] / l * FAR_DISTANCE_M;
            p[1] = p[1] / l * FAR_DISTANCE_M;
            p[2] = p[2] / l * FAR_DISTANCE_M;
            key = 1e9f;
        } else {
            ViewToCockpit(&view, p);
            if (p[0] * p[0] + p[1] * p[1] + p[2] * p[2] < 1e-4f)
                continue;
        }
        MakeBillboard(&g_billboards[obj], p, focal);
        AddSprite(&g_billboards[obj], 0.0f, 0.0f, shape, frame, angle, scale,
                  flip, key);
        g_owned[obj] = 1;
        if (visible && cls >= OBJECT_CLASS_MISSILE)
            LogTypeOnce(obj, shape, frame, scale, key, focal);
    }

    /* Thrusters and turrets ride on their parent's billboard. */
    for (obj = 10; obj <= SPACE_LAST_MOVING_OBJECT; obj++) {
        int parent;
        float dx, dy;

        if (g_aeObjectClass_00495328[obj] != OBJECT_CLASS_FIXED_OBJECT ||
            (int)g_asObjectType_00495298[obj] < 0)
            continue;
        if (g_asObjectType_00495298[obj] != OBJECT_DATA_THRUSTERS &&
            g_asObjectType_00495298[obj] != OBJECT_DATA_TURRET_GUN)
            continue;
        parent = (int)g_acObjectOwner_00495208[obj];
        if (parent < 0 || parent >= SPACE_OBJECT_COUNT ||
            !g_billboards[parent].valid ||
            g_asObjectScreenX_00493598[parent] == (short)0x8001 ||
            g_aObjectViewPosition_0059afa0[parent].z == 0)
            continue;
        if (g_asObjectType_00495298[obj] == OBJECT_DATA_THRUSTERS) {
            float tx, ty;
            float px = (float)g_nViewCenterX_005c80d8 +
                       focal * (float)g_aObjectViewPosition_0059afa0[parent].x /
                           (float)g_aObjectViewPosition_0059afa0[parent].z;
            float py = (float)g_nViewCenterY_005c80da +
                       focal * (float)g_aObjectViewPosition_0059afa0[parent].y /
                           (float)g_aObjectViewPosition_0059afa0[parent].z;
            SdlGetThrusterScreenPosition((short)obj, &tx, &ty);
            dx = tx - px;
            dy = ty - py;
        } else {
            dx = (float)(g_asObjectScreenX_00493598[obj] - g_asObjectScreenX_00493598[parent]);
            dy = (float)(g_asObjectScreenY_00493628[obj] - g_asObjectScreenY_00493628[parent]);
        }
        AddSprite(&g_billboards[parent], dx, dy, g_apObjectShape_00493868[obj],
                  g_asObjectViewFrame_00493508[obj], g_asObjectScreenAngle_004936b8[obj],
                  g_asObjectScreenScale_00493a58[obj], g_asObjectFlip_004939c8[obj],
                  (float)g_asObjectDistance_00493ae8[obj]);
        g_owned[obj] = 1;
    }

    qsort(g_sorted, (size_t)g_sortedCount, sizeof(g_sorted[0]),
          CompareFarFirst);
    for (i = 0; i < g_sortedCount; i++)
        g_quads[i] = g_sorted[i].quad;
    g_header.quadCount = g_sortedCount;

    g_stereoActive = 1;
    g_lastCaptureTicks = SDL_GetTicks();
    g_captureCount++;
    if (g_lastCaptureTicks - g_statsTicks >= 5000) {
        if (g_statsTicks != 0)
            printf("stereo: %.1f space frames/s, %d billboards\n",
                   g_captureCount * 1000.0f /
                       (float)(g_lastCaptureTicks - g_statsTicks),
                   g_sortedCount);
        g_statsTicks = g_lastCaptureTicks;
        g_captureCount = 0;
    }
}

/* obj == -2 asks whether a stereo frame is being captured at all. */
int SdlQuestStereoOwnsObject(int obj)
{
    if (obj == -2)
        return g_stereoActive;
    return g_stereoActive && obj >= 0 && obj < SPACE_OBJECT_COUNT &&
           g_owned[obj];
}

void SdlQuestCancelStereo(void)
{
    if (g_stereoActive)
        printf("stereo: off\n");
    g_stereoActive = 0;
    memset(g_owned, 0, sizeof(g_owned));
    g_resetPending = 1;
}

/* Called for every presented frame.  Returns 1 when it was sent as a stereo
 * frame (cockpit panels + billboards). */
int SdlQuestPresentStereo(const unsigned char *pixels,
                          const unsigned char *palette)
{
    int i;

    if (!g_stereoActive)
        return 0;
    if (SDL_GetTicks() - g_lastCaptureTicks > STEREO_TIMEOUT_MS) {
        SdlQuestCancelStereo();
        return 0;
    }
    for (i = 0; i < SDL_FRAME_WIDTH * SDL_FRAME_HEIGHT; i++) {
        unsigned char index = pixels[i];
        const unsigned char *c = palette + index * 4;
        unsigned char *hud = g_hud + i * 4;
        unsigned char *dash = g_dash + i * 4;
        int inWindow = g_mask[i] != 0;

        if (inWindow && index == g_clearColour) {
            memset(hud, 0, 4);
            memset(dash, 0, 4);
        } else if (inWindow || !g_header.hasDashboard) {
            hud[0] = c[2];
            hud[1] = c[1];
            hud[2] = c[0];
            hud[3] = 255;
            memset(dash, 0, 4);
        } else {
            dash[0] = c[2];
            dash[1] = c[1];
            dash[2] = c[0];
            dash[3] = 255;
            memset(hud, 0, 4);
        }
    }
    QuestXr_SubmitStereoFrame(&g_header, g_quads, g_hud, g_dash, palette);
    return 1;
}
