// ============================================================================
//  xbox_input.cpp  --  Xbox controller -> Dreamcast maple input.
//
//  Polls the OG Xbox gamepad(s) via XDK XInput and writes Flycast's frontend
//  input interface (mapleInputState[port]) each frame. The maple controller
//  device reads that to drive the emulated DC pad.
//
//  DC kcode is active-LOW (~0 = nothing; clear a bit to "press"). Analog: the
//  triggers go in halfAxes (u16), the left stick in fullAxes (int16).
//
//  RXDK XInput pattern cribbed from the zelda3 Xbox port (proven on this SDK):
//  XInitDevices -> XGetDeviceChanges -> XInputOpen -> XInputGetState. Face
//  buttons A/B/X/Y and the triggers are ANALOG (bAnalogButtons, 0..255).
// ============================================================================
#include "types.h"
#include "hw/maple/maple_cfg.h"   // mapleInputState[], PJTI_*, PJAI_*
#include "input/gamepad.h"        // DC_BTN_*, DC_DPAD_*
#include <xtl.h>

#define NUM_PADS    4
#define ABTN_THRESH 30            // analog-button press threshold (0..255)

static HANDLE s_pads[NUM_PADS];

void xbox_InitInput()
{
    XDEVICE_PREALLOC_TYPE prealloc[] = { { XDEVICE_TYPE_GAMEPAD, NUM_PADS } };
    XInitDevices(1, prealloc);
    Sleep(50);                    // let the device layer enumerate

    DWORD ins = 0, rem = 0;
    XGetDeviceChanges(XDEVICE_TYPE_GAMEPAD, &ins, &rem);
    for (DWORD i = 0; i < NUM_PADS; i++)
        if (ins & (1u << i))
            s_pads[i] = XInputOpen(XDEVICE_TYPE_GAMEPAD, i, XDEVICE_NO_SLOT, NULL);

    OutputDebugStringA("FLYCAST: xbox_InitInput done\n");
}

void xbox_PollInput()
{
    // Hot-plug: open/close pads as they come and go.
    DWORD ins = 0, rem = 0;
    if (XGetDeviceChanges(XDEVICE_TYPE_GAMEPAD, &ins, &rem))
    {
        for (DWORD i = 0; i < NUM_PADS; i++)
        {
            if ((rem & (1u << i)) && s_pads[i]) { XInputClose(s_pads[i]); s_pads[i] = NULL; }
            if (ins & (1u << i))                  s_pads[i] = XInputOpen(XDEVICE_TYPE_GAMEPAD, i, XDEVICE_NO_SLOT, NULL);
        }
    }

    for (int port = 0; port < NUM_PADS; port++)
    {
        MapleInputState& m = mapleInputState[port];
        u32     kcode = ~0u;
        u16     lt = 0, rt = 0;
        int16_t lx = 0, ly = 0;

        if (s_pads[port])
        {
            XINPUT_STATE xs;
            ZeroMemory(&xs, sizeof(xs));
            if (XInputGetState(s_pads[port], &xs) == ERROR_SUCCESS)
            {
                const WORD  b  = xs.Gamepad.wButtons;
                const BYTE* ab = xs.Gamepad.bAnalogButtons;

                if (b & XINPUT_GAMEPAD_DPAD_UP)    kcode &= ~DC_DPAD_UP;
                if (b & XINPUT_GAMEPAD_DPAD_DOWN)  kcode &= ~DC_DPAD_DOWN;
                if (b & XINPUT_GAMEPAD_DPAD_LEFT)  kcode &= ~DC_DPAD_LEFT;
                if (b & XINPUT_GAMEPAD_DPAD_RIGHT) kcode &= ~DC_DPAD_RIGHT;
                if (b & XINPUT_GAMEPAD_START)      kcode &= ~DC_BTN_START;

                // Xbox A/B/X/Y -> DC A/B/X/Y (DC has no separate "white/black").
                if (ab[XINPUT_GAMEPAD_A] > ABTN_THRESH) kcode &= ~DC_BTN_A;
                if (ab[XINPUT_GAMEPAD_B] > ABTN_THRESH) kcode &= ~DC_BTN_B;
                if (ab[XINPUT_GAMEPAD_X] > ABTN_THRESH) kcode &= ~DC_BTN_X;
                if (ab[XINPUT_GAMEPAD_Y] > ABTN_THRESH) kcode &= ~DC_BTN_Y;

                // Analog triggers: 0..255 -> 0..65535.
                lt = (u16)(ab[XINPUT_GAMEPAD_LEFT_TRIGGER]  * 257);
                rt = (u16)(ab[XINPUT_GAMEPAD_RIGHT_TRIGGER] * 257);

                // Left stick -> DC analog stick. Y inverted (Xbox up=+, DC up=-).
                lx = xs.Gamepad.sThumbLX;
                ly = (int16_t)(-(int)xs.Gamepad.sThumbLY);
            }
        }

        m.kcode            = kcode;
        m.halfAxes[PJTI_L] = lt;
        m.halfAxes[PJTI_R] = rt;
        m.fullAxes[PJAI_X1] = lx;
        m.fullAxes[PJAI_Y1] = ly;
    }
}
