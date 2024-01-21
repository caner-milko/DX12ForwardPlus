#include <DeferredRenderer.h>

#include <Application.h>
#include <D3D12/Device.h>
#include <D3D12/CommandQueue.h>
#include <Common.h>
#include <Window.h>

#include <wrl.h>
using namespace Microsoft::WRL;

#include <d3dx12.h>
#include <d3dcompiler.h>

#include <algorithm> // For std::min and std::max.

#include <AssetLoader.h>

using namespace DirectX;


// Clamp a value between a min and max range.
template<typename T>
constexpr const T& clamp(const T& val, const T& min, const T& max)
{
	return val < min ? min : val > max ? max : val;
}

namespace dfr 
{
DeferredRenderer::DeferredRenderer(const std::wstring& name, int width, int height, bool vSync)
	: super(name, width, height, vSync)
	, m_ScissorRect(CD3DX12_RECT(0, 0, LONG_MAX, LONG_MAX))
	, m_Viewport(CD3DX12_VIEWPORT(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)))
	, m_FoV(45.0)
	, m_ContentLoaded(false)
{
}

void DeferredRenderer::UpdateBufferResource(
	ComPtr<ID3D12GraphicsCommandList2> commandList,
	ID3D12Resource** pDestinationResource,
	ID3D12Resource** pIntermediateResource,
	size_t bufferSize, const void* bufferData,
	D3D12_RESOURCE_FLAGS flags)
{
	auto device = GDxDev->DxDevice;

	// Create a committed resource for the GPU resource in a default heap.
	CD3DX12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE_DEFAULT);
	CD3DX12_RESOURCE_DESC buf = CD3DX12_RESOURCE_DESC::Buffer(bufferSize, flags);
	ThrowIfFailed(device->CreateCommittedResource(
		&heapProperties,
		D3D12_HEAP_FLAG_NONE,
		&buf,
		D3D12_RESOURCE_STATE_COPY_DEST,
		nullptr,
		IID_PPV_ARGS(pDestinationResource)));

	// Create an committed resource for the upload.
	if (bufferData)
	{
		heapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
		buf = CD3DX12_RESOURCE_DESC::Buffer(bufferSize);
		ThrowIfFailed(device->CreateCommittedResource(
			&heapProperties,
			D3D12_HEAP_FLAG_NONE,
			&buf,
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(pIntermediateResource)));

		D3D12_SUBRESOURCE_DATA subresourceData = {};
		subresourceData.pData = bufferData;
		subresourceData.RowPitch = bufferSize;
		subresourceData.SlicePitch = subresourceData.RowPitch;

		UpdateSubresources(commandList.Get(),
			*pDestinationResource, *pIntermediateResource,
			0, 0, 1, &subresourceData);
	}
}



void DeferredRenderer::ResizeBuffers(int width, int height)
{
	if (m_ContentLoaded)
	{
		// Flush any GPU commands that might be referencing the depth buffer.    
		GDxDev->GetImmediateCommandQueue()->Flush();

		width = std::max(1, width);
		height = std::max(1, height);

		CreateGBufferTextures({width, height});
	}
}


bool DeferredRenderer::LoadContent()
{
	auto device = GDxDev->DxDevice;
	auto* commandQueue = GDxDev->GetImmediateCommandQueue();
	auto* cmdList = commandQueue->BeginCommandList();
	// Load default render shaders
	// Load shading shaders
	LoadShaders();
	// Create GBuffer Heaps
	CreateGBufferHeaps();
	// Create buffer for camera
	// Create buffer for lights
	CreateCamLightBuffers();
	// Load vertex and index buffers of meshes
	LoadMeshes(cmdList);
	// Create root signatures for gbuffer and shading passes
	CreateRootSignatures();
	// Create PSO for gbuffer pass and shading pass
	CreatePSOs();

	cmdList->Execute().Wait();

	m_ContentLoaded = true;

	// Create GBuffers(Albedo, normals, depth, final)
	CreateGBufferTextures({ GetClientWidth(), GetClientHeight() });
	
	return true;
}

void DeferredRenderer::LoadShaders()
{
	// Load the vertex shader.
	ThrowIfFailed(D3DReadFileToBlob(L"VertexShader.cso", &Shaders.MeshVertexShader));

	// Load the pixel shader.
	ThrowIfFailed(D3DReadFileToBlob(L"LightingShader.cso", &Shaders.MeshFragmentShader));
}

void DeferredRenderer::CreateGBufferHeaps()
{
	{
		// Create the descriptor heap for the depth-stencil view.
		D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
		dsvHeapDesc.NumDescriptors = 1;
		dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
		dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		ThrowIfFailed(GDxDev->DxDevice->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&GBuffers.DepthHeap)));
	}
	{
		// Create the descriptor heap for the depth-stencil view.
		D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
		rtvHeapDesc.NumDescriptors = 2;
		rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE::D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAGS::D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		ThrowIfFailed(GDxDev->DxDevice->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&GBuffers.Heap)));
	}
}

void DeferredRenderer::CreateGBufferTextures(XMINT2 size)
{
	auto device = GDxDev->DxDevice;        
	GDxDev->GetImmediateCommandQueue()->Flush();

	CD3DX12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE_DEFAULT);
	size.x = std::max(1, size.x);
	size.y = std::max(1, size.y);
	{
		D3D12_CLEAR_VALUE optimizedClearValue = {};
		optimizedClearValue.Format = DXGI_FORMAT_D32_FLOAT;
		optimizedClearValue.DepthStencil = { 1.0f, 0 };
		auto texDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_D32_FLOAT, size.x, size.y,
			1, 0, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
		ThrowIfFailed(device->CreateCommittedResource(
			&heapProperties,
			D3D12_HEAP_FLAG_NONE,
			&texDesc,
			D3D12_RESOURCE_STATE_DEPTH_WRITE,
			&optimizedClearValue,
			IID_PPV_ARGS(&GBuffers.Depth)
		));

		// Update the depth-stencil view.
		D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
		dsv.Format = DXGI_FORMAT_D32_FLOAT;
		dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
		dsv.Texture2D.MipSlice = 0;
		dsv.Flags = D3D12_DSV_FLAG_NONE;

		device->CreateDepthStencilView(GBuffers.Depth.Get(), &dsv,
			GBuffers.DepthHeap->GetCPUDescriptorHandleForHeapStart());
	}

	{
		DXGI_FORMAT albedoFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
		
		// Create an albedo texture.
		D3D12_CLEAR_VALUE optimizedClearValue = {};
		optimizedClearValue.Format = albedoFormat;
		optimizedClearValue.Color[0] = 0.f;
		optimizedClearValue.Color[1] = 0.f;
		optimizedClearValue.Color[2] = 0.f;
		optimizedClearValue.Color[3] = 0.f;
		auto texDesc = CD3DX12_RESOURCE_DESC::Tex2D(albedoFormat, size.x, size.y,
			1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		ThrowIfFailed(device->CreateCommittedResource(
			&heapProperties,
			D3D12_HEAP_FLAG_NONE,
			&texDesc,
			D3D12_RESOURCE_STATE_COMMON,
			&optimizedClearValue,
			IID_PPV_ARGS(&GBuffers.Albedo)
		));
		ThrowIfFailed(device->CreateCommittedResource(
			&heapProperties,
			D3D12_HEAP_FLAG_NONE,
			&texDesc,
			D3D12_RESOURCE_STATE_RENDER_TARGET,
			&optimizedClearValue,
			IID_PPV_ARGS(&GBuffers.Normal)
		));


		D3D12_RENDER_TARGET_VIEW_DESC dsc = {};
		dsc.Format = albedoFormat;
		dsc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
		dsc.Texture2D.MipSlice = 0;
		GBuffers.HeapIncrementSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE::D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		CD3DX12_CPU_DESCRIPTOR_HANDLE handle(GBuffers.Heap->GetCPUDescriptorHandleForHeapStart());
		device->CreateRenderTargetView(GBuffers.Albedo.Get(), &dsc, handle);
		handle.Offset(GBuffers.HeapIncrementSize);
		device->CreateRenderTargetView(GBuffers.Normal.Get(), &dsc, handle);
	}

}

void DeferredRenderer::CreateCamLightBuffers()
{
}

void DeferredRenderer::LoadMeshes(d3d12::CommandList* cmdList)
{
	rc<Mesh> mesh = std::move(loadObj(MeshPath));
	// Upload vertex buffer data.
	cmdList->AddDependency(mesh);

	m_GPUMesh.IndexCount = mesh->Indices.size();

	UpdateBufferResourceFromVec(cmdList, &m_GPUMesh.Positions, mesh->Positions);

	UpdateBufferResourceFromVec(cmdList, &m_GPUMesh.TexCoords, mesh->TexCoords);

	UpdateBufferResourceFromVec(cmdList, &m_GPUMesh.Normals, mesh->Normals);

	// Upload index buffer data.
	UpdateBufferResourceFromVec(cmdList, &m_GPUMesh.VertexIndices, mesh->Indices);

	std::vector<uint32_t> indices;

	for (int i = 0; i < mesh->Indices.size(); i++)
	{
		indices.push_back(i);
	}

	//Upload indices
	UpdateBufferResourceFromVec(cmdList, &m_GPUMesh.IndexBuffer, indices);

	m_GPUMesh.IndexBufferView.BufferLocation = m_GPUMesh.IndexBuffer->GetGPUVirtualAddress();
	m_GPUMesh.IndexBufferView.Format = DXGI_FORMAT_R32_UINT;
	m_GPUMesh.IndexBufferView.SizeInBytes = mesh->Indices.size() * sizeof(uint32_t);

	D3D12_DESCRIPTOR_HEAP_DESC desc;
	desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAGS::D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	desc.NodeMask = 0;
	desc.NumDescriptors = 4;
	desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE::D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	GDxDev->DxDevice->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_GPUMesh.Heap));
	m_GPUMesh.HeapSize = GDxDev->DxDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);



	D3D12_SHADER_RESOURCE_VIEW_DESC positionSRVDesc = {};
	positionSRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	positionSRVDesc.Format = DXGI_FORMAT_UNKNOWN;
	positionSRVDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
	positionSRVDesc.Buffer.FirstElement = 0;
	positionSRVDesc.Buffer.NumElements = mesh->Positions.size()/3;
	positionSRVDesc.Buffer.StructureByteStride = sizeof(float)*3;
	positionSRVDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

	D3D12_SHADER_RESOURCE_VIEW_DESC texCoordsSRVDesc = {};
	texCoordsSRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	texCoordsSRVDesc.Format = DXGI_FORMAT_UNKNOWN;
	texCoordsSRVDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
	texCoordsSRVDesc.Buffer.FirstElement = 0;
	texCoordsSRVDesc.Buffer.NumElements = mesh->TexCoords.size()/2;
	texCoordsSRVDesc.Buffer.StructureByteStride = sizeof(float) * 2;
	texCoordsSRVDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

	D3D12_SHADER_RESOURCE_VIEW_DESC normalSRVDesc = {};
	normalSRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	normalSRVDesc.Format = DXGI_FORMAT_UNKNOWN;
	normalSRVDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
	normalSRVDesc.Buffer.FirstElement = 0;
	normalSRVDesc.Buffer.NumElements = mesh->Normals.size()/3;
	normalSRVDesc.Buffer.StructureByteStride = sizeof(float) * 3;
	normalSRVDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

	D3D12_SHADER_RESOURCE_VIEW_DESC indicesSRVDesc = {};
	indicesSRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	indicesSRVDesc.Format = DXGI_FORMAT_UNKNOWN;
	indicesSRVDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
	indicesSRVDesc.Buffer.FirstElement = 0;
	indicesSRVDesc.Buffer.NumElements = mesh->Indices.size();
	indicesSRVDesc.Buffer.StructureByteStride = sizeof(decltype(mesh->Indices)::value_type);
	indicesSRVDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

	CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandles[4];
	srvHandles[0] = CD3DX12_CPU_DESCRIPTOR_HANDLE(m_GPUMesh.Heap->GetCPUDescriptorHandleForHeapStart(), 0, m_GPUMesh.HeapSize);
	srvHandles[1] = CD3DX12_CPU_DESCRIPTOR_HANDLE(m_GPUMesh.Heap->GetCPUDescriptorHandleForHeapStart(), 1, m_GPUMesh.HeapSize);
	srvHandles[2] = CD3DX12_CPU_DESCRIPTOR_HANDLE(m_GPUMesh.Heap->GetCPUDescriptorHandleForHeapStart(), 2, m_GPUMesh.HeapSize);
	srvHandles[3] = CD3DX12_CPU_DESCRIPTOR_HANDLE(m_GPUMesh.Heap->GetCPUDescriptorHandleForHeapStart(), 3, m_GPUMesh.HeapSize);
	GDxDev->DxDevice->CreateShaderResourceView(m_GPUMesh.VertexIndices.Get(), &indicesSRVDesc, srvHandles[0]);
	GDxDev->DxDevice->CreateShaderResourceView(m_GPUMesh.Positions.Get(), &positionSRVDesc, srvHandles[1]);
	GDxDev->DxDevice->CreateShaderResourceView(m_GPUMesh.TexCoords.Get(), &texCoordsSRVDesc, srvHandles[2]);
	GDxDev->DxDevice->CreateShaderResourceView(m_GPUMesh.Normals.Get(), &normalSRVDesc, srvHandles[3]);
	//GDxDev->DxDevice->CreateShaderResourceView(m_GPUMesh.TexCoords.Get(), &srvDesc, srvHandles[0]);
}

void DeferredRenderer::CreateRootSignatures()
{
	auto device = GDxDev->DxDevice;

	// Create a root signature.
	D3D12_FEATURE_DATA_ROOT_SIGNATURE featureData = {};
	featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
	if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &featureData, sizeof(featureData))))
	{
		featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
	}

	// Allow input layout and deny unnecessary access to certain pipeline stages.
	D3D12_ROOT_SIGNATURE_FLAGS rootSignatureFlags =
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS;

	CD3DX12_DESCRIPTOR_RANGE1 DescRange{};

	DescRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0);

	// A single 32-bit constant root parameter that is used by the vertex shader.
	CD3DX12_ROOT_PARAMETER1 rootParameters[2];
	rootParameters[0].InitAsConstants(sizeof(XMMATRIX) / 4, 0, 0, D3D12_SHADER_VISIBILITY_VERTEX);
	rootParameters[1].InitAsDescriptorTable(1, &DescRange);
	CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDescription;
	rootSignatureDescription.Init_1_1(_countof(rootParameters), rootParameters, 0, nullptr, rootSignatureFlags);

	// Serialize the root signature.
	ComPtr<ID3DBlob> rootSignatureBlob;
	ComPtr<ID3DBlob> errorBlob;
	ThrowIfFailed(D3DX12SerializeVersionedRootSignature(&rootSignatureDescription,
		featureData.HighestVersion, &rootSignatureBlob, &errorBlob));
	// Create the root signature.
	ThrowIfFailed(device->CreateRootSignature(0, rootSignatureBlob->GetBufferPointer(),
		rootSignatureBlob->GetBufferSize(), IID_PPV_ARGS(&m_RootSignature)));
}

void DeferredRenderer::CreatePSOs()
{
	auto device = GDxDev->DxDevice;
	// Create the vertex input layout
	auto desc = CD3DX12_RASTERIZER_DESC(CD3DX12_DEFAULT{});

	struct PipelineStateStream
	{
		CD3DX12_PIPELINE_STATE_STREAM_ROOT_SIGNATURE pRootSignature;
		CD3DX12_PIPELINE_STATE_STREAM_PRIMITIVE_TOPOLOGY PrimitiveTopologyType;
		CD3DX12_PIPELINE_STATE_STREAM_VS VS;
		CD3DX12_PIPELINE_STATE_STREAM_PS PS;
		CD3DX12_PIPELINE_STATE_STREAM_DEPTH_STENCIL_FORMAT DSVFormat;
		CD3DX12_PIPELINE_STATE_STREAM_RENDER_TARGET_FORMATS RTVFormats;
		CD3DX12_PIPELINE_STATE_STREAM_RASTERIZER Rasterizer;
	} pipelineStateStream;
	pipelineStateStream.Rasterizer = desc;

	D3D12_RT_FORMAT_ARRAY rtvFormats = {};
	rtvFormats.NumRenderTargets = 2;
	rtvFormats.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	rtvFormats.RTFormats[1] = DXGI_FORMAT_R8G8B8A8_UNORM;

	pipelineStateStream.pRootSignature = m_RootSignature.Get();
	pipelineStateStream.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pipelineStateStream.VS = CD3DX12_SHADER_BYTECODE(Shaders.MeshVertexShader.Get());
	pipelineStateStream.PS = CD3DX12_SHADER_BYTECODE(Shaders.MeshFragmentShader.Get());
	pipelineStateStream.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	pipelineStateStream.RTVFormats = rtvFormats;

	D3D12_PIPELINE_STATE_STREAM_DESC pipelineStateStreamDesc = {
		sizeof(PipelineStateStream), &pipelineStateStream
	};
	ThrowIfFailed(device->CreatePipelineState(&pipelineStateStreamDesc, IID_PPV_ARGS(&m_PipelineState)));
}

void DeferredRenderer::OnResize(ResizeEventArgs& e)
{
	if (e.Width != GetClientWidth() || e.Height != GetClientHeight())
	{
		super::OnResize(e);

		m_Viewport = CD3DX12_VIEWPORT(0.0f, 0.0f,
			static_cast<float>(e.Width), static_cast<float>(e.Height));

		ResizeBuffers(e.Width, e.Height);
	}
}

void DeferredRenderer::UnloadContent()
{
	m_ContentLoaded = false;
}

void DeferredRenderer::OnUpdate(UpdateEventArgs& e)
{
	static uint64_t frameCount = 0;
	static double totalTime = 0.0;

	super::OnUpdate(e);

	totalTime += e.ElapsedTime;
	frameCount++;

	if (totalTime > 1.0)
	{
		double fps = frameCount / totalTime;

		char buffer[512];
		sprintf_s(buffer, "FPS: %f\n", fps);
		OutputDebugStringA(buffer);

		frameCount = 0;
		totalTime = 0.0;
	}

	// Update the model matrix.
	float angle = static_cast<float>(e.TotalTime * 90.0);
	const XMVECTOR rotationAxis = XMVectorSet(0, 1, 1, 0);
	m_ModelMatrix = XMMatrixRotationAxis(rotationAxis, XMConvertToRadians(angle));

	// Update the view matrix.
	const XMVECTOR eyePosition = XMVectorSet(0, 0, -10, 1);
	const XMVECTOR focusPoint = XMVectorSet(0, 0, 0, 1);
	const XMVECTOR upDirection = XMVectorSet(0, 1, 0, 0);
	m_ViewMatrix = XMMatrixLookAtLH(eyePosition, focusPoint, upDirection);

	// Update the projection matrix.
	float aspectRatio = GetClientWidth() / static_cast<float>(GetClientHeight());
	m_ProjectionMatrix = XMMatrixPerspectiveFovLH(XMConvertToRadians(m_FoV), aspectRatio, 0.1f, 100.0f);
}

// Transition a resource
void DeferredRenderer::TransitionResource(Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList2> commandList,
	Microsoft::WRL::ComPtr<ID3D12Resource> resource,
	D3D12_RESOURCE_STATES beforeState, D3D12_RESOURCE_STATES afterState)
{
	CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
		resource.Get(),
		beforeState, afterState);

	commandList->ResourceBarrier(1, &barrier);
}

// Clear a render target.
void DeferredRenderer::ClearRTV(Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList2> commandList,
	D3D12_CPU_DESCRIPTOR_HANDLE rtv, FLOAT* clearColor)
{
	commandList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
}

void DeferredRenderer::ClearDepth(Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList2> commandList,
	D3D12_CPU_DESCRIPTOR_HANDLE dsv, FLOAT depth)
{
	commandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, depth, 0, 0, nullptr);
}

void DeferredRenderer::OnRender(RenderEventArgs& e)
{
	super::OnRender(e);

	auto* commandQueue = GDxDev->GetImmediateCommandQueue();
	auto* dfrCommandList = commandQueue->BeginCommandList();
	auto commandList = dfrCommandList->DxCommandList;

	UINT currentBackBufferIndex = Window->GetCurrentBackBufferIndex();
	commandList->SetName((std::wstring(L"Present Cmd ") + std::to_wstring(currentBackBufferIndex)).c_str());
	auto dsv = GBuffers.DepthHeap->GetCPUDescriptorHandleForHeapStart();
	auto rtv = GBuffers.Heap->GetCPUDescriptorHandleForHeapStart();
	auto rtAlbedo = GBuffers.Albedo;
	auto rtNormal = GBuffers.Normal;
	// Clear the render targets.
	{
		TransitionResource(commandList, rtAlbedo,
			D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
		FLOAT clearColor[] = { 0.4f, 0.6f, 0.9f, 1.0f };
		CD3DX12_CPU_DESCRIPTOR_HANDLE rtHandle(rtv, 0, GBuffers.HeapIncrementSize);
		ClearRTV(commandList, rtHandle, clearColor);
		rtHandle.Offset(GBuffers.HeapIncrementSize);
		ClearRTV(commandList, rtHandle, clearColor);
		ClearDepth(commandList, dsv);
	}

	commandList->SetPipelineState(m_PipelineState.Get());
	commandList->SetGraphicsRootSignature(m_RootSignature.Get());

	commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	commandList->IASetIndexBuffer(&m_GPUMesh.IndexBufferView);

	commandList->RSSetViewports(1, &m_Viewport);
	commandList->RSSetScissorRects(1, &m_ScissorRect);

	commandList->OMSetRenderTargets(2, &rtv, TRUE, &dsv);

	// Update the MVP matrix
	XMMATRIX mvpMatrix = XMMatrixMultiply(m_ModelMatrix, m_ViewMatrix);
	mvpMatrix = XMMatrixMultiply(mvpMatrix, m_ProjectionMatrix);
	commandList->SetGraphicsRoot32BitConstants(0, sizeof(XMMATRIX) / 4, &mvpMatrix, 0);
	ID3D12DescriptorHeap* ppHeaps[] = { m_GPUMesh.Heap.Get(),};
	commandList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);
	CD3DX12_GPU_DESCRIPTOR_HANDLE srvHandle(m_GPUMesh.Heap->GetGPUDescriptorHandleForHeapStart(), 0, m_GPUMesh.HeapSize);
	commandList->SetGraphicsRootDescriptorTable(1, srvHandle);
	commandList->DrawIndexedInstanced(m_GPUMesh.IndexCount, 1, 0, 0, 0);

	// Present
	{
		TransitionResource(commandList, rtAlbedo,
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
		auto& backBuffer = Window->GetCurrentBackBuffer();
		backBuffer.LastCmdList = dfrCommandList;
		auto backBufferView = Window->GetCurrentRenderTargetView();

		TransitionResource(commandList, backBuffer.DxResource,
			D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);

		commandList->CopyResource(backBuffer.DxResource.Get(), rtAlbedo.Get());

		TransitionResource(commandList, backBuffer.DxResource,
			D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);

		dfrCommandList->Execute();

		currentBackBufferIndex = Window->Present();

		auto& oldBackBuffer = Window->GetCurrentBackBuffer();
		if (oldBackBuffer.LastCmdList)
			oldBackBuffer.LastCmdList->Wait();
		
		oldBackBuffer.LastCmdList = nullptr;
	}
}

void DeferredRenderer::OnKeyPressed(KeyEventArgs& e)
{
	super::OnKeyPressed(e);

	switch (e.Key)
	{
	case KeyCode::Escape:
		Application::Get().Quit(0);
		break;
	case KeyCode::Enter:
		if (e.Alt)
		{
	case KeyCode::F11:
		Window->ToggleFullscreen();
		break;
		}
	case KeyCode::V:
		Window->ToggleVSync();
		break;
	}
}

void DeferredRenderer::OnMouseWheel(MouseWheelEventArgs& e)
{
	m_FoV -= e.WheelDelta;
	m_FoV = clamp(m_FoV, 12.0f, 90.0f);

	char buffer[256];
	sprintf_s(buffer, "FoV: %f\n", m_FoV);
	OutputDebugStringA(buffer);
}
};