//
// RT64
//

#define MAX_HIT_QUERIES	16

RWBuffer<float> gHitDistance : register(u3);
RWBuffer<half4> gHitColor : register(u4);
RWBuffer<half4> gHitNormal : register(u5);
RWBuffer<uint> gHitInstanceId : register(u6);
RWBuffer<half> gHitSpecular : register(u7);

uint getHitBufferIndex(uint hitPos, uint2 pixelIdx, uint2 pixelDims) {
	return (hitPos * pixelDims.y + pixelIdx.y) * pixelDims.x + pixelIdx.x;
}