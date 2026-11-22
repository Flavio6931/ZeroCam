#pragma once
#include <cstdint>

namespace zerocam {

class D3D11Context;

/// Ejecuta en unidad de compilación aislada (sin conflicto icodecapi.h/strmif.h) las pruebas de ZeroCamDShowTest.dll.
void RunDShowSelfTests(
    const wchar_t* exePath,
    D3D11Context& d3dCtx,
    bool& outMetaOk,
    bool& outYuy2Ok,
    bool& outNv12StandbyOk);

} // namespace zerocam
