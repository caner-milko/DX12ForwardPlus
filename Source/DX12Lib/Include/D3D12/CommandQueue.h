#pragma once
#include "D3D12Common.h"
#include "D3D12/CommandList.h"
#include "D3D12/Fence.h"
namespace dfr::d3d12
{

struct CommandQueue : public DeviceChild
{
	struct CommandQueueCreateInfo
	{
		D3D12_COMMAND_QUEUE_DESC Desc = {};
		uint32_t CommandListCount = 64;
	};
	CommandQueue(Device* dev);

	bool Init(CommandQueueCreateInfo createInfo);

	void Flush();
	CommandList* BeginCommandList();
	void ExecuteCommandList(CommandList& cmdList);


	std::vector<ru<CommandList>> CommandLists;
	ComPtr<ID3D12CommandQueue> DxCommandQueue;
	size_t NextCommandListIndex = 0;
	Fence QueueFence;
};

};
