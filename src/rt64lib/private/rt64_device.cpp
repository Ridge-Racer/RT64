//
// RT64
//

#include <cassert>

#include <dwmapi.h>

#include "utf8conv/utf8conv.h"

#include "rt64_device.h"

#ifndef RT64_MINIMAL
#include "rt64_inspector.h"
#include "rt64_scene.h"
#include "rt64_texture.h"

#include "shaders/ComposePS.hlsl.h"
#include "shaders/ComposeVS.hlsl.h"
#include "shaders/Im3DPS.hlsl.h"
#include "shaders/Im3DVS.hlsl.h"
#include "shaders/Im3DGSPoints.hlsl.h"
#include "shaders/Im3DGSLines.hlsl.h"
#include "shaders/RasterPS.hlsl.h"
#include "shaders/RasterVS.hlsl.h"
#include "shaders/Shadow.hlsl.h"
#include "shaders/Surface.hlsl.h"
#include "shaders/Tracer.hlsl.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"
#endif

// Private

RT64::Device::Device(HWND hwnd) {
	createDXGIFactory();
	createRaytracingDevice();

#ifndef RT64_MINIMAL
	assert(hwnd != 0);
	this->hwnd = hwnd;
	d3dAllocator = nullptr;
	d3dCommandListOpen = true;
	lastCommandQueueBarrierActive = false;
	lastCopyQueueBarrierActive = false;
	d3dRenderTargets[0] = nullptr;
	d3dRenderTargets[1] = nullptr;
	d3dRenderTargetReadbackRowWidth = 0;
	width = 0;
	height = 0;

	updateSize();
	loadPipeline();
	loadAssets();
	createRaytracingPipeline();
#endif
}

RT64::Device::~Device() {
	/* TODO: Re-enable once resources are properly released.
	if (d3dAllocator != nullptr) {
		d3dAllocator->Release();
	}
	*/
}

void RT64::Device::createDXGIFactory() {
	UINT dxgiFactoryFlags = 0;
	dxgiFactory = nullptr;

#ifndef NDEBUG
	ID3D12Debug *debugController;
	if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
		debugController->EnableDebugLayer();

		// Enable additional debug layers.
		dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
	}
#endif

	D3D12_CHECK(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&dxgiFactory)));
}

void RT64::Device::createRaytracingDevice() {
	d3dAdapter = nullptr;
	d3dDevice = nullptr;

	std::stringstream ss;
	{
		// Attempt to create D3D12 devices and pick the first one that actually supports raytracing.
		// This implementation should detect more accurately cases where multiple D3D12 adapters are available
		// but they're not raytracing capable, yet there's more devices on the system that fit the criteria.
		DXGI_ADAPTER_DESC1 desc;
		for (UINT adapterIndex = 0; dxgiFactory->EnumAdapters1(adapterIndex, &d3dAdapter) != DXGI_ERROR_NOT_FOUND; ++adapterIndex) {
			d3dAdapter->GetDesc1(&desc);

			// Ignore software adapters.
			if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
				d3dAdapter->Release();
				d3dAdapter = nullptr;
				continue;
			}

			auto handleAdapterError = [this, &ss, &desc, &adapterIndex](const std::string &errorSuffix) {
				ss << "Adapter " << win32::Utf16ToUtf8(desc.Description) << " (#" << adapterIndex << "): " << errorSuffix << std::endl;
				if (d3dDevice != nullptr) {
					d3dDevice->Release();
					d3dDevice = nullptr;
				}

				if (d3dAdapter != nullptr) {
					d3dAdapter->Release();
					d3dAdapter = nullptr;
				}
			};

			// Try creating the device for this adapter.
			HRESULT deviceResult = D3D12CreateDevice(d3dAdapter, D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&d3dDevice));
			if (SUCCEEDED(deviceResult)) {
				D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
				HRESULT checkResult = d3dDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5));
				if (SUCCEEDED(checkResult)) {
					if (options5.RaytracingTier < D3D12_RAYTRACING_TIER_1_0) {
						handleAdapterError("No raytracing support.");
					}
					else {
						break;
					}
				}
				else {
					handleAdapterError("No feature checking at the required level.");
					ss << "D3D12Device->CheckFeatureSupport error code: " << std::hex << checkResult << std::endl;
				}
			}
			else {
				handleAdapterError("No D3D12.1 feature level support.");
				ss << "D3D12CreateDevice error code: " << std::hex << deviceResult << std::endl;
			}
		}
	}

	// Only throw an exception if no device was detected.
	if (d3dDevice == nullptr) {
		throw std::runtime_error("Unable to detect a device capable of raytracing.\n" + ss.str());
	}
}

#ifndef RT64_MINIMAL

void RT64::Device::updateSize() {
	RECT rect;
	GetClientRect(hwnd, &rect);
	int newWidth = rect.right - rect.left;
	int newHeight = rect.bottom - rect.top;

	// Recrease the swap chain if the sizes have changed.
	if (((newWidth != width) || (newHeight != height)) && (newWidth > 0) && (newHeight > 0)) {
		width = newWidth;
		height = newHeight;
		aspectRatio = static_cast<float>(width) / static_cast<float>(height);
		d3dViewport = CD3DX12_VIEWPORT(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
		d3dScissorRect = CD3DX12_RECT(0, 0, static_cast<LONG>(width), static_cast<LONG>(height));

		if (d3dSwapChain != nullptr) {
			releaseRTVs();
			D3D12_CHECK(d3dSwapChain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0));
			createRTVs();
			d3dFrameIndex = d3dSwapChain->GetCurrentBackBufferIndex();
		}

		for (Scene *scene : scenes) {
			scene->resize();
		}

		for (Inspector *inspector : inspectors) {
			inspector->resize();
		}
	}
}

void RT64::Device::releaseRTVs() {
	if (d3dRtvHeap != nullptr) {
		d3dRtvHeap->Release();
		d3dRtvHeap = nullptr;
	}

	for (UINT n = 0; n < FrameCount; n++) {
		d3dRenderTargets[n]->Release();
		d3dRenderTargets[n] = nullptr;
	}

	d3dRenderTargetReadback.Release();
}

void RT64::Device::createRTVs() {
	// Describe and create a render target view (RTV) descriptor heap.
	D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
	rtvHeapDesc.NumDescriptors = FrameCount;
	rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	D3D12_CHECK(d3dDevice->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&d3dRtvHeap)));
	
	d3dRtvDescriptorSize = d3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

	CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(d3dRtvHeap->GetCPUDescriptorHandleForHeapStart());

	// Create a RTV for each frame.
	for (UINT n = 0; n < FrameCount; n++) {
		D3D12_CHECK(d3dSwapChain->GetBuffer(n, IID_PPV_ARGS(&d3dRenderTargets[n])));
		d3dDevice->CreateRenderTargetView(d3dRenderTargets[n], nullptr, rtvHandle);
		rtvHandle.Offset(1, d3dRtvDescriptorSize);
	}

	// Create the resource for render target readback.
	UINT rowPadding;
	CalculateTextureRowWidthPadding(width, 4, d3dRenderTargetReadbackRowWidth, rowPadding);

	D3D12_RESOURCE_DESC resDesc = { };
	resDesc.Format = DXGI_FORMAT_UNKNOWN;
	resDesc.Width = (d3dRenderTargetReadbackRowWidth * height);
	resDesc.Height = 1;
	resDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	resDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	resDesc.DepthOrArraySize = 1;
	resDesc.MipLevels = 1;
	resDesc.SampleDesc.Count = 1;
	d3dRenderTargetReadback = allocateResource(D3D12_HEAP_TYPE_READBACK, &resDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr);
}

HWND RT64::Device::getHwnd() const {
	return hwnd;
}

ID3D12Device8 *RT64::Device::getD3D12Device() {
	return d3dDevice;
}

ID3D12GraphicsCommandList4 *RT64::Device::getD3D12CommandList() {
	return d3dCommandList;
}

ID3D12StateObject *RT64::Device::getD3D12RtStateObject() {
	return d3dRtStateObject;
}

ID3D12StateObjectProperties *RT64::Device::getD3D12RtStateObjectProperties() {
	return d3dRtStateObjectProps;
}

ID3D12Resource *RT64::Device::getD3D12RenderTarget() {
	return d3dRenderTargets[d3dFrameIndex];
}

CD3DX12_CPU_DESCRIPTOR_HANDLE RT64::Device::getD3D12RTV() {
	return CD3DX12_CPU_DESCRIPTOR_HANDLE(d3dRtvHeap->GetCPUDescriptorHandleForHeapStart(), d3dFrameIndex, d3dRtvDescriptorSize);
}

ID3D12RootSignature* RT64::Device::getD3D12RootSignature() {
	return d3dRootSignature;
}

ID3D12PipelineState* RT64::Device::getD3D12PipelineState() {
	return d3dPipelineState;
}

ID3D12RootSignature *RT64::Device::getComposeRootSignature() {
	return d3dComposeRootSignature;
}

ID3D12PipelineState *RT64::Device::getComposePipelineState() {
	return d3dComposePipelineState;
}

ID3D12RootSignature *RT64::Device::getIm3dRootSignature() {
	return im3dRootSignature;
}

ID3D12PipelineState *RT64::Device::getIm3dPipelineStatePoint() {
	return im3dPipelineStatePoint;
}

ID3D12PipelineState *RT64::Device::getIm3dPipelineStateLine() {
	return im3dPipelineStateLine;
}

ID3D12PipelineState *RT64::Device::getIm3dPipelineStateTriangle() {
	return im3dPipelineStateTriangle;
}

CD3DX12_VIEWPORT RT64::Device::getD3D12Viewport() {
	return d3dViewport;
}

CD3DX12_RECT RT64::Device::getD3D12ScissorRect() {
	return d3dScissorRect;
}

RT64::AllocatedResource RT64::Device::allocateResource(D3D12_HEAP_TYPE HeapType, _In_  const D3D12_RESOURCE_DESC *pDesc, D3D12_RESOURCE_STATES InitialResourceState, _In_opt_  const D3D12_CLEAR_VALUE *pOptimizedClearValue, bool committed, bool shared) {
	D3D12MA::ALLOCATION_DESC allocationDesc = {};
	allocationDesc.HeapType = HeapType;
	allocationDesc.ExtraHeapFlags = shared ? D3D12_HEAP_FLAG_SHARED : D3D12_HEAP_FLAG_NONE;
	allocationDesc.Flags = committed ? D3D12MA::ALLOCATION_FLAG_COMMITTED : D3D12MA::ALLOCATION_FLAG_NONE;

	D3D12MA::Allocation *allocation = nullptr;
	ID3D12Resource *resource = nullptr;
	d3dAllocator->CreateResource(&allocationDesc, pDesc, InitialResourceState, pOptimizedClearValue, &allocation, IID_PPV_ARGS(&resource));
	return AllocatedResource(allocation);
}

RT64::AllocatedResource RT64::Device::allocateBuffer(D3D12_HEAP_TYPE HeapType, uint64_t size, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES InitialResourceState, bool committed, bool shared) {
	D3D12MA::ALLOCATION_DESC allocationDesc = {};
	allocationDesc.HeapType = HeapType;
	allocationDesc.ExtraHeapFlags = shared ? D3D12_HEAP_FLAG_SHARED : D3D12_HEAP_FLAG_NONE;
	allocationDesc.Flags = committed ? D3D12MA::ALLOCATION_FLAG_COMMITTED : D3D12MA::ALLOCATION_FLAG_NONE;

	D3D12_RESOURCE_DESC bufDesc = {};
	bufDesc.Alignment = 0;
	bufDesc.DepthOrArraySize = 1;
	bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bufDesc.Flags = flags;
	bufDesc.Format = DXGI_FORMAT_UNKNOWN;
	bufDesc.Height = 1;
	bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	bufDesc.MipLevels = 1;
	bufDesc.SampleDesc.Count = 1;
	bufDesc.SampleDesc.Quality = 0;
	bufDesc.Width = size;

	D3D12MA::Allocation *allocation = nullptr;
	ID3D12Resource *resource = nullptr;
	d3dAllocator->CreateResource(&allocationDesc, &bufDesc, InitialResourceState, nullptr, &allocation, IID_PPV_ARGS(&resource));
	return AllocatedResource(allocation);
}

void RT64::Device::setLastCommandQueueBarrier(const D3D12_RESOURCE_BARRIER &barrier) {
	lastCommandQueueBarrier = barrier;
	lastCommandQueueBarrierActive = true;
}

void RT64::Device::submitCommandQueueBarrier() {
	if (lastCommandQueueBarrierActive) {
		d3dCommandList->ResourceBarrier(1, &lastCommandQueueBarrier);
		lastCommandQueueBarrierActive = false;
	}
}

void RT64::Device::setLastCopyQueueBarrier(const D3D12_RESOURCE_BARRIER &barrier) {
	lastCopyQueueBarrier = barrier;
	lastCopyQueueBarrierActive = true;
}

void RT64::Device::submitCopyQueueBarrier() {
	if (lastCopyQueueBarrierActive) {
		d3dCommandList->ResourceBarrier(1, &lastCopyQueueBarrier);
		lastCopyQueueBarrierActive = false;
	}
}

int RT64::Device::getWidth() const {
	return width;
}

int RT64::Device::getHeight() const {
	return height;
}

float RT64::Device::getAspectRatio() const {
	return aspectRatio;
}

void RT64::Device::loadPipeline() {
	// Create memory allocator.
	D3D12MA::ALLOCATOR_DESC allocatorDesc = {};
	allocatorDesc.pDevice = d3dDevice;
	allocatorDesc.pAdapter = d3dAdapter;

	D3D12_CHECK(D3D12MA::CreateAllocator(&allocatorDesc, &d3dAllocator));

	// Describe and create the command queue.
	D3D12_COMMAND_QUEUE_DESC queueDesc = {};
	queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

	D3D12_CHECK(d3dDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&d3dCommandQueue)));

	// Describe and create the swap chain.
	DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
	swapChainDesc.BufferCount = FrameCount;
	swapChainDesc.Width = width;
	swapChainDesc.Height = height;
	swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	swapChainDesc.SampleDesc.Count = 1;

	IDXGISwapChain1 *swapChain;
	D3D12_CHECK(dxgiFactory->CreateSwapChainForHwnd(d3dCommandQueue, hwnd, &swapChainDesc, nullptr, nullptr, &swapChain));
	d3dSwapChain = static_cast<IDXGISwapChain3 *>(swapChain);
	d3dFrameIndex = d3dSwapChain->GetCurrentBackBufferIndex();

	createRTVs();

	D3D12_CHECK(d3dDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&d3dCommandAllocator)));
}

void RT64::Device::loadAssets() {
	const D3D12_RENDER_TARGET_BLEND_DESC alphaBlendDesc = {
		TRUE, FALSE,
		D3D12_BLEND_SRC_ALPHA, D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_OP_ADD,
		D3D12_BLEND_ONE, D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_OP_ADD,
		D3D12_LOGIC_OP_NOOP,
		D3D12_COLOR_WRITE_ENABLE_ALL
	};

	const D3D12_RENDER_TARGET_BLEND_DESC composeBlendDesc = {
		TRUE, FALSE,
		D3D12_BLEND_ONE, D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_OP_ADD,
		D3D12_BLEND_ONE, D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_OP_ADD,
		D3D12_LOGIC_OP_NOOP,
		D3D12_COLOR_WRITE_ENABLE_ALL
	};

	auto setPsoDefaults = [](D3D12_GRAPHICS_PIPELINE_STATE_DESC &psoDesc, const D3D12_RENDER_TARGET_BLEND_DESC &blendDesc) {
		psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

		D3D12_BLEND_DESC bd = {};
		bd.AlphaToCoverageEnable = FALSE;
		bd.IndependentBlendEnable = FALSE;

		for (UINT i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; i++) {
			bd.RenderTarget[i] = blendDesc;
		}

		psoDesc.BlendState = bd;
		psoDesc.DepthStencilState.DepthEnable = FALSE;
		psoDesc.DepthStencilState.StencilEnable = FALSE;
		psoDesc.SampleMask = UINT_MAX;
		psoDesc.NumRenderTargets = 1;
		psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
		psoDesc.SampleDesc.Count = 1;
	};

	// Raster root signature.
	{
		nv_helpers_dx12::RootSignatureGenerator rsc;
		rsc.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS, 0);
		rsc.AddHeapRangesParameter({
			{ SRV_INDEX(instanceProps), 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, HEAP_INDEX(instanceProps) },
			{ SRV_INDEX(gTextures), 1024, 0, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, HEAP_INDEX(gTextures) }
		});

		d3dRootSignature = rsc.Generate(d3dDevice, false, true, true);
	}

	// Create the pipeline state, which includes compiling and loading shaders.
	{
		// Define the vertex input layout.
		D3D12_INPUT_ELEMENT_DESC inputElementDescs[] =
		{
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "COLOR", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 48, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "COLOR", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 64, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "COLOR", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 80, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "COLOR", 4, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 96, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
		};

		// Describe and create the graphics pipeline state object (PSO).
		D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
		setPsoDefaults(psoDesc, alphaBlendDesc);
		psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
		psoDesc.pRootSignature = d3dRootSignature;
		psoDesc.VS = CD3DX12_SHADER_BYTECODE(RasterVSBlob, sizeof(RasterVSBlob));
		psoDesc.PS = CD3DX12_SHADER_BYTECODE(RasterPSBlob, sizeof(RasterPSBlob));
		psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		D3D12_CHECK(d3dDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&d3dPipelineState)));
	}

	// Im3d Root signature.
	{
		nv_helpers_dx12::RootSignatureGenerator rsc;
		rsc.AddHeapRangesParameter({
			{ UAV_INDEX(gHitDistance), 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, HEAP_INDEX(gHitDistance) },
			{ UAV_INDEX(gHitColor), 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, HEAP_INDEX(gHitColor) },
			{ UAV_INDEX(gHitNormal), 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, HEAP_INDEX(gHitNormal) },
			{ UAV_INDEX(gHitSpecular), 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, HEAP_INDEX(gHitSpecular) },
			{ UAV_INDEX(gHitInstanceId), 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, HEAP_INDEX(gHitInstanceId) },
			{ CBV_INDEX(ViewParams), 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_CBV, HEAP_INDEX(ViewParams) }
		});

		im3dRootSignature = rsc.Generate(d3dDevice, false, true, false);
	}

	// Im3d Pipeline state.
	{
		// Define the vertex input layout.
		D3D12_INPUT_ELEMENT_DESC inputElementDescs[] =
		{
			{ "POSITION_SIZE", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0  }
		};

		// Describe and create the graphics pipeline state object (PSO).
		D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
		setPsoDefaults(psoDesc, alphaBlendDesc);

		psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
		psoDesc.pRootSignature = im3dRootSignature;
		psoDesc.VS = CD3DX12_SHADER_BYTECODE(Im3DVSBlob, sizeof(Im3DVSBlob));
		psoDesc.PS = CD3DX12_SHADER_BYTECODE(Im3DPSBlob, sizeof(Im3DPSBlob));

		psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		D3D12_CHECK(d3dDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&im3dPipelineStateTriangle)));

		psoDesc.GS = CD3DX12_SHADER_BYTECODE(Im3DGSPointsBlob, sizeof(Im3DGSPointsBlob));
		psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
		D3D12_CHECK(d3dDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&im3dPipelineStatePoint)));

		psoDesc.GS = CD3DX12_SHADER_BYTECODE(Im3DGSLinesBlob, sizeof(Im3DGSLinesBlob));
		psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
		D3D12_CHECK(d3dDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&im3dPipelineStateLine)));
	}

	// Compose shader.
	{
		nv_helpers_dx12::RootSignatureGenerator rsc;
		rsc.AddHeapRangesParameter({
			{ 0, 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0 }
		});

		d3dComposeRootSignature = rsc.Generate(d3dDevice, false, true, true);
	}

	{
		// Describe and create the graphics pipeline state object (PSO).
		D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
		setPsoDefaults(psoDesc, composeBlendDesc);
		psoDesc.InputLayout = { nullptr, 0 };
		psoDesc.pRootSignature = d3dComposeRootSignature;
		psoDesc.VS = CD3DX12_SHADER_BYTECODE(ComposeVSBlob, sizeof(ComposeVSBlob));
		psoDesc.PS = CD3DX12_SHADER_BYTECODE(ComposePSBlob, sizeof(ComposePSBlob));
		psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		D3D12_CHECK(d3dDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&d3dComposePipelineState)));
	}

	// Create the command list.
	D3D12_CHECK(d3dDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, d3dCommandAllocator, d3dPipelineState, IID_PPV_ARGS(&d3dCommandList)));

	// Create synchronization objects and wait until assets have been uploaded to the GPU.
	D3D12_CHECK(d3dDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&d3dFence)));
	d3dFenceValue = 1;

	// Create an event handle to use for frame synchronization.
	d3dFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	if (d3dFenceEvent == nullptr) {
		D3D12_CHECK(HRESULT_FROM_WIN32(GetLastError()));
	}

	// Close command list and wait for it to finish.
	waitForGPU();
}

void RT64::Device::createRaytracingPipeline() {
	nv_helpers_dx12::RayTracingPipelineGenerator pipeline(d3dDevice);

	// Shader libraries.
	d3dTracerLibrary = new StaticBlob(TracerBlob, sizeof(TracerBlob));
	d3dSurfaceLibrary = new StaticBlob(SurfaceBlob, sizeof(SurfaceBlob));
	d3dShadowLibrary = new StaticBlob(ShadowBlob, sizeof(ShadowBlob));

	// Add shaders from library to the pipeline.
	pipeline.AddLibrary(d3dTracerLibrary, { L"TraceRayGen" });
	pipeline.AddLibrary(d3dSurfaceLibrary, { L"SurfaceClosestHit", L"SurfaceAnyHit", L"SurfaceMiss" });
	pipeline.AddLibrary(d3dShadowLibrary, { L"ShadowClosestHit", L"ShadowAnyHit", L"ShadowMiss" });

	// Create root signatures.
	d3dTracerSignature = createTracerSignature();
	d3dSurfaceShadowSignature = createSurfaceShadowSignature();

	// Add the hit groups with the loaded shaders.
	pipeline.AddHitGroup(L"SurfaceHitGroup", L"SurfaceClosestHit", L"SurfaceAnyHit");
	pipeline.AddHitGroup(L"ShadowHitGroup", L"ShadowClosestHit", L"ShadowAnyHit");

	// Associate the root signatures to the hit groups.
	pipeline.AddRootSignatureAssociation(d3dTracerSignature, { L"TraceRayGen" });
	pipeline.AddRootSignatureAssociation(d3dSurfaceShadowSignature, { L"SurfaceHitGroup" });
	pipeline.AddRootSignatureAssociation(d3dSurfaceShadowSignature, { L"ShadowHitGroup" });
	
	// Pipeline configuration. Path tracing only needs one recursion level at most.
	pipeline.SetMaxPayloadSize(2 * sizeof(float));
	pipeline.SetMaxAttributeSize(2 * sizeof(float));
	pipeline.SetMaxRecursionDepth(1);

	// Generate the pipeline.
	d3dRtStateObject = pipeline.Generate();

	// Cast the state object into a properties object, allowing to later access the shader pointers by name.
	D3D12_CHECK(d3dRtStateObject->QueryInterface(IID_PPV_ARGS(&d3dRtStateObjectProps)));

}

ID3D12RootSignature *RT64::Device::createTracerSignature() {
	nv_helpers_dx12::RootSignatureGenerator rsc;
	rsc.AddHeapRangesParameter({
		{ UAV_INDEX(gOutput), 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, HEAP_INDEX(gOutput) },
		{ UAV_INDEX(gAlbedo), 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, HEAP_INDEX(gAlbedo) },
		{ UAV_INDEX(gNormal), 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, HEAP_INDEX(gNormal) },
		{ UAV_INDEX(gHitDistance), 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, HEAP_INDEX(gHitDistance) },
		{ UAV_INDEX(gHitColor), 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, HEAP_INDEX(gHitColor) },
		{ UAV_INDEX(gHitNormal), 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, HEAP_INDEX(gHitNormal) },
		{ UAV_INDEX(gHitSpecular), 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, HEAP_INDEX(gHitSpecular) },
		{ UAV_INDEX(gHitInstanceId), 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, HEAP_INDEX(gHitInstanceId) },
		{ SRV_INDEX(gBackground), 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, HEAP_INDEX(gBackground) },
		{ SRV_INDEX(SceneBVH), 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, HEAP_INDEX(SceneBVH) },
		{ SRV_INDEX(SceneLights), 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, HEAP_INDEX(SceneLights) },
		{ SRV_INDEX(instanceProps), 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, HEAP_INDEX(instanceProps) },
		{ CBV_INDEX(ViewParams), 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_CBV, HEAP_INDEX(ViewParams) }
	});

	return rsc.Generate(d3dDevice, true, false, true);
}

ID3D12RootSignature *RT64::Device::createSurfaceShadowSignature() {
	nv_helpers_dx12::RootSignatureGenerator rsc;
	rsc.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_SRV, SRV_INDEX(vertexBuffer));
	rsc.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_SRV, SRV_INDEX(indexBuffer));
	rsc.AddHeapRangesParameter({
		{ UAV_INDEX(gHitDistance), 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, HEAP_INDEX(gHitDistance) },
		{ UAV_INDEX(gHitColor), 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, HEAP_INDEX(gHitColor) },
		{ UAV_INDEX(gHitNormal), 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, HEAP_INDEX(gHitNormal) },
		{ UAV_INDEX(gHitSpecular), 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, HEAP_INDEX(gHitSpecular) },
		{ UAV_INDEX(gHitInstanceId), 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, HEAP_INDEX(gHitInstanceId) },
		{ SRV_INDEX(instanceProps), 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, HEAP_INDEX(instanceProps) },
		{ SRV_INDEX(gTextures), 1024, 0, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, HEAP_INDEX(gTextures) },
		{ CBV_INDEX(ViewParams), 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_CBV, HEAP_INDEX(ViewParams) }
	});

	return rsc.Generate(d3dDevice, true, false, true);
}

void RT64::Device::preRender() {
	// Submit and wait for execution if command list was open.
	if (d3dCommandListOpen) {
		submitCommandList();
		waitForGPU();
	}

	resetCommandList();

	// Set necessary state.
	d3dCommandList->SetGraphicsRootSignature(d3dRootSignature);
	d3dCommandList->RSSetViewports(1, &d3dViewport);
	d3dCommandList->RSSetScissorRects(1, &d3dScissorRect);

	// Indicate that the back buffer will be used as a render target.
	CD3DX12_RESOURCE_BARRIER transitionBarrier = CD3DX12_RESOURCE_BARRIER::Transition(d3dRenderTargets[d3dFrameIndex], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
	d3dCommandList->ResourceBarrier(1, &transitionBarrier);

	CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle = getD3D12RTV();
	d3dCommandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

	const float clearColor[] = { 0.0f, 0.0f, 0.0f, 1.0f };
	d3dCommandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
}

void RT64::Device::postRender(int vsyncInterval) {
	// Indicate that the back buffer will now be used to present.
	CD3DX12_RESOURCE_BARRIER transitionBarrier = CD3DX12_RESOURCE_BARRIER::Transition(d3dRenderTargets[d3dFrameIndex], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
	d3dCommandList->ResourceBarrier(1, &transitionBarrier);

	submitCommandList();

	// Present the frame.
	D3D12_CHECK(d3dSwapChain->Present(vsyncInterval, 0));

	waitForGPU();
	d3dFrameIndex = d3dSwapChain->GetCurrentBackBufferIndex();

	// Leave command list open.
	resetCommandList();
}

void RT64::Device::draw(int vsyncInterval) {
	submitCommandQueueBarrier();
	submitCopyQueueBarrier();
	
	// Make sure that the size of the window is up to date.
	updateSize();
	
	// Update all scenes as necessary.
	for (Scene *scene : scenes) {
		scene->update();
	}

	// Render each scene.
	preRender();

	for (Scene *scene : scenes) {
		scene->render();
	}

	// Scene has most likely changed the render target. Set it again for the inspectors to work properly.
	CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle = getD3D12RTV();
	d3dCommandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

	// Find mouse cursor position.
	POINT cursorPos = {};
	GetCursorPos(&cursorPos);
	ScreenToClient(hwnd, &cursorPos);

	// Determine the active view (use the first available view for now).
	View *activeView = nullptr;
	for (Scene *scene : scenes) {
		auto views = scene->getViews();
		if (!views.empty()) {
			activeView = views[0];
		}
	}

	// Render the inspectors on the active view.
	if (activeView != nullptr) {
		for (Inspector *inspector : inspectors) {
			inspector->render(activeView, cursorPos.x, cursorPos.y);
			inspector->reset();
		}
	}

	postRender(vsyncInterval);
}

void RT64::Device::addScene(Scene *scene) {
	assert(scene != nullptr);
	scenes.push_back(scene);
}

void RT64::Device::removeScene(Scene *scene) {
	assert(scene != nullptr);
	scenes.erase(std::remove(scenes.begin(), scenes.end(), scene), scenes.end());
}

void RT64::Device::addInspector(Inspector* inspector) {
	assert(inspector != nullptr);
	inspectors.push_back(inspector);
}

void RT64::Device::removeInspector(Inspector* inspector) {
	assert(inspector != nullptr);
	inspectors.erase(std::remove(inspectors.begin(), inspectors.end(), inspector), inspectors.end());
}

void RT64::Device::resetCommandList() {
	// Reset the command allocator.
	d3dCommandAllocator->Reset();

	// Reset the command list.
	d3dCommandList->Reset(d3dCommandAllocator, d3dPipelineState);

	d3dCommandListOpen = true;
}

void RT64::Device::submitCommandList() {
	// Close the command list.
	d3dCommandList->Close();

	// Execute command list and signal on the fence when it's completed.
	ID3D12CommandList *pGraphicsList = { d3dCommandList };
	d3dCommandQueue->ExecuteCommandLists(1, &pGraphicsList);

	d3dCommandListOpen = false;
}

void RT64::Device::waitForGPU() {
	// Schedule a signal command in the queue.
	d3dCommandQueue->Signal(d3dFence, d3dFenceValue);

	// Wait until the fence has been processed.
	d3dFence->SetEventOnCompletion(d3dFenceValue, d3dFenceEvent);
	WaitForSingleObjectEx(d3dFenceEvent, INFINITE, FALSE);

	// Increment the fence value.
	d3dFenceValue++;
}

void RT64::Device::dumpRenderTarget(const std::string &path) {
	ID3D12Resource *renderTarget = getD3D12RenderTarget();

	CD3DX12_RESOURCE_BARRIER transitionBarrier = CD3DX12_RESOURCE_BARRIER::Transition(renderTarget, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
	d3dCommandList->ResourceBarrier(1, &transitionBarrier);

	D3D12_TEXTURE_COPY_LOCATION source = {};
	source.pResource = renderTarget;
	source.SubresourceIndex = 0;
	source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

	D3D12_SUBRESOURCE_FOOTPRINT subresource = {};
	subresource.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	subresource.Width = width;
	subresource.Height = height;
	subresource.RowPitch = d3dRenderTargetReadbackRowWidth;
	subresource.Depth = 1;

	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
	footprint.Offset = 0;
	footprint.Footprint = subresource;

	D3D12_TEXTURE_COPY_LOCATION destination = {};
	destination.pResource = d3dRenderTargetReadback.Get();
	destination.PlacedFootprint = footprint;
	destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;

	d3dCommandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

	transitionBarrier = CD3DX12_RESOURCE_BARRIER::Transition(renderTarget, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
	d3dCommandList->ResourceBarrier(1, &transitionBarrier);

	// Wait until the resource is actually copied.
	submitCommandList();
	waitForGPU();
	resetCommandList();
	
	// Save the render target copy to the target path.
	unsigned char *bmpRGB = (unsigned char *)(malloc(width * height * 3));
	{
		UINT8 *pData;
		d3dRenderTargetReadback.Get()->Map(0, nullptr, reinterpret_cast<void **>(&pData));
		int i = 0;
		for (int y = 0; y < height; y++) {
			for (int x = 0; x < width; x++) {
				bmpRGB[i++] = pData[y * d3dRenderTargetReadbackRowWidth + x * 4 + 0];
				bmpRGB[i++] = pData[y * d3dRenderTargetReadbackRowWidth + x * 4 + 1];
				bmpRGB[i++] = pData[y * d3dRenderTargetReadbackRowWidth + x * 4 + 2];
			}
		}
		d3dRenderTargetReadback.Get()->Unmap(0, nullptr);
	}

	stbi_write_bmp(path.c_str(), width, height, 3, bmpRGB);
	free(bmpRGB);

	// Reset the current render target.
	CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle = getD3D12RTV();
	d3dCommandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
}

#endif

// Public

DLLEXPORT RT64_DEVICE *RT64_CreateDevice(void *hwnd) {
	try {
		return (RT64_DEVICE *)(new RT64::Device((HWND)(hwnd)));
	}
	RT64_CATCH_EXCEPTION();
	return nullptr;
}

DLLEXPORT void RT64_DestroyDevice(RT64_DEVICE *devicePtr) {
	assert(devicePtr != nullptr);
	try {
		delete (RT64::Device *)(devicePtr);
	}
	RT64_CATCH_EXCEPTION();
}

#ifndef RT64_MINIMAL

DLLEXPORT void RT64_DrawDevice(RT64_DEVICE *devicePtr, int vsyncInterval) {
	assert(devicePtr != nullptr);
	try {
		RT64::Device *device = (RT64::Device *)(devicePtr);
		device->draw(vsyncInterval);
	}
	RT64_CATCH_EXCEPTION();
}

#endif