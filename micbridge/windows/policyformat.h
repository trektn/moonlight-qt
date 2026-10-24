#pragma once
#include <mmdeviceapi.h>

// IPolicyConfig's first four slots after IUnknown. The COM ABI is documented in
// EreTIk's PolicyConfig.h and used by Apollo's Steam microphone implementation:
// https://github.com/logabell/apollo-microphone/tree/master/src/platform/windows
inline constexpr GUID MicPolicyInterface = {0xf8679f50, 0x850a, 0x41cf, {0x9c, 0x72, 0x43, 0x0f, 0x29, 0x02, 0x90, 0xc8}};
inline constexpr GUID MicPolicyClass = {0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};
inline constexpr GUID MicPcmSubtype = {1, 0, 0x10, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
inline constexpr GUID MicFloatSubtype = {3, 0, 0x10, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
struct IPolicyFormat : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
};
