#pragma once

// LatteConst.h pulls in LatteReg.h, which supplies Latte::E_HWTILEMODE as well as
// LatteConst::ShaderType. Every previous user of this header included it from a translation unit
// that already had those, so the header was never self-contained; a JNI bridge that includes it
// cold is the first thing to notice.
#include "Cafe/HW/Latte/Core/LatteConst.h"

void LatteAsyncCommands_queueForceTextureReadback(MPTR physAddr, MPTR mipAddr, uint32 swizzle, sint32 format, sint32 width, sint32 height, sint32 depth, uint32 pitch, uint32 slice, sint32 dim, Latte::E_HWTILEMODE tilemode, sint32 aa, sint32 level);
void LatteAsyncCommands_queueDeleteShader(uint64 shaderBaseHash, uint64 shaderAuxHash, LatteConst::ShaderType shaderType);
void LatteAsyncCommands_queueReloadTextures();
void LatteAsyncCommands_waitUntilAllProcessed();

void LatteAsyncCommands_checkAndExecute();