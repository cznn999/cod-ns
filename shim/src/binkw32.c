/* binkw32.dll (Bink video), which United Offensive plays its videos with. There is
 * no Bink decoder here yet: BinkOpen fails, so the game skips each video as if its
 * file were missing. The other functions are what the game may call around it
 * (setup, error text) and do nothing. Call of Duty does not import this DLL. */
#include "shim.h"

static uint32_t error_text;

SHIM(binkw32, _BinkOpen_8)
{
    static unsigned logged;
    if (logged++ < 20u)
        rt_log("BinkOpen(\"%s\"): videos are not played yet, skipped", gstr(ARG(0)) ? gstr(ARG(0)) : "");
    stdcall_ret(c, 0, 2);
}

SHIM(binkw32, _BinkGetError_0)
{
    if (!error_text)
        error_text = scratch_strdup("Bink videos are not supported by this port");
    stdcall_ret(c, error_text, 0);
}

/* Sound system setup: BinkSetSoundSystem(open function, parameter), and the open
 * functions passed to it. */
SHIM(binkw32, _BinkSetSoundSystem_8) { stdcall_ret(c, 1, 2); }
SHIM(binkw32, _BinkOpenMiles_4) { stdcall_ret(c, 0, 1); }
SHIM(binkw32, _BinkOpenDirectSound_4) { stdcall_ret(c, 0, 1); }
SHIM(binkw32, _BinkOpenWaveOut_4) { stdcall_ret(c, 0, 1); }
SHIM(binkw32, _BinkSetSoundTrack_8) { stdcall_ret(c, 0, 2); }
SHIM(binkw32, _BinkSetIOSize_4) { stdcall_ret(c, 0, 1); }
SHIM(binkw32, _BinkSetIO_4) { stdcall_ret(c, 0, 1); }
SHIM(binkw32, _BinkSetMemory_8) { stdcall_ret(c, 0, 2); }
SHIM(binkw32, _BinkSetError_4) { stdcall_ret(c, 0, 1); }

/* Calls on a Bink handle: never reached while BinkOpen fails; harmless if they are. */
SHIM(binkw32, _BinkClose_4) { stdcall_ret(c, 0, 1); }
SHIM(binkw32, _BinkDoFrame_4) { stdcall_ret(c, 0, 1); }
SHIM(binkw32, _BinkNextFrame_4) { stdcall_ret(c, 0, 1); }
SHIM(binkw32, _BinkWait_4) { stdcall_ret(c, 0, 1); }
SHIM(binkw32, _BinkCopyToBuffer_28) { stdcall_ret(c, 0, 7); }
SHIM(binkw32, _BinkCopyToBufferRect_44) { stdcall_ret(c, 0, 11); }
SHIM(binkw32, _BinkGoto_12) { stdcall_ret(c, 0, 3); }
SHIM(binkw32, _BinkPause_8) { stdcall_ret(c, 0, 2); }
SHIM(binkw32, _BinkSetVolume_12) { stdcall_ret(c, 0, 3); }
SHIM(binkw32, _BinkSetPan_12) { stdcall_ret(c, 0, 3); }
SHIM(binkw32, _BinkSetSoundOnOff_8) { stdcall_ret(c, 0, 2); }
SHIM(binkw32, _BinkSetVideoOnOff_8) { stdcall_ret(c, 0, 2); }
SHIM(binkw32, _BinkService_4) { stdcall_ret(c, 0, 1); }
SHIM(binkw32, _BinkShouldSkip_4) { stdcall_ret(c, 0, 1); }
SHIM(binkw32, _BinkGetRects_8) { stdcall_ret(c, 0, 2); }
SHIM(binkw32, _BinkGetSummary_8) { stdcall_ret(c, 0, 2); }
SHIM(binkw32, _BinkGetRealtime_12) { stdcall_ret(c, 0, 3); }
SHIM(binkw32, _BinkGetKeyFrame_12) { stdcall_ret(c, 0, 3); }
SHIM(binkw32, _BinkSetFrameRate_8) { stdcall_ret(c, 0, 2); }
SHIM(binkw32, _BinkSetSimulate_4) { stdcall_ret(c, 0, 1); }
SHIM(binkw32, _BinkDX8SurfaceType_4) { stdcall_ret(c, 0, 1); }
SHIM(binkw32, _BinkDDSurfaceType_4) { stdcall_ret(c, 0, 1); }
SHIM(binkw32, _BinkGetFrameBuffersInfo_8) { stdcall_ret(c, 0, 2); }
SHIM(binkw32, _BinkRegisterFrameBuffers_8) { stdcall_ret(c, 0, 2); }
