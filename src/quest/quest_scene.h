/*
 * Stereo space scene shared between the game thread (quest_stereo.c, which
 * captures it from the WC2 engine) and the XR thread (quest_xr.c, which
 * renders it per eye).
 *
 * Coordinates: "cockpit space" in metres, the XR recentre anchor's frame:
 * origin = the pilot's eye point, x right, y up, -z = the ship's nose.
 */
#ifndef WC2_QUEST_SCENE_H
#define WC2_QUEST_SCENE_H

#ifdef __cplusplus
extern "C" {
#endif

#define QUEST_SCENE_MAX_QUADS 96
#define QUEST_ATLAS_SIZE 2048
#define QUEST_ATLAS_LAYERS 4

typedef struct QuestSceneQuad {
    float p[4][3];      /* corners: top-left, bottom-left, top-right, bottom-right */
    float uv[4][2];     /* atlas texel coordinates */
    float layer;        /* atlas layer */
} QuestSceneQuad;

typedef struct QuestSceneHeader {
    int quadCount;
    /* Game world -> cockpit space rotation (rows), for the star sphere. */
    float worldToCockpit[3][3];
    /* Cockpit panels: the 320x200 frame mapped onto planes at these
     * distances.  Pixel (cx, cy) is straight ahead; one pixel spans
     * 1/focal (horizontal) and stretch/focal (vertical) of the distance. */
    float centreX, centreY, focal, stretch;
    float dashDistance, hudDistance;
    int hasDashboard;   /* 0: no cockpit art (external views, intro) */
    /* 3D cockpit: per column, the first row below the canopy window (the
     * console's top edge), smoothed.  cockpit3d 0 = flat dashboard panel. */
    short consoleTop[320];
    int cockpit3d;
    int cockpitWrap;    /* extend the art around the pilot */
} QuestSceneHeader;

/* Game -> XR: one stereo frame (copied; caller keeps ownership).
 * hud/dash: 320x200 premultiplied RGBA, top row first. palette: 256 x
 * [B,G,R,pad] for the sprite atlas. */
void QuestXr_SubmitStereoFrame(const QuestSceneHeader *header,
                               const QuestSceneQuad *quads,
                               const unsigned char *hudRgba,
                               const unsigned char *dashRgba,
                               const unsigned char *palette);
/* Game -> XR: indexed pixels (255 = transparent) for an atlas rectangle.
 * The XR thread takes ownership of pixels (free()). */
void QuestXr_QueueAtlasUpload(int layer, int x, int y, int width, int height,
                              unsigned char *pixels);

#ifdef __cplusplus
}
#endif

#endif
