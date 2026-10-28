/* Which game the build recompiles: Call of Duty (CoDSP.exe 1.3, the default) or
 * United Offensive (CoDUOSP.exe, CMake -DCOD_GAME=uo, which defines COD_GAME_UO).
 * Everything here differs between the two executables. */
#ifndef COD_GAME_H
#define COD_GAME_H

#if defined(COD_GAME_UO)

#define COD_EXE_NAME "CoDUOSP.exe"
/* cls.keyCatchers and cls.state (tools/find_uo_addresses.py). */
#define COD_KEY_CATCHERS 0x04842104u
#define COD_CLS_STATE    0x04842100u
/* CoDUOSP.exe's image ends at 0x0492A000: the game DLLs follow it (bases in
 * CMakeLists.txt), then the thunks, the stack and the heap. */
#define COD_IMAGE_END    0x0492A000u
#define COD_THUNK_BASE   0x05200000u
#define COD_STACK_BASE   0x05300000u
#define COD_HEAP_BASE    0x06000000u
/* Files of this build next to the Call of Duty ones on the Switch's SD card
 * (args_uo.txt, env_uo.txt, cod1_uo.cfg, cod1_uo.log ...). */
#define COD_SW_TAG "_uo"

#else

#define COD_EXE_NAME "CoDSP.exe"
#define COD_KEY_CATCHERS 0x01439264u
#define COD_CLS_STATE    0x01439260u
#define COD_IMAGE_END    0x01864000u
#define COD_THUNK_BASE   0x02000000u
#define COD_STACK_BASE   0x02100000u
#define COD_HEAP_BASE    0x03000000u
#define COD_SW_TAG ""

#endif

#endif
