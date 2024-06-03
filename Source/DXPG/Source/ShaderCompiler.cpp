#include "ShaderCompiler.h"

#include <unordered_map>
#include "DXPGCommon.h"

namespace dxpg::dx12
{
std::unique_ptr<ShaderCompiler> ShaderCompiler::Create()
{
	auto compiler = new ShaderCompiler();
	ThrowIfFailed(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&compiler->Utils)));
	ThrowIfFailed(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler->Compiler)));
	ThrowIfFailed(compiler->Utils->CreateDefaultIncludeHandler(&compiler->IncludeHandler));

	return std::unique_ptr<ShaderCompiler>(compiler);
}
std::unique_ptr<Shader> ShaderCompiler::CompileShader(std::wstring_view name, std::wstring_view shaderPath, ShaderType type, std::wstring_view entryPoint, std::span<std::wstring_view> includeFolders)
{
	LPCWSTR shaderType = nullptr;
	switch(type)
	{
	case ShaderType::Vertex:
		shaderType = L"vs_6_0";
		break;
	case ShaderType::Pixel:
		shaderType = L"ps_6_0";
		break;
	case ShaderType::Compute:
		shaderType = L"cs_6_0";
		break;
	}

	std::vector<LPCWSTR> compilationArgs =
	{
		name.data(),                  // Optional shader source file name for error reporting and for PIX shader source view.  
		L"-E", entryPoint.data(),              // Entry point.
		L"-T", shaderType,            // Target.
		DXC_ARG_WARNINGS_ARE_ERRORS,
		DXC_ARG_ALL_RESOURCES_BOUND,
	};
	
	for (auto& includeFolder : includeFolders)
	{
		compilationArgs.push_back(L"-I");
		compilationArgs.push_back(includeFolder.data());
	}

	if constexpr (_DEBUG)
		compilationArgs.push_back(DXC_ARG_DEBUG);
	else
		compilationArgs.push_back(DXC_ARG_OPTIMIZATION_LEVEL3);

	ComPtr<IDxcBlobEncoding> sourceEncoded;
	Utils->LoadFile(shaderPath.data(), nullptr, &sourceEncoded);
	if (!sourceEncoded)
	{
		wprintf(L"Failed to load shader file %s\n", shaderPath.data());
		return nullptr;
	}

	DxcBuffer Source;
	Source.Ptr = sourceEncoded->GetBufferPointer();
	Source.Size = sourceEncoded->GetBufferSize();
	Source.Encoding = DXC_CP_ACP;

	ComPtr<IDxcResult> results;
	Compiler->Compile(&Source, compilationArgs.data(), compilationArgs.size(), IncludeHandler.Get(), IID_PPV_ARGS(&results));

	ComPtr<IDxcBlobUtf8> pErrors = nullptr;
	results->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&pErrors), nullptr);
	if (pErrors != nullptr && pErrors->GetStringLength() != 0)
		wprintf(L"Warnings and Errors:\n%S\n", pErrors->GetStringPointer());

	HRESULT hrStatus;
	results->GetStatus(&hrStatus);
	if (FAILED(hrStatus))
	{
		wprintf(L"Compilation Failed\n");
		return nullptr;
	}

	ComPtr<ID3DBlob> compiledBlob;

	results->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&compiledBlob), 0);

	auto shader = std::make_unique<Shader>();

	shader->EntryPoint = entryPoint;
	shader->Path = shaderPath;
	shader->Name = name;
	shader->Blob = compiledBlob;
    return shader;
}

}