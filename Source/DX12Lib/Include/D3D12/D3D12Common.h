#pragma once

#include "Common.h"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl.h>
#include "d3dx12.h"

namespace dfr::d3d12
{
	
struct Device;
struct CommandQueue;
struct CommandList;

struct DeviceChild
{
	DeviceChild(Device* dev) : Dev(dev) {}

	Device* Dev;
};

};