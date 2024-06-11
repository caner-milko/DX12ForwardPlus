#include "StaticMeshPipeline.h"

#include "ShaderManager.h"

namespace dxpg
{

struct ShaderMaterialInfo
{
	Vector4 Diffuse;
	int UseDiffuseTexture;
	int UseAlphaTexture;
};

struct ModelViewProjectionCB
{
	Matrix4x4 ModelViewProjection;
	Matrix4x4 NormalMatrix;
};

bool StaticMeshPipeline::Setup(ID3D12Device2* dev)
{
	// Create Root Signature
	RootSignatureBuilder builder{};

	std::vector< CD3DX12_ROOT_PARAMETER1> rootParams;
	builder.AddConstants("ModelViewProjectionCB", sizeof(ModelViewProjectionCB) / 4, { .ShaderRegister = 0, .Visibility = D3D12_SHADER_VISIBILITY_VERTEX });
	builder.AddDescriptorTable("VertexSRV", { { CD3DX12_DESCRIPTOR_RANGE1(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0) } }, D3D12_SHADER_VISIBILITY_VERTEX);
	builder.AddDescriptorTable("DiffuseTexture", { { CD3DX12_DESCRIPTOR_RANGE1(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3) } }, D3D12_SHADER_VISIBILITY_PIXEL);
	builder.AddDescriptorTable("AlphaTexture", { { CD3DX12_DESCRIPTOR_RANGE1(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 4) } }, D3D12_SHADER_VISIBILITY_PIXEL );
	builder.AddConstants("MaterialCB", sizeof(ShaderMaterialInfo) / 4, { .ShaderRegister = 1, .Visibility = D3D12_SHADER_VISIBILITY_PIXEL });
	builder.AddConstantBufferView("LightCB", { .ShaderRegister = 2, .Visibility = D3D12_SHADER_VISIBILITY_PIXEL, .DescFlags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE});

	D3D12_ROOT_SIGNATURE_FLAGS rootSignatureFlags =
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

	CD3DX12_STATIC_SAMPLER_DESC staticSampler(0);
	staticSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	staticSampler.MaxAnisotropy = 16;
	staticSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
	staticSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	builder.AddStaticSampler(staticSampler);

	RootSignature = builder.Build("StaticMeshRS", dev, rootSignatureFlags);

	struct PipelineStateStream : PipelineStateStreamBase
	{
		CD3DX12_PIPELINE_STATE_STREAM_INPUT_LAYOUT InputLayout;
		CD3DX12_PIPELINE_STATE_STREAM_PRIMITIVE_TOPOLOGY PrimitiveTopologyType;
		CD3DX12_PIPELINE_STATE_STREAM_VS VS;
		CD3DX12_PIPELINE_STATE_STREAM_PS PS;
		CD3DX12_PIPELINE_STATE_STREAM_DEPTH_STENCIL_FORMAT DSVFormat;
		CD3DX12_PIPELINE_STATE_STREAM_RENDER_TARGET_FORMATS RTVFormats;
		CD3DX12_PIPELINE_STATE_STREAM_RASTERIZER Rasterizer;
	} pipelineStateStream;

	D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
		{ "POSINDEX", 0, DXGI_FORMAT_R32_UINT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMALINDEX", 0, DXGI_FORMAT_R32_UINT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORDINDEX", 0, DXGI_FORMAT_R32_UINT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};

	pipelineStateStream.InputLayout = { inputLayout, _countof(inputLayout) };
	pipelineStateStream.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

	auto* vertexShader = ShaderManager::Get().CompileShader(L"Triangle.vs", DXPG_SHADERS_DIR L"Vertex/Triangle.vs.hlsl", ShaderType::Vertex);
	auto* pixelShader = ShaderManager::Get().CompileShader(L"Triangle.ps", DXPG_SHADERS_DIR L"Pixel/Triangle.ps.hlsl", ShaderType::Pixel);

	pipelineStateStream.VS = CD3DX12_SHADER_BYTECODE(vertexShader->Blob.Get());
	pipelineStateStream.PS = CD3DX12_SHADER_BYTECODE(pixelShader->Blob.Get());

	pipelineStateStream.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	D3D12_RT_FORMAT_ARRAY rtvFormats = {};
	rtvFormats.NumRenderTargets = 1;
	rtvFormats.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	pipelineStateStream.RTVFormats = rtvFormats;

	pipelineStateStream.Rasterizer = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);

	PipelineState = PipelineState::Create("StaticMeshPipeline", dev, pipelineStateStream, &RootSignature);

	LightBuffer = std::make_unique<DXBuffer>(DXBuffer::Create(dev, L"LightBuffer", sizeof(LightData), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS));

	return true;
}

bool StaticMeshPipeline::Run(ID3D12GraphicsCommandList2* cmd, ViewData const& viewData, SceneDataView const& scene, FrameContext& frameCtx)
{
	cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	cmd->SetPipelineState(PipelineState.DXPipelineState.Get());
	cmd->SetGraphicsRootSignature(RootSignature.DXSignature.Get());

	auto resBar = LightBuffer->Transition(D3D12_RESOURCE_STATE_COPY_DEST);
	cmd->ResourceBarrier(1, &resBar);
	// Update Light Buffer
	constexpr size_t paramCount = sizeof(LightData) / sizeof(UINT);
	D3D12_WRITEBUFFERIMMEDIATE_PARAMETER params[paramCount] = {};
	for (size_t i = 0; i < paramCount; i++)
	{
		params[i].Dest = LightBuffer->GPUAddress(i * sizeof(UINT));
		params[i].Value = reinterpret_cast<const UINT*>(&scene.Light)[i];
	}
	cmd->WriteBufferImmediate(paramCount, params, nullptr);
	resBar = LightBuffer->Transition(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
	cmd->ResourceBarrier(1, &resBar);
	cmd->SetGraphicsRootConstantBufferView(RootSignature.NameToParameterIndices["LightCB"], LightBuffer->GPUAddress());


	for (auto& group : scene.MeshGroups)
	{
		cmd->SetGraphicsRootDescriptorTable(RootSignature.NameToParameterIndices["VertexSRV"], group.VertexSRV);
	
		ComPtr<ID3D12DescriptorHeap> heap;
		for (int i = 0; i < group.Meshes.size(); i++)
		{
			auto& mesh = group.Meshes[i];
			cmd->IASetVertexBuffers(0, 1, &mesh.IndexBufferView);
			ModelViewProjectionCB mvp{};
			mvp.ModelViewProjection = XMMatrixMultiply(mesh.ModelMatrix, viewData.ViewProjection);
			mvp.NormalMatrix = DirectX::XMMatrixTranspose(DirectX::XMMatrixInverse(nullptr, mesh.ModelMatrix));
			cmd->SetGraphicsRoot32BitConstants(RootSignature.NameToParameterIndices["ModelViewProjectionCB"], sizeof(ModelViewProjectionCB) / sizeof(uint32_t), &mvp, 0);

			ShaderMaterialInfo matInfo{};

			matInfo.UseDiffuseTexture = mesh.UseDiffuseTexture;
			matInfo.Diffuse = { mesh.DiffuseColor.x, mesh.DiffuseColor.y, mesh.DiffuseColor.z, 1 };

			if (mesh.UseDiffuseTexture)
				cmd->SetGraphicsRootDescriptorTable(RootSignature.NameToParameterIndices["DiffuseTexture"], mesh.DiffuseSRV);

			if (mesh.UseAlphaTexture)
				cmd->SetGraphicsRootDescriptorTable(RootSignature.NameToParameterIndices["AlphaTexture"], mesh.AlphaSRV);

			cmd->SetGraphicsRoot32BitConstants(RootSignature.NameToParameterIndices["MaterialCB"], sizeof(ShaderMaterialInfo) / sizeof(uint32_t), &matInfo, 0);

			cmd->DrawInstanced(mesh.IndexCount, 1, 0, 0);
		}
	}
	return false;
}
}