/*
 * Meta Quest (OpenXR) host for the Wing Commander SDL2 port.
 *
 * The game keeps running on SDL's main thread and composes its 320x200
 * indexed frame as on the desktop.  A separate render thread owns an EGL
 * context and the OpenXR session; it shows the latest frame on a world-fixed
 * virtual screen and keeps presenting at the headset's rate even while the
 * game is loading or blocked.
 *
 * Input flows the other way: the render thread samples the Touch controllers
 * once per XR frame, and the game thread drains that state from
 * SdlPumpEvents() (laser pointer -> logical mouse, buttons -> keys in menus,
 * sticks/buttons -> a virtual SDL gamepad while piloting).
 */
#ifndef WC2_QUEST_XR_H
#define WC2_QUEST_XR_H

#ifdef __cplusplus
extern "C" {
#endif

#define QUEST_FRAME_WIDTH 320
#define QUEST_FRAME_HEIGHT 200

/* Touch controller snapshot, sampled on the XR thread. */
typedef struct QuestXrControllers {
    float leftStickX, leftStickY;   /* -1..1, +y = up */
    float rightStickX, rightStickY;
    float leftTrigger, rightTrigger; /* 0..1 */
    float leftGrip, rightGrip;
    int a, b, x, y;
    int leftStickClick, rightStickClick;
    int menu;
    int active;                      /* session focused, values valid */
} QuestXrControllers;

/* Laser pointer state relative to the virtual screen. */
typedef struct QuestXrPointer {
    int onScreen;        /* ray hits the screen */
    int x, y;            /* logical 320x200 coordinates */
    int primary;         /* right trigger (or left trigger, left hand aiming) */
    int secondary;       /* grip of the aiming hand */
} QuestXrPointer;

/* Starts the XR thread (EGL + OpenXR).  Call on the SDL main thread after
 * SDL is up.  Returns 1 when the thread started; the session itself becomes
 * visible asynchronously. */
int QuestXr_Start(void);
void QuestXr_Shutdown(void);
int QuestXr_IsRunning(void);

/* Game -> headset.  pixels: 320x200 palette indices; palette: 256 x
 * [B,G,R,pad] (the layout SdlPresentIndexedFrame receives). */
void QuestXr_SubmitIndexedFrame(const unsigned char *pixels,
                                const unsigned char *palette);
/* Blocks until the next headset display tick (vsync replacement). */
void QuestXr_WaitForDisplayTick(void);

/* Headset -> game (call on the game thread). */
void QuestXr_GetControllers(QuestXrControllers *controllers);
void QuestXr_GetPointer(QuestXrPointer *pointer);
/* Tells the XR thread whether the player is flying (laser hidden, no
 * clicks) or in a menu/2D screen (laser on, clicks enabled). */
void QuestXr_SetPiloting(int piloting);
/* Shows a solid colour instead of the game frame (data missing etc). */
void QuestXr_ShowErrorColour(unsigned int rgb);
/* Short haptic pulse on the right (hand=1) or left (hand=0) controller. */
void QuestXr_Haptic(int hand, float amplitude, float seconds);

/* On-screen keyboard (pilot names, callsigns, saved-game names).
 * rgba: QUEST_KEYBOARD_WIDTH x HEIGHT premultiplied pixels, top row first. */
void QuestXr_SetKeyboardImage(const unsigned char *rgba);
void QuestXr_SetKeyboardVisible(int visible);
/* Lights the Shift key (one-shot shift state owned by the game side). */
void QuestXr_SetKeyboardShift(int shift);
/* Next key typed on the keyboard (SDL keycode), 0 when none. */
int QuestXr_PopKeyboardKey(void);

/* Glue implemented by the game side (src/sdl, WC2_QUEST builds). */
int SdlQuestIsPiloting(void);
void SdlQuestServiceInput(void);
int SdlQuestIsQuestPad(int deviceIndex);
void SdlQuestSetTextInput(int active);
int SdlQuestTakeShift(void);

#ifdef __cplusplus
}
#endif

#endif
