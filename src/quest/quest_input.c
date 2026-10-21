/*
 * Game-side glue for the Meta Quest build (WC2_QUEST): start-up, logging,
 * and Touch controller input.
 *
 * Touch -> virtual SDL gamepad, read by the port's existing 4button-4axis
 * joystick code (src/sdl/joystick.c).  While piloting:
 *
 *   left stick        pitch / yaw            right stick X/Y  roll / throttle
 *   right trigger     fire guns (pad A)      left trigger     missile (pad B)
 *   right grip        afterburner (pad X)    A                cycle target (pad Y)
 *   B                 select missile (RB)    X                select guns (LB)
 *   Y                 pause (Start)          menu (left)      Escape (Back)
 *   left stick click  nav map (tap)          right stick click autopilot
 *   left grip + A/B/X/Y  D-pad right/down/left/up:
 *                     target lock / stop / comms / full speed
 *                     (comm menu: up/down select, right = send, left = close)
 *
 * Outside flight the pad is neutral; the laser drives the mouse
 * (trigger = left click, grip = right click), B / menu = Escape,
 * Y = "Y", X = "N", A = Enter.  In Yes/No prompts (load, save, quit):
 * A = yes, B = no.
 * Hold the left stick click for one second to recentre the screen.
 */
#include "game.h"
#include "../quest/quest_xr.h"

#include "../quest/quest_keyboard_layout.h"

#include <android/log.h>
#include <zlib.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#define QUEST_PAD_VENDOR 0x2833 /* Oculus VR */
#define QUEST_PAD_PRODUCT 0x5743 /* "WC" */
#define QUEST_PAD_NAME "Quest Touch (Wing Commander II)"

static int g_questPadDeviceIndex = -1;
static SDL_JoystickID g_questPadInstance = -1;

/* ------------------------------------------------------------------ */
/* stdout/stderr -> logcat                                              */
/* ------------------------------------------------------------------ */

static int g_logPipe[2];

static void *QuestLogThread(void *unused)
{
    char buffer[1024];
    size_t used = 0;
    (void)unused;

    for (;;) {
        ssize_t count = read(g_logPipe[0], buffer + used,
                             sizeof(buffer) - 1 - used);
        char *start;
        char *newline;

        if (count <= 0)
            break;
        used += (size_t)count;
        buffer[used] = 0;
        start = buffer;
        while ((newline = strchr(start, '\n')) != 0) {
            *newline = 0;
            __android_log_write(ANDROID_LOG_INFO, "WC2", start);
            start = newline + 1;
        }
        used = strlen(start);
        if (used == sizeof(buffer) - 1) {
            __android_log_write(ANDROID_LOG_INFO, "WC2", start);
            used = 0;
        } else {
            memmove(buffer, start, used);
        }
    }
    return 0;
}

static void QuestRedirectStdio(void)
{
    pthread_t thread;

    setvbuf(stdout, 0, _IOLBF, 0);
    setvbuf(stderr, 0, _IONBF, 0);
    if (pipe(g_logPipe) != 0)
        return;
    dup2(g_logPipe[1], 1);
    dup2(g_logPipe[1], 2);
    if (pthread_create(&thread, 0, QuestLogThread, 0) == 0)
        pthread_detach(thread);
}

/* ------------------------------------------------------------------ */
/* Start-up                                                             */
/* ------------------------------------------------------------------ */

static int QuestDirectoryHasGame(const char *path)
{
    char probe[PATH_MAX];
    char resolved[PATH_MAX];
    struct stat info;

    SDL_snprintf(probe, sizeof(probe), "%s/GAMEDAT/COCKPIT.VGA", path);
    if (!SdlResolvePath(probe, resolved, sizeof(resolved)))
        return 0;
    return stat(resolved, &info) == 0;
}

int SdlQuestStartup(int *argumentCount, char ***arguments)
{
    static char *questArguments[] = {
        "wc2", "--joystick-mode=4button-4axis",
        "--joystick-axes=twin-stick-roll", "--joystick-rumble", 0
    };
    static const char *const candidates[] = {
        "/sdcard/WingCommander2", "/sdcard/wingcommander2",
        "/sdcard/WC2", "/sdcard/WingCommander2/WING2",
        "/storage/emulated/0/WingCommander2"
    };
    size_t i;

    QuestRedirectStdio();
    printf("Wing Commander II VR starting\n");
    *argumentCount = 4;
    *arguments = questArguments;

    for (i = 0; i < SDL_arraysize(candidates); i++) {
        if (QuestDirectoryHasGame(candidates[i])) {
            if (SdlChangeDirectory(candidates[i]) == 0) {
                printf("game data: %s\n", candidates[i]);
                return 1;
            }
        }
    }
    fprintf(stderr, "No game data found. Copy the Wing Commander II "
                    "folder (WC2.EXE + GAMEDAT) to /sdcard/WingCommander2\n");
    /* Show a red screen so the problem is visible in the headset. */
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) == 0 && QuestXr_Start()) {
        QuestXr_ShowErrorColour(0x801818);
        while (QuestXr_IsRunning()) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_QUIT) {
                    QuestXr_Shutdown();
                    return 0;
                }
            }
            SDL_Delay(50);
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Virtual gamepad                                                      */
/* ------------------------------------------------------------------ */

static int SDLCALL QuestPadRumble(void *userdata, Uint16 low, Uint16 high)
{
    float amplitude = (float)(low > high ? low : high) / 65535.0f;
    (void)userdata;

    if (amplitude > 0.0f) {
        QuestXr_Haptic(0, amplitude, 0.1f);
        QuestXr_Haptic(1, amplitude, 0.1f);
    }
    return 0;
}

static void QuestLoadKeyboard(void)
{
    uLongf size = (uLongf)QUEST_KEYBOARD_WIDTH * QUEST_KEYBOARD_HEIGHT * 4;
    unsigned char *pixels;
    void *packed;
    size_t packedSize = 0;

    packed = SDL_LoadFile("keyboard.rgba.z", &packedSize);
    if (packed == 0) {
        fprintf(stderr, "keyboard asset missing: %s\n", SDL_GetError());
        return;
    }
    pixels = (unsigned char *)SDL_malloc(size);
    if (pixels != 0 &&
        uncompress(pixels, &size, (const Bytef *)packed, (uLong)packedSize) == Z_OK &&
        size == (uLongf)QUEST_KEYBOARD_WIDTH * QUEST_KEYBOARD_HEIGHT * 4) {
        QuestXr_SetKeyboardImage(pixels);
        printf("keyboard loaded\n");
    } else {
        fprintf(stderr, "keyboard asset corrupt\n");
    }
    SDL_free(pixels);
    SDL_free(packed);
}

static int g_textInputDepth;
static int g_yesNoPrompt;

void SdlQuestSetYesNoPrompt(int active)
{
    if (active != g_yesNoPrompt)
        printf("yes/no prompt %s\n", active ? "on (A = yes, B = no)" : "off");
    g_yesNoPrompt = active;
}
/* One-shot shift: on at the start of a field and after a space, so names
 * come out capitalised.  ReadTextInput (trainsim.c) consumes it when it
 * decides the case of a letter. */
static int g_shiftOneShot;

int SdlQuestTakeShift(void)
{
    int active = g_textInputDepth > 0 && g_shiftOneShot;

    if (active) {
        g_shiftOneShot = 0;
        QuestXr_SetKeyboardShift(0);
    }
    return active;
}

void SdlQuestSetTextInput(int active)
{
    g_textInputDepth += active ? 1 : -1;
    if (g_textInputDepth < 0)
        g_textInputDepth = 0;
    g_shiftOneShot = g_textInputDepth > 0;
    QuestXr_SetKeyboardShift(g_shiftOneShot);
    QuestXr_SetKeyboardVisible(g_textInputDepth > 0);
    printf("text input %s\n", g_textInputDepth > 0 ? "on" : "off");
}

void SdlQuestCreatePad(void)
{
    QuestLoadKeyboard();
    SDL_VirtualJoystickDesc desc;

    SDL_zero(desc);
    desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    desc.naxes = SDL_CONTROLLER_AXIS_MAX;
    desc.nbuttons = SDL_CONTROLLER_BUTTON_DPAD_RIGHT + 1;
    desc.vendor_id = QUEST_PAD_VENDOR;
    desc.product_id = QUEST_PAD_PRODUCT;
    desc.button_mask = (1U << (SDL_CONTROLLER_BUTTON_DPAD_RIGHT + 1)) - 1;
    desc.axis_mask = (1U << SDL_CONTROLLER_AXIS_MAX) - 1;
    desc.name = QUEST_PAD_NAME;
    desc.Rumble = QuestPadRumble;
    g_questPadDeviceIndex = SDL_JoystickAttachVirtualEx(&desc);
    if (g_questPadDeviceIndex < 0) {
        fprintf(stderr, "virtual pad: %s\n", SDL_GetError());
        return;
    }
    g_questPadInstance = SDL_JoystickGetDeviceInstanceID(g_questPadDeviceIndex);
    printf("virtual Touch pad attached (index %d, instance %d)\n",
           g_questPadDeviceIndex, (int)g_questPadInstance);
}

int SdlQuestIsQuestPad(int deviceIndex)
{
    return SDL_JoystickGetDeviceVendor(deviceIndex) == QUEST_PAD_VENDOR &&
           SDL_JoystickGetDeviceProduct(deviceIndex) == QUEST_PAD_PRODUCT;
}

/* ------------------------------------------------------------------ */
/* Per-pump input service                                               */
/* ------------------------------------------------------------------ */

enum {
    PHYS_RTRIG, PHYS_LTRIG, PHYS_RGRIP, PHYS_LGRIP, PHYS_A, PHYS_B, PHYS_X,
    PHYS_Y, PHYS_LCLICK, PHYS_RCLICK, PHYS_MENU, PHYS_COUNT
};

static int g_prevMode = -1;
static int g_latched[PHYS_COUNT];
static int g_prevPhys[PHYS_COUNT];
static Uint32 g_leftClickStart;
static int g_navPulse;
static int g_prevPointerX = -1, g_prevPointerY = -1;
static int g_prevPrimary, g_prevSecondary;

/* Flight stick shaping (/sdcard/WingCommander2/vr.cfg):
 *   stick_deadzone=0.10   ignore small deflections (rescaled, no jump)
 *   stick_expo=0.65       0 = linear, 1 = cubic: fine aim near centre,
 *                         full deflection still gives the full turn rate
 *   stick_sensitivity=1.0 overall scale for pitch / yaw / roll */
static int g_stickConfigLoaded;
static float g_stickDeadzone = 0.10f;
static float g_stickExpo = 0.65f;
static float g_stickSensitivity = 1.0f;

static void LoadStickConfig(void)
{
    char *text;
    size_t size = 0;

    g_stickConfigLoaded = 1;
    text = (char *)SDL_LoadFile("/sdcard/WingCommander2/vr.cfg", &size);
    if (text != 0) {
        char *line = text;
        char *end = text + size;

        while (line < end) {
            char buffer[128];
            size_t n = 0;
            float value;

            while (line < end && *line != '\n' && n + 1 < sizeof(buffer))
                buffer[n++] = *line++;
            while (line < end && *line != '\n')
                line++;
            line++;
            buffer[n] = 0;
            if (sscanf(buffer, "stick_deadzone=%f", &value) == 1 &&
                value >= 0.0f && value < 0.9f)
                g_stickDeadzone = value;
            else if (sscanf(buffer, "stick_expo=%f", &value) == 1 &&
                     value >= 0.0f && value <= 1.0f)
                g_stickExpo = value;
            else if (sscanf(buffer, "stick_sensitivity=%f", &value) == 1 &&
                     value > 0.05f && value <= 2.0f)
                g_stickSensitivity = value;
        }
        SDL_free(text);
    }
    printf("stick config: deadzone %.2f expo %.2f sensitivity %.2f\n",
           g_stickDeadzone, g_stickExpo, g_stickSensitivity);
}

static float ShapeStick(float v)
{
    float a, sign;

    if (!g_stickConfigLoaded)
        LoadStickConfig();
    sign = v < 0.0f ? -1.0f : 1.0f;
    a = v * sign;
    if (a > 1.0f)
        a = 1.0f;
    if (a <= g_stickDeadzone)
        return 0.0f;
    a = (a - g_stickDeadzone) / (1.0f - g_stickDeadzone);
    a = (1.0f - g_stickExpo) * a + g_stickExpo * a * a * a;
    return sign * a * g_stickSensitivity;
}

static Sint16 AxisValue(float v)
{
    if (v > 1.0f)
        v = 1.0f;
    else if (v < -1.0f)
        v = -1.0f;
    return (Sint16)(v * 32767.0f);
}

static void PushKey(SDL_Keycode key, int pressed)
{
    SDL_Event event;

    SDL_zero(event);
    event.type = pressed ? SDL_KEYDOWN : SDL_KEYUP;
    event.key.state = pressed ? SDL_PRESSED : SDL_RELEASED;
    event.key.keysym.sym = key;
    event.key.keysym.scancode = SDL_GetScancodeFromKey(key);
    SDL_PushEvent(&event);
}

static void TapKey(SDL_Keycode key)
{
    PushKey(key, 1);
    PushKey(key, 0);
}

/* Text keys go through a FIFO released one per 60 ms: the game's key wait
 * (WaitForInputKey) keeps the first key of a pump and drains the rest. */
static SDL_Keycode g_typeQueue[256];
static int g_typeHead, g_typeTail;
static Uint32 g_typeNext;

static void QueueTypedKey(SDL_Keycode key)
{
    int next = (g_typeTail + 1) % 256;
    if (next != g_typeHead) {
        g_typeQueue[g_typeTail] = key;
        g_typeTail = next;
    }
}

/* ------------------------------------------------------------------ */
/* Developer command file: drive menus headless over adb.               */
/*   adb shell "echo 'click 160 100' > /sdcard/WingCommander2/vrcmd"      */
/* One command runs every 400 ms: click X Y | rclick X Y | key NAME |    */
/* type TEXT | wait.  The file is deleted once read.                     */
/* ------------------------------------------------------------------ */

static char g_devCommands[2048];
static size_t g_devCommandPos;
static Uint32 g_devNextPoll;
static Uint32 g_devNextCommand;
static int g_devReleaseX = -1, g_devReleaseY, g_devReleaseButton;
/* "fake FIELD VALUE MS": simulated controller input for self-tests. */
static float g_fakeValue[12];
static Uint32 g_fakeUntil[12];
static const char *const g_fakeNames[12] = {
    "lsx", "lsy", "rsx", "rsy", "rt", "lt", "rg", "lg", "a", "b", "x", "y"
};

static void DevApplyFake(QuestXrControllers *c, Uint32 now)
{
    float *fields[12];
    int i;

    fields[0] = &c->leftStickX; fields[1] = &c->leftStickY;
    fields[2] = &c->rightStickX; fields[3] = &c->rightStickY;
    fields[4] = &c->rightTrigger; fields[5] = &c->leftTrigger;
    fields[6] = &c->rightGrip; fields[7] = &c->leftGrip;
    for (i = 0; i < 12; i++) {
        if (g_fakeUntil[i] == 0 || now >= g_fakeUntil[i]) {
            g_fakeUntil[i] = 0;
            continue;
        }
        if (i < 8)
            *fields[i] = g_fakeValue[i];
        else if (i == 8)
            c->a = g_fakeValue[i] > 0.5f;
        else if (i == 9)
            c->b = g_fakeValue[i] > 0.5f;
        else if (i == 10)
            c->x = g_fakeValue[i] > 0.5f;
        else
            c->y = g_fakeValue[i] > 0.5f;
    }
}

static void DevRunCommand(const char *line)
{
    int x, y;
    char word[64];

    if (sscanf(line, "click %d %d", &x, &y) == 2 ||
        sscanf(line, "rclick %d %d", &x, &y) == 2) {
        int right = line[0] == 'r';
        SdlQuestInjectMouse(x, y, 0, 0, 0);
        SdlQuestInjectMouse(x, y, !right, right, 2);
        g_devReleaseX = x;
        g_devReleaseY = y;
        g_devReleaseButton = right;
    } else if (sscanf(line, "move %d %d", &x, &y) == 2) {
        SdlQuestInjectMouse(x, y, 0, 0, 0);
    } else if (sscanf(line, "key %63s", word) == 1) {
        SDL_Keycode key = SDL_GetKeyFromName(word);
        if (key != SDLK_UNKNOWN)
            TapKey(key);
    } else if (sscanf(line, "fake %63s %d %d", word, &x, &y) == 3) {
        int i;
        for (i = 0; i < 12; i++) {
            if (strcmp(word, g_fakeNames[i]) == 0) {
                g_fakeValue[i] = x / 100.0f;
                g_fakeUntil[i] = SDL_GetTicks() + (Uint32)y;
            }
        }
    } else if (strncmp(line, "type ", 5) == 0) {
        const char *c;
        for (c = line + 5; *c; c++) {
            if (*c >= 'a' && *c <= 'z')
                QueueTypedKey((SDL_Keycode)*c);
            else if (*c >= 'A' && *c <= 'Z')
                QueueTypedKey((SDL_Keycode)(*c - 'A' + 'a'));
            else if (*c == '^')
                QueueTypedKey(SDLK_LSHIFT);
            else if (*c >= '0' && *c <= '9')
                QueueTypedKey((SDL_Keycode)*c);
            else if (*c == ' ')
                QueueTypedKey(SDLK_SPACE);
        }
    }
    printf("devcmd: %s\n", line);
}

static void DevServiceCommands(Uint32 now)
{
    if (g_devReleaseX >= 0) {
        SdlQuestInjectMouse(g_devReleaseX, g_devReleaseY, 0, 0, 1);
        g_devReleaseX = -1;
    }
    if (g_devCommands[g_devCommandPos] == 0 && now >= g_devNextPoll) {
        size_t size = 0;
        char *text;

        g_devNextPoll = now + 500;
        text = (char *)SDL_LoadFile("/sdcard/WingCommander2/vrcmd", &size);
        if (text != 0) {
            remove("/sdcard/WingCommander2/vrcmd");
            if (size >= sizeof(g_devCommands))
                size = sizeof(g_devCommands) - 1;
            memcpy(g_devCommands, text, size);
            g_devCommands[size] = 0;
            g_devCommandPos = 0;
            SDL_free(text);
        }
    }
    if (g_devCommands[g_devCommandPos] != 0 && now >= g_devNextCommand) {
        char line[256];
        size_t n = 0;

        while (g_devCommands[g_devCommandPos] != 0 &&
               g_devCommands[g_devCommandPos] != '\n' && n < sizeof(line) - 1)
            line[n++] = g_devCommands[g_devCommandPos++];
        line[n] = 0;
        while (g_devCommands[g_devCommandPos] == '\n' ||
               g_devCommands[g_devCommandPos] == '\r')
            g_devCommandPos++;
        if (g_devCommands[g_devCommandPos] == 0) {
            g_devCommands[0] = 0;
            g_devCommandPos = 0;
        }
        g_devNextCommand = now + 400;
        if (n > 0)
            DevRunCommand(line);
    }
}

void SdlQuestServiceInput(void)
{
    QuestXrControllers c;
    QuestXrPointer p;
    SDL_Joystick *pad;
    int phys[PHYS_COUNT];
    int edge[PHYS_COUNT];
    int piloting;
    int i;
    Uint32 now = SDL_GetTicks();

    DevServiceCommands(now);
    QuestXr_GetControllers(&c);
    DevApplyFake(&c, now);
    QuestXr_GetPointer(&p);
    piloting = SdlQuestIsPiloting();
    QuestXr_SetPiloting(piloting);

    phys[PHYS_RTRIG] = c.rightTrigger > 0.5f;
    phys[PHYS_LTRIG] = c.leftTrigger > 0.5f;
    phys[PHYS_RGRIP] = c.rightGrip > 0.5f;
    phys[PHYS_LGRIP] = c.leftGrip > 0.5f;
    phys[PHYS_A] = c.a;
    phys[PHYS_B] = c.b;
    phys[PHYS_X] = c.x;
    phys[PHYS_Y] = c.y;
    phys[PHYS_LCLICK] = c.leftStickClick;
    phys[PHYS_RCLICK] = c.rightStickClick;
    phys[PHYS_MENU] = c.menu;

    /* A button held across a flight <-> menu switch stays masked until it
     * is released, so one press never acts in both modes. */
    if (piloting != g_prevMode) {
        printf("controls: %s\n", piloting ? "flight" : "menus/laser");
        for (i = 0; i < PHYS_COUNT; i++)
            g_latched[i] = phys[i];
        g_prevMode = piloting;
        g_leftClickStart = 0;
    }
    for (i = 0; i < PHYS_COUNT; i++) {
        if (!phys[i])
            g_latched[i] = 0;
        if (g_latched[i])
            phys[i] = 0;
        edge[i] = phys[i] && !g_prevPhys[i];
    }

    /* Left stick click: tap = nav map, hold = recentre (XR thread). */
    if (edge[PHYS_LCLICK])
        g_leftClickStart = now;
    if (!phys[PHYS_LCLICK] && g_prevPhys[PHYS_LCLICK] &&
        g_leftClickStart != 0 && now - g_leftClickStart < 600)
        g_navPulse = 2;

    pad = g_questPadInstance >= 0 ? SDL_JoystickFromInstanceID(g_questPadInstance)
                                  : 0;
    if (pad != 0) {
        int mod = phys[PHYS_LGRIP];
        int b[SDL_CONTROLLER_BUTTON_MAX];

        SDL_zeroa(b);
        if (piloting) {
            SDL_JoystickSetVirtualAxis(pad, SDL_CONTROLLER_AXIS_LEFTX,
                                       AxisValue(ShapeStick(c.leftStickX)));
            SDL_JoystickSetVirtualAxis(pad, SDL_CONTROLLER_AXIS_LEFTY,
                                       AxisValue(ShapeStick(-c.leftStickY)));
            SDL_JoystickSetVirtualAxis(pad, SDL_CONTROLLER_AXIS_RIGHTX,
                                       AxisValue(ShapeStick(c.rightStickX)));
            SDL_JoystickSetVirtualAxis(pad, SDL_CONTROLLER_AXIS_RIGHTY,
                                       AxisValue(-c.rightStickY));
            b[SDL_CONTROLLER_BUTTON_A] = phys[PHYS_RTRIG];
            b[SDL_CONTROLLER_BUTTON_B] = phys[PHYS_LTRIG];
            b[SDL_CONTROLLER_BUTTON_X] = phys[PHYS_RGRIP];
            if (mod) {
                b[SDL_CONTROLLER_BUTTON_DPAD_RIGHT] = phys[PHYS_A];
                b[SDL_CONTROLLER_BUTTON_DPAD_DOWN] = phys[PHYS_B];
                b[SDL_CONTROLLER_BUTTON_DPAD_LEFT] = phys[PHYS_X];
                b[SDL_CONTROLLER_BUTTON_DPAD_UP] = phys[PHYS_Y];
            } else {
                b[SDL_CONTROLLER_BUTTON_Y] = phys[PHYS_A];
                b[SDL_CONTROLLER_BUTTON_RIGHTSHOULDER] = phys[PHYS_B];
                b[SDL_CONTROLLER_BUTTON_LEFTSHOULDER] = phys[PHYS_X];
                b[SDL_CONTROLLER_BUTTON_START] = phys[PHYS_Y];
            }
            b[SDL_CONTROLLER_BUTTON_BACK] = phys[PHYS_MENU];
            b[SDL_CONTROLLER_BUTTON_RIGHTSTICK] = phys[PHYS_RCLICK];
            if (g_navPulse > 0) {
                b[SDL_CONTROLLER_BUTTON_LEFTSTICK] = 1;
                g_navPulse--;
            }
        } else {
            int axis;
            for (axis = 0; axis < SDL_CONTROLLER_AXIS_MAX; axis++)
                SDL_JoystickSetVirtualAxis(pad, axis,
                                           axis >= SDL_CONTROLLER_AXIS_TRIGGERLEFT
                                               ? -32768 : 0);
            b[SDL_CONTROLLER_BUTTON_BACK] =
                (phys[PHYS_B] && !g_yesNoPrompt) || phys[PHYS_MENU];
            g_navPulse = 0;
        }
        for (i = 0; i < SDL_CONTROLLER_BUTTON_MAX; i++)
            SDL_JoystickSetVirtualButton(pad, i, (Uint8)b[i]);
    }

    {
        int keycode;
        while ((keycode = QuestXr_PopKeyboardKey()) != 0)
            QueueTypedKey((SDL_Keycode)keycode);
        if (g_typeHead != g_typeTail && now >= g_typeNext) {
            SDL_Keycode key = g_typeQueue[g_typeHead];

            g_typeHead = (g_typeHead + 1) % 256;
            if (key == SDLK_LSHIFT) {
                g_shiftOneShot = !g_shiftOneShot;
            } else {
                g_typeNext = now + 60;
                TapKey(key);
                if (key == SDLK_SPACE)
                    g_shiftOneShot = 1;
            }
            QuestXr_SetKeyboardShift(g_shiftOneShot);
        }
    }

    if (!piloting) {
        /* Yes/No prompts (load, save, quit): A = yes, B = no.  Elsewhere
         * A = Enter, B = Escape (via the pad's Back button). */
        if (g_yesNoPrompt) {
            if (edge[PHYS_A] || edge[PHYS_Y])
                TapKey(SDLK_y);
            if (edge[PHYS_B] || edge[PHYS_X])
                TapKey(SDLK_n);
        } else {
            if (edge[PHYS_A])
                TapKey(SDLK_RETURN);
            if (edge[PHYS_X])
                TapKey(SDLK_n);
            if (edge[PHYS_Y])
                TapKey(SDLK_y);
        }

        /* Laser pointer -> mouse. */
        if (p.onScreen) {
            int primary = p.primary && !g_latched[PHYS_RTRIG] &&
                          !g_latched[PHYS_LTRIG];
            int secondary = p.secondary;

            if (p.x != g_prevPointerX || p.y != g_prevPointerY)
                SdlQuestInjectMouse(p.x, p.y, g_prevPrimary,
                                    g_prevSecondary, 0);
            if (primary != g_prevPrimary || secondary != g_prevSecondary) {
                int down = (primary && !g_prevPrimary) ||
                           (secondary && !g_prevSecondary);
                SdlQuestInjectMouse(p.x, p.y, primary, secondary,
                                    down ? 2 : 1);
            }
            g_prevPointerX = p.x;
            g_prevPointerY = p.y;
            g_prevPrimary = primary;
            g_prevSecondary = secondary;
        } else if (g_prevPrimary || g_prevSecondary) {
            SdlQuestInjectMouse(g_prevPointerX, g_prevPointerY, 0, 0, 1);
            g_prevPrimary = 0;
            g_prevSecondary = 0;
        }
    } else {
        g_prevPrimary = 0;
        g_prevSecondary = 0;
    }

    if (piloting && (c.leftStickX != 0.0f || c.leftStickY != 0.0f ||
                     c.rightStickX != 0.0f || c.rightStickY != 0.0f ||
                     c.rightTrigger > 0.5f)) {
        static Uint32 lastLog;
        if (now - lastLog > 1000) {
            lastLog = now;
            printf("flight input: L %.2f %.2f R %.2f %.2f trig %.2f -> "
                   "yaw %d pitch %d roll %d dev %d\n",
                   c.leftStickX, c.leftStickY, c.rightStickX, c.rightStickY,
                   c.rightTrigger, (int)g_nYawInput_004931aa, (int)g_nPitchInput_004931a8,
                   (int)g_nRollInput_004931ac,
                   (int)g_nActiveInputDevice_005d1726);
        }
    }
    for (i = 0; i < PHYS_COUNT; i++)
        g_prevPhys[i] = phys[i];
}
