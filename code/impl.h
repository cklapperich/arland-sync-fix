#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include "log.h"

namespace atfix {

extern Log log;

void hookDevice(ID3D11Device* pDevice);
void hookContext(ID3D11DeviceContext* pContext);

}
