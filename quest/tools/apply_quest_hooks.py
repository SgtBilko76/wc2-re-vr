#!/usr/bin/env python3
"""Apply the WC2_QUEST hooks to the wc2-re sources (idempotent).

Every change is wrapped in #ifdef WC2_QUEST, so the desktop SDL2 build and the
reconstructed Win32 build are unaffected.
"""
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]


def edit(rel, old, new, marker):
    path = ROOT / rel
    text = path.read_text()
    if marker in text:
        return
    if text.count(old) != 1:
        sys.exit(f"{rel}: anchor found {text.count(old)} times: {old[:60]!r}")
    path.write_text(text.replace(old, new, 1))
    print(f"patched {rel}")


# --- video.c: present into the headset, vsync from the XR thread -------------
edit("src/sdl/video.c",
     '#include "video_internal.h"\n',
     '#include "video_internal.h"\n'
     '#ifdef WC2_QUEST\n#include "../quest/quest_xr.h"\n#endif\n',
     '../quest/quest_xr.h')
edit("src/sdl/video.c",
     "    SdlShutdownVideo();\n    if (SdlUsingGlRenderer())\n"
     "        return SdlGlRendererInitialize(window);\n",
     "    SdlShutdownVideo();\n"
     "#ifdef WC2_QUEST\n"
     "    /* The XR thread owns presentation; the Android window stays unused. */\n"
     "    (void)window;\n    return 1;\n#endif\n"
     "    if (SdlUsingGlRenderer())\n"
     "        return SdlGlRendererInitialize(window);\n",
     "XR thread owns presentation")
edit("src/sdl/video.c",
     "        pixels = g_abEgaDitherPixels;\n        palette = g_abEgaDitherPalette;\n"
     "    }\n\n    if (SdlUsingGlRenderer()) {\n",
     "        pixels = g_abEgaDitherPixels;\n        palette = g_abEgaDitherPalette;\n"
     "    }\n\n"
     "#ifdef WC2_QUEST\n"
     "    if (pixels == 0 || palette == 0)\n        return 0;\n"
     "    if (!SdlQuestPresentStereo(pixels, palette))\n"
     "        QuestXr_SubmitIndexedFrame(pixels, palette);\n"
     "    if (restoreMouseCursor)\n        RestoreMouseCursorBackground();\n"
     "    return 1;\n#endif\n"
     "    if (SdlUsingGlRenderer()) {\n",
     "QuestXr_SubmitIndexedFrame(pixels, palette)")
edit("src/sdl/video.c",
     "    SDL_Rect dest;\n\n    if (SdlUsingGlRenderer()) {\n"
     "        SdlGlRendererWaitForVerticalBlank();\n",
     "    SDL_Rect dest;\n\n"
     "#ifdef WC2_QUEST\n    (void)dest;\n    QuestXr_WaitForDisplayTick();\n    return;\n#endif\n"
     "    if (SdlUsingGlRenderer()) {\n"
     "        SdlGlRendererWaitForVerticalBlank();\n",
     "QuestXr_WaitForDisplayTick();")
edit("src/sdl/video.c",
     "    int fullViewportCopy, unsigned char clearColour)\n{\n"
     "    if (SdlUsingGlRenderer()) {\n        SdlGlRendererBeginSpaceFrame(\n",
     "    int fullViewportCopy, unsigned char clearColour)\n{\n"
     "#ifdef WC2_QUEST\n"
     "    SdlQuestCaptureSpaceFrame(geometry, viewportMode, fullViewportCopy,\n"
     "                              clearColour);\n"
     "#endif\n"
     "    if (SdlUsingGlRenderer()) {\n        SdlGlRendererBeginSpaceFrame(\n",
     "SdlQuestCaptureSpaceFrame(geometry")
edit("src/sdl/video.c",
     "    SdlEndJoystickSpaceflight();\n    if (SdlUsingGlRenderer())\n"
     "        SdlGlRendererCancelSpaceFrame();\n",
     "    SdlEndJoystickSpaceflight();\n"
     "#ifdef WC2_QUEST\n    SdlQuestCancelStereo();\n#endif\n"
     "    if (SdlUsingGlRenderer())\n        SdlGlRendererCancelSpaceFrame();\n",
     "SdlQuestCancelStereo();")

# --- events.c: drain headset input every pump; laser -> mouse ---------------
edit("src/sdl/events.c",
     "    SdlServiceOriginFxMusic();\n    while (SDL_PollEvent(&event)) {\n",
     "    SdlServiceOriginFxMusic();\n"
     "#ifdef WC2_QUEST\n    SdlQuestServiceInput();\n#endif\n"
     "    while (SDL_PollEvent(&event)) {\n",
     "    SdlQuestServiceInput();\n")
edit("src/sdl/events.c",
     "void SdlStartEventPump(void)\n{\n",
     "#ifdef WC2_QUEST\n"
     "/* Laser pointer -> the same logical mouse path SdlHandleMouseEvent uses.\n"
     " * buttonEvent: 0 = motion, 2 = button down, 1 = button up. */\n"
     "void SdlQuestInjectMouse(int x, int y, int primaryButton,\n"
     "                         int secondaryButton, int buttonEvent)\n"
     "{\n"
     "    if (x < 0)\n        x = 0;\n    else if (x > 319)\n        x = 319;\n"
     "    if (y < 0)\n        y = 0;\n    else if (y > 199)\n        y = 199;\n"
     "    if (buttonEvent == 0) {\n"
     "        if (g_bSuppressNextMouseMove_005c843c != 0) {\n"
     "            g_bSuppressNextMouseMove_005c843c = 0;\n            return;\n        }\n"
     "        SdlQueueMouseMotion((unsigned short)x, (unsigned short)y,\n"
     "                            primaryButton, secondaryButton);\n"
     "    } else {\n"
     "        /* WC2 queues 1 for a press and 2 for a release. */\n"
     "        if (buttonEvent == 2)\n            g_nInputPressCount_0049c258++;\n"
     "        QueueInputEvent(buttonEvent == 2 ? 1 : 2, (unsigned short)x,\n"
     "                        (unsigned short)y, 0, primaryButton,\n"
     "                        secondaryButton, 0, 0, 0);\n"
     "    }\n"
     "    g_stHostMouseMessage_005d10d0.x = x;\n"
     "    g_stHostMouseMessage_005d10d0.y = y;\n"
     "    g_stHostMouseMessage_005d10d0.primaryButton = primaryButton;\n"
     "    g_stHostMouseMessage_005d10d0.secondaryButton = secondaryButton;\n"
     "}\n"
     "#endif\n\n"
     "void SdlStartEventPump(void)\n{\n",
     "void SdlQuestInjectMouse(")

# --- joystick.c: only the virtual Touch pad; piloting query ------------------
edit("src/sdl/joystick.c",
     "    instanceId = SDL_JoystickGetDeviceInstanceID(deviceIndex);\n"
     "    if (instanceId < 0 || SdlFindJoystick(instanceId) != -1)\n        return;\n",
     "    instanceId = SDL_JoystickGetDeviceInstanceID(deviceIndex);\n"
     "    if (instanceId < 0 || SdlFindJoystick(instanceId) != -1)\n        return;\n"
     "#ifdef WC2_QUEST\n"
     "    /* Android also lists the Touch controllers as gamepads; they never\n"
     "     * report input in an immersive session.  Use only the virtual pad. */\n"
     "    if (!SdlQuestIsQuestPad(deviceIndex))\n        return;\n#endif\n",
     "SdlQuestIsQuestPad(deviceIndex)")
edit("src/sdl/joystick.c",
     "void SdlEndJoystickSpaceflight(void)\n{\n",
     "#ifdef WC2_QUEST\n"
     "int SdlQuestIsPiloting(void)\n{\n"
     "    return g_bSdlJoystickSpaceflightActive &&\n"
     "        g_pfnInputPump_005c840c == get_player_input &&\n"
     "        g_nArcadeState_0049d75c == 0;\n}\n#endif\n\n"
     "void SdlEndJoystickSpaceflight(void)\n{\n",
     "int SdlQuestIsPiloting(void)")

# --- launcher.c: Quest start-up ------------------------------------------------
edit("src/sdl/launcher.c",
     '#include "slint/launcher_api.h"\n',
     '#include "slint/launcher_api.h"\n'
     '#ifdef WC2_QUEST\n#include "../quest/quest_xr.h"\n'
     'int SdlQuestStartup(int *argumentCount, char ***arguments);\n'
     'void SdlQuestCreatePad(void);\n#endif\n',
     '../quest/quest_xr.h')
edit("src/sdl/launcher.c",
     "    launcherGuiImplicit = argumentCount == 1;\n",
     "#ifdef WC2_QUEST\n"
     "    /* Logging, data directory and Quest default options. */\n"
     "    if (!SdlQuestStartup(&argumentCount, &arguments))\n        return 1;\n"
     "#endif\n"
     "    launcherGuiImplicit = argumentCount == 1;\n",
     "SdlQuestStartup(&argumentCount")
edit("src/sdl/launcher.c",
     "        return 1;\n    }\n\n    windowFlags = SDL_WINDOW_RESIZABLE;\n",
     "        return 1;\n    }\n"
     "#ifdef WC2_QUEST\n"
     "    if (!QuestXr_Start()) {\n"
     "        fprintf(stderr, \"OpenXR start failed\\n\");\n"
     "        SDL_Quit();\n        return 1;\n    }\n"
     "    SdlQuestCreatePad();\n"
     "#endif\n\n"
     "    windowFlags = SDL_WINDOW_RESIZABLE;\n",
     "    SdlQuestCreatePad();\n")
edit("src/sdl/launcher.c",
     "    DIBunInstall();\n    SdlShutdownJoysticks();\n    SDL_DestroyWindow(window);\n"
     "    SDL_Quit();\n",
     "    DIBunInstall();\n    SdlShutdownJoysticks();\n    SDL_DestroyWindow(window);\n"
     "#ifdef WC2_QUEST\n    QuestXr_Shutdown();\n#endif\n"
     "    SDL_Quit();\n",
     "    QuestXr_Shutdown();\n")

# --- compat.c: absolute paths (Android SDL reads relative ones from assets) --
edit("src/sdl/compat.c",
     "        strcpy(prefix, \"/\");\n    else\n        strcpy(prefix, \".\");\n",
     "        strcpy(prefix, \"/\");\n"
     "#ifdef WC2_QUEST\n"
     "    /* Android's SDL_RWFromFile/SDL_LoadFile read relative paths from the\n"
     "     * APK's assets, so resolved game paths must be absolute. */\n"
     "    else if (getcwd(prefix, sizeof(prefix)) == 0)\n"
     "        strcpy(prefix, \".\");\n"
     "#else\n"
     "    else\n        strcpy(prefix, \".\");\n"
     "#endif\n",
     "resolved game paths must be absolute")

# --- geom.c: VR keyboard for exactly the duration of a text prompt ------------
edit("src/geom.c",
     "/* Function start: 0x4597E3 */\nshort ReadTextInput(",
     "#ifdef WC2_QUEST\n"
     "static short ReadTextInputBody(char *destination, short maximumLength,\n"
     "                               volatile short mode,\n"
     "                               volatile short allowPathSeparators);\n"
     "\n"
     "short ReadTextInput(char *destination, short maximumLength,\n"
     "                    volatile short mode,\n"
     "                    volatile short allowPathSeparators)\n"
     "{\n"
     "    short result;\n"
     "\n"
     "    SdlQuestSetTextInput(1);\n"
     "    result = ReadTextInputBody(destination, maximumLength, mode,\n"
     "                               allowPathSeparators);\n"
     "    SdlQuestSetTextInput(0);\n"
     "    return result;\n"
     "}\n"
     "#define ReadTextInput ReadTextInputBody\n"
     "#endif\n\n"
     "/* Function start: 0x4597E3 */\nshort ReadTextInput(",
     "static short ReadTextInputBody(")

# --- eventmgr.c: objects drawn in 3D skip the software draw -----------------
edit("src/eventmgr.c",
     "                if (g_nNavPointerObject_004931b8 == obj) {\n"
     "#ifdef SDL_PORT\n",
     "#ifdef WC2_QUEST\n"
     "                /* Stars come from the VR star sphere; dust is 3D. */\n"
     "                if (g_nNavPointerObject_004931b8 != obj &&\n"
     "                    (objectClass == OBJECT_CLASS_STAR ||\n"
     "                     SdlQuestStereoOwnsObject(obj)) &&\n"
     "                    SdlQuestStereoOwnsObject(-2))\n"
     "                    break;\n"
     "#endif\n"
     "                if (g_nNavPointerObject_004931b8 == obj) {\n"
     "#ifdef SDL_PORT\n",
     "Stars come from the VR star sphere")
edit("src/eventmgr.c",
     "                if (g_apObjectShape_00493868[obj] != 0) {\n"
     "#ifdef SDL_PORT\n"
     "                    if (!SdlRecordSpaceSprite(\n",
     "#ifdef WC2_QUEST\n"
     "                /* Drawn in 3D by the VR renderer instead. */\n"
     "                if (SdlQuestStereoOwnsObject(obj))\n"
     "                    break;\n"
     "#endif\n"
     "                if (g_apObjectShape_00493868[obj] != 0) {\n"
     "#ifdef SDL_PORT\n"
     "                    if (!SdlRecordSpaceSprite(\n",
     "Drawn in 3D by the VR renderer instead.")

# --- sdl_port.h: prototypes ---------------------------------------------------
edit("include/sdl_port.h",
     "#endif /* SDL_PORT_H */",
     "#ifdef WC2_QUEST\n"
     "void SdlQuestInjectMouse(int x, int y, int primaryButton,\n"
     "                         int secondaryButton, int buttonEvent);\n"
     "int SdlQuestIsPiloting(void);\n"
     "void SdlQuestServiceInput(void);\n"
     "int SdlQuestIsQuestPad(int deviceIndex);\n"
     "void SdlQuestSetTextInput(int active);\n"
     "int SdlQuestTakeShift(void);\n"
     "struct ScreenViewportGeometry;\n"
     "void SdlQuestCaptureSpaceFrame(const struct ScreenViewportGeometry *geometry,\n"
     "                               int viewportMode, int fullViewportCopy,\n"
     "                               unsigned char clearColour);\n"
     "int SdlQuestStereoOwnsObject(int obj);\n"
     "void SdlQuestSetYesNoPrompt(int active);\n"
     "void SdlQuestCancelStereo(void);\n"
     "int SdlQuestPresentStereo(const unsigned char *pixels,\n"
     "                          const unsigned char *palette);\n"
     "#endif\n\n"
     "#endif /* SDL_PORT_H */",
     "void SdlQuestInjectMouse(int x")
print("done")
