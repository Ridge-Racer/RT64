//
// RT64
//

#pragma once

#include <fstream>
#include <sstream>
#include <string>
#include <d3d12.h>
#include <d3dx12.h>
#include <dxcapi.h>
#include <d3dcompiler.h>

#include <vector>

#include <dxgi1_4.h>
#include <d3d12.h>

#include "D3D12MemoryAllocator/D3D12MemAlloc.h"

#include <DirectXMath.h>

#include "../public/rt64.h"

#define DLLEXPORT extern "C" __declspec(dllexport)  

using namespace DirectX;

#define HEAP_INDEX(x) (int)(RT64::HeapIndices::x)
#define UAV_INDEX(x) (int)(RT64::UAVIndices::x)
#define SRV_INDEX(x) (int)(RT64::SRVIndices::x)
#define CBV_INDEX(x) (int)(RT64::CBVIndices::x)

namespace RT64 {
	// Matches order in heap used in shader binding table.
	enum class HeapIndices : int {
		gOutput,
		gAlbedo,
		gNormal,
		gHitDistance,
		gHitColor,
		gHitNormal,
		gHitSpecular,
		gHitInstanceId,
		gBackground,
		SceneBVH,
		ViewParams,
		SceneLights,
		instanceProps,
		gTextures,
		MAX
	};

	enum class UAVIndices : int {
		gOutput,
		gAlbedo,
		gNormal,
		gHitDistance,
		gHitColor,
		gHitNormal,
		gHitSpecular,
		gHitInstanceId
	};

	enum class SRVIndices : int {
		SceneBVH,
		gBackground,
		vertexBuffer,
		indexBuffer,
		SceneLights,
		instanceProps,
		gTextures
	};

	enum class CBVIndices : int {
		ViewParams
	};

	// Error string for last error or exception that was caught.
	extern std::string GlobalLastError;

#ifndef RT64_MINIMAL
	class AllocatedResource {
	private:
		D3D12MA::Allocation *d3dMaAllocation;
	public:
		AllocatedResource() {
			d3dMaAllocation = nullptr;
		}

		AllocatedResource(D3D12MA::Allocation *d3dMaAllocation) {
			this->d3dMaAllocation = d3dMaAllocation;
		}

		~AllocatedResource() { }

		inline ID3D12Resource *Get() const {
			if (!IsNull()) {
				return d3dMaAllocation->GetResource();
			}
			else {
				return nullptr;
			}
		}

		inline bool IsNull() const {
			return (d3dMaAllocation == nullptr);
		}

		void Release() {
			if (!IsNull()) {
				ID3D12Resource *d3dResource = d3dMaAllocation->GetResource();
				d3dMaAllocation->Release();
				d3dResource->Release();
				d3dMaAllocation = nullptr;
			}
		}
	};

	struct InstanceProperties {
		XMMATRIX objectToWorld;
		XMMATRIX objectToWorldNormal;
		RT64_MATERIAL material;
	};

	struct AccelerationStructureBuffers {
		AllocatedResource scratch;
		UINT64 scratchSize;
		AllocatedResource result;
		UINT64 resultSize;
		AllocatedResource instanceDesc;
		UINT64 instanceDescSize;

		AccelerationStructureBuffers() {
			scratchSize = resultSize = instanceDescSize = 0;
		}

		void Release() {
			scratch.Release();
			result.Release();
			instanceDesc.Release();
			scratchSize = resultSize = instanceDescSize = 0;
		}
	};

	// Points to a static array of data in memory using the IDxcBlob interface.
	class StaticBlob : public IDxcBlob {
	private:
		const unsigned char *data;
		size_t dataSize;
		std::atomic<int> refCount;
	public:
		StaticBlob(const unsigned char *data, size_t dataSize) {
			this->data = data;
			this->dataSize = dataSize;
		}

		~StaticBlob() { }

		virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, _COM_Outptr_ void __RPC_FAR *__RPC_FAR *ppvObject) override {
			// Always set out parameter to NULL, validating it first.
			if (!ppvObject) {
				return E_INVALIDARG;
			}

			*ppvObject = NULL;
			if (riid == IID_IUnknown) {
				// Increment the reference count and return the pointer.
				*ppvObject = (LPVOID)this;
				AddRef();
				return NOERROR;
			}

			return E_NOINTERFACE;
		}

		virtual ULONG STDMETHODCALLTYPE AddRef(void) override {
			refCount++;
			return refCount;
		}

		virtual ULONG STDMETHODCALLTYPE Release(void) override {
			refCount--;
			if (refCount == 0) {
				delete this;
			}

			return refCount;
		}

		virtual LPVOID STDMETHODCALLTYPE GetBufferPointer(void) override {
			return (LPVOID)(data);
		}

		virtual SIZE_T STDMETHODCALLTYPE GetBufferSize(void) override {
			return dataSize;
		}
	};

	inline float Length(const RT64_VECTOR3 &a) {
		float sqrLength = a.x * a.x + a.y * a.y + a.z * a.z;
		return sqrt(sqrLength);
	}

	inline void operator+=(RT64_VECTOR3 &a, const RT64_VECTOR3 &b) {
		a.x += b.x;
		a.y += b.y;
		a.z += b.z;
	}

	inline RT64_VECTOR3 operator/(const RT64_VECTOR3 &a, const float v) {
		return { a.x / v, a.y / v, a.z / v };
	}

	inline RT64_VECTOR3 DirectionFromTo(const RT64_VECTOR3 &a, const RT64_VECTOR3 &b) {
		RT64_VECTOR3 dir = { b.x - a.x,  b.y - a.y, b.z - a.z };
		float length = Length(dir);
		return dir / length;
	}

	inline void CalculateTextureRowWidthPadding(int width, int stride, UINT &rowWidth, UINT &rowPadding) {
		const int RowMultiple = 256;
		rowWidth = width * stride;
		rowPadding = (rowWidth % RowMultiple) ? RowMultiple - (rowWidth % RowMultiple) : 0;
		rowWidth += rowPadding;
	}
#endif
};

#define D3D12_CHECK( call )                                                         \
    do                                                                              \
    {                                                                               \
        HRESULT hr = call;                                                          \
        if (FAILED(hr))														        \
        {																	        \
			char errorMessage[512];													\
			snprintf(errorMessage, sizeof(errorMessage), "D3D12 call " #call " "	\
				"failed with error code %X.", hr);									\
																					\
            throw std::runtime_error(errorMessage);                                 \
        }                                                                           \
    } while( 0 )

#define RT64_CATCH_EXCEPTION()							\
	catch (const std::runtime_error &e) {				\
		RT64::GlobalLastError = std::string(e.what());	\
		fprintf(stderr, "%s\n", e.what());				\
	}

#ifndef RT64_MINIMAL
namespace nv_helpers_dx12
{
#ifndef ROUND_UP
#define ROUND_UP(v, powerOf2Alignment) (((v) + (powerOf2Alignment)-1) & ~((powerOf2Alignment)-1))
#endif

	ID3D12DescriptorHeap* CreateDescriptorHeap(ID3D12Device* device, uint32_t count, D3D12_DESCRIPTOR_HEAP_TYPE type, bool shaderVisible);
}
#endif