//
// RT64
//

cbuffer ViewParams : register(b0) {
	float4x4 view;
	float4x4 projection;
	float4x4 viewI;
	float4x4 projectionI;
	float4x4 prevViewProj;
	float4 viewport;
	float4 resolution;
	uint randomSeed;
	uint softLightSamples;
	uint giBounces;
	uint giEnvBounces;
	uint maxLightSamples;
	float ambGIMixWeight;
	uint frameCount;
}