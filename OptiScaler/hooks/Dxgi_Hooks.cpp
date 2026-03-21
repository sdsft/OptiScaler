#include "pch.h"
#include "Dxgi_Hooks.h"

#include "DxgiFactory_Hooks.h"

#include <proxies/Dxgi_Proxy.h>
#include <proxies/D3D12_Proxy.h>
#include <proxies/Streamline_Proxy.h>
#include <wrapped/wrapped_factory.h>

#include <DllNames.h>
#include <misc/IdentifyGpu.h>

#include "Hook_Utils.h"

static DxgiProxy::PFN_CreateDxgiFactory o_CreateDXGIFactory = nullptr;
static DxgiProxy::PFN_CreateDxgiFactory1 o_CreateDXGIFactory1 = nullptr;
static DxgiProxy::PFN_CreateDxgiFactory2 o_CreateDXGIFactory2 = nullptr;
static bool creatingD3D12DeviceForLuma = false;
static bool skipDLSSGFactory = false;

#pragma intrinsic(_ReturnAddress)

static void InitD3D12DeviceForLuma(IDXGIFactory* factory)
{
    IDXGIAdapter* hardwareAdapter = nullptr;
    IdentifyGpu::getHardwareAdapter(factory, &hardwareAdapter, D3D_FEATURE_LEVEL_11_0);

    if (hardwareAdapter == nullptr)
        LOG_WARN("Can't get hardwareAdapter, will try nullptr!");

    auto result = D3d12Proxy::D3D12CreateDevice_()(hardwareAdapter, D3D_FEATURE_LEVEL_11_0,
                                                   IID_PPV_ARGS(&State::Instance().currentD3D12Device));

    LOG_DEBUG("D3D12CreateDevice result: {0:x}", result);
}

static void CheckLumaAndReShade(IDXGIFactory* factory)
{
    if (!Config::Instance()->LoadReShade.value_or_default() ||
        !(State::Instance().gameQuirks & GameQuirk::CreateD3D12DeviceForLuma) ||
        State::Instance().currentD3D12Device != nullptr || creatingD3D12DeviceForLuma)
    {
        return;
    }

    auto rsFile = Util::ExePath().parent_path() / L"ReShade64.dll";
    if (reshadeModule == nullptr)
    {
        auto rsFileExist = std::filesystem::exists(rsFile);
        if (!rsFileExist)
        {
            Config::Instance()->LoadReShade.set_volatile_value(false);
            return;
        }
    }

    // For Luma mod + Agility update we are creating D3D12 device early to prevent issues with Luma
    if (State::Instance().gameQuirks & GameQuirk::CreateD3D12DeviceForLuma &&
        State::Instance().currentD3D12Device == nullptr)
    {
        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};

        ScopedSkipSpoofing skipSpoofing {};
        creatingD3D12DeviceForLuma = true;

        LOG_INFO("Applying Luma DX12 workaround - creating D3D12 device early");
        InitD3D12DeviceForLuma(factory);

        creatingD3D12DeviceForLuma = false;
    }

    // Loading Reshade after Luma's D3D12 device creation to prevent conflicts
    if (reshadeModule == nullptr && Config::Instance()->LoadReShade.value_or_default())
    {
        SetEnvironmentVariableW(L"RESHADE_DISABLE_LOADING_CHECK", L"1");

        if (skModule != nullptr)
            SetEnvironmentVariableW(L"RESHADE_DISABLE_GRAPHICS_HOOK", L"1");

        State::EnableServeOriginal(201);
        reshadeModule = NtdllProxy::LoadLibraryExW_Ldr(rsFile.c_str(), NULL, 0);
        State::DisableServeOriginal(201);

        LOG_INFO("Loading ReShade64.dll, result: {0:X}", (size_t) reshadeModule);
    }
}

VALIDATE_HOOK(hkCreateDXGIFactory, DxgiProxy::PFN_CreateDxgiFactory)
inline static HRESULT hkCreateDXGIFactory(REFIID riid, IDXGIFactory** ppFactory)
{
    DxgiProxy::PFN_CreateDxgiFactory cdf = nullptr;
    bool dlssgFactory = false;

    if (State::Instance().activeFgOutput == FGOutput::DLSSG && !skipDLSSGFactory &&
        StreamlineProxy::CreateDxgiFactory() != nullptr)
    {
        cdf = StreamlineProxy::CreateDxgiFactory();
        dlssgFactory = true;
    }
    else
    {
        cdf = o_CreateDXGIFactory;
    }

    auto caller = Util::WhoIsTheCaller(_ReturnAddress());
    LOG_DEBUG("Caller: {}", caller);

    if (creatingD3D12DeviceForLuma)
    {
        LOG_DEBUG("Bypassing hooking/wrapping during Luma D3D12 device creation");

        HRESULT result;

        if (!skipDLSSGFactory)
        {
            skipDLSSGFactory = true;
            result = cdf(riid, ppFactory);
            skipDLSSGFactory = false;
        }
        else
        {
            result = cdf(riid, ppFactory);
        }

        return result;
    }

    if (Config::Instance()->DxgiFactoryWrapping.value_or_default() && CheckDllName(&caller, &skipDxgiWrappingNames))
    {
        LOG_INFO("Skipping wrapping for: {}", caller);

        HRESULT result;

        if (!skipDLSSGFactory)
        {
            skipDLSSGFactory = true;
            result = cdf(riid, ppFactory);
            skipDLSSGFactory = false;
        }
        else
        {
            result = cdf(riid, ppFactory);
        }

        return result;
    }

    if (Config::Instance()->DxgiFactoryWrapping.value_or_default() &&
        Util::GetCallerModule(_ReturnAddress()) == slInterposerModule)
    {
        LOG_DEBUG("Delaying 500ms");
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    HRESULT result;
    auto owner = State::GetOwner();
    State::DisableChecks(owner, "dxgi");
    if (!skipDLSSGFactory)
    {
#ifndef ENABLE_DEBUG_LAYER_DX12
        skipDLSSGFactory = true;
        result = cdf(riid, ppFactory);
        skipDLSSGFactory = false;
#else
        result = o_CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG, riid, (IDXGIFactory2**) ppFactory);
#endif
    }
    else
    {
#ifndef ENABLE_DEBUG_LAYER_DX12
        result = cdf(riid, ppFactory);
#else
        result = o_CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG, riid, (IDXGIFactory2**) ppFactory);
#endif
    }
    State::EnableChecks(owner);

    if (result != S_OK)
        return result;

    // if (State::Instance().activeFgOutput != FGOutput::DLSSG ||
    //     (StreamlineProxy::CreateDxgiFactory() != nullptr && !skipDLSSGFactory))
    {
        IDXGIFactory* real = nullptr;

        if (State::Instance().activeFgOutput != FGOutput::DLSSG &&
            Util::CheckForRealObject(__FUNCTION__, *ppFactory, (IUnknown**) &real))
        {
            *ppFactory = real;
        }

        if (Config::Instance()->DxgiFactoryWrapping.value_or_default())
        {
            *ppFactory = (IDXGIFactory*) (new WrappedIDXGIFactory7(*ppFactory));
        }
        else
        {
            if (!dlssgFactory)
            {
                DxgiFactoryHooks::HookToFactory(*ppFactory);
            }
            else
            {
                DxgiFactoryHooks::HookToDLSSGFactory(*ppFactory);
            }
        }
    }

    CheckLumaAndReShade(*ppFactory);

    return result;
}

VALIDATE_HOOK(hkCreateDXGIFactory1, DxgiProxy::PFN_CreateDxgiFactory1)
inline static HRESULT hkCreateDXGIFactory1(REFIID riid, IDXGIFactory1** ppFactory)
{
    DxgiProxy::PFN_CreateDxgiFactory1 cdf = nullptr;
    bool dlssgFactory = false;

    if (State::Instance().activeFgOutput == FGOutput::DLSSG && !skipDLSSGFactory &&
        StreamlineProxy::CreateDxgiFactory1() != nullptr)
    {
        cdf = StreamlineProxy::CreateDxgiFactory1();
        dlssgFactory = true;
    }
    else
    {
        cdf = o_CreateDXGIFactory1;
    }

    auto caller = Util::WhoIsTheCaller(_ReturnAddress());
    LOG_DEBUG("Caller: {}", caller);

    if (creatingD3D12DeviceForLuma)
    {
        LOG_DEBUG("Bypassing hooking/wrapping during Luma D3D12 device creation");

        HRESULT result;

        if (!skipDLSSGFactory)
        {
            skipDLSSGFactory = true;
            result = cdf(riid, ppFactory);
            skipDLSSGFactory = false;
        }
        else
        {
            result = cdf(riid, ppFactory);
        }

        return result;
    }

    if (Config::Instance()->DxgiFactoryWrapping.value_or_default() && CheckDllName(&caller, &skipDxgiWrappingNames))
    {
        LOG_INFO("Skipping wrapping for: {}", caller);

        HRESULT result;

        if (!skipDLSSGFactory)
        {
            skipDLSSGFactory = true;
            result = cdf(riid, ppFactory);
            skipDLSSGFactory = false;
        }
        else
        {
            result = cdf(riid, ppFactory);
        }

        return result;
    }

    if (Config::Instance()->DxgiFactoryWrapping.value_or_default() &&
        Util::GetCallerModule(_ReturnAddress()) == slInterposerModule)
    {
        LOG_DEBUG("Delaying 500ms");
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    HRESULT result;
    auto owner = State::GetOwner();
    State::DisableChecks(owner, "dxgi");
    if (!skipDLSSGFactory)
    {
#ifndef ENABLE_DEBUG_LAYER_DX12
        skipDLSSGFactory = true;
        result = cdf(riid, ppFactory);
        skipDLSSGFactory = false;
#else
        result = o_CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG, riid, (IDXGIFactory2**) ppFactory);
#endif
    }
    else
    {
#ifndef ENABLE_DEBUG_LAYER_DX12
        result = cdf(riid, ppFactory);
#else
        result = o_CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG, riid, (IDXGIFactory2**) ppFactory);
#endif
    }
    State::EnableChecks(owner);

    if (result != S_OK)
        return result;

    // if (State::Instance().activeFgOutput != FGOutput::DLSSG ||
    //     (StreamlineProxy::CreateDxgiFactory() != nullptr && !skipDLSSGFactory))
    {
        IDXGIFactory1* real = nullptr;

        if (State::Instance().activeFgOutput != FGOutput::DLSSG &&
            Util::CheckForRealObject(__FUNCTION__, *ppFactory, (IUnknown**) &real))
        {
            *ppFactory = real;
        }

        if (Config::Instance()->DxgiFactoryWrapping.value_or_default())
        {
            *ppFactory = (IDXGIFactory1*) (new WrappedIDXGIFactory7(*ppFactory));
        }
        else
        {
            if (!dlssgFactory)
            {
                DxgiFactoryHooks::HookToFactory(*ppFactory);
            }
            else
            {
                DxgiFactoryHooks::HookToDLSSGFactory(*ppFactory);
            }
        }
    }

    CheckLumaAndReShade(*ppFactory);

    return result;
}

VALIDATE_HOOK(hkCreateDXGIFactory2, DxgiProxy::PFN_CreateDxgiFactory2)
inline static HRESULT hkCreateDXGIFactory2(UINT Flags, REFIID riid, IDXGIFactory2** ppFactory)
{
    DxgiProxy::PFN_CreateDxgiFactory2 cdf = nullptr;
    bool dlssgFactory = false;

    if (State::Instance().activeFgOutput == FGOutput::DLSSG && !skipDLSSGFactory &&
        StreamlineProxy::CreateDxgiFactory2() != nullptr)
    {
        cdf = StreamlineProxy::CreateDxgiFactory2();
        dlssgFactory = true;
    }
    else
    {
        cdf = o_CreateDXGIFactory2;
    }

    auto caller = Util::WhoIsTheCaller(_ReturnAddress());
    LOG_DEBUG("Caller: {}", caller);

    if (creatingD3D12DeviceForLuma)
    {
        LOG_DEBUG("Bypassing hooking/wrapping during Luma D3D12 device creation");

        HRESULT result;

        if (!skipDLSSGFactory)
        {
            skipDLSSGFactory = true;
            result = cdf(Flags, riid, ppFactory);
            skipDLSSGFactory = false;
        }
        else
        {
            result = cdf(Flags, riid, ppFactory);
        }

        return result;
    }

    if (Config::Instance()->DxgiFactoryWrapping.value_or_default() && CheckDllName(&caller, &skipDxgiWrappingNames))
    {
        LOG_INFO("Skipping wrapping for: {}", caller);

        HRESULT result;

        if (!skipDLSSGFactory)
        {
            skipDLSSGFactory = true;
            result = cdf(Flags, riid, ppFactory);
            skipDLSSGFactory = false;
        }
        else
        {
            result = cdf(Flags, riid, ppFactory);
        }

        return result;
    }

    LOG_DEBUG("Caller: {}", Util::WhoIsTheCaller(_ReturnAddress()));

    if (Config::Instance()->DxgiFactoryWrapping.value_or_default() &&
        Util::GetCallerModule(_ReturnAddress()) == slInterposerModule)
    {
        LOG_DEBUG("Delaying 500ms");
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    HRESULT result;
    auto owner = State::GetOwner();
    State::DisableChecks(owner, "dxgi");
    if (!skipDLSSGFactory)
    {
#ifndef ENABLE_DEBUG_LAYER_DX12
        skipDLSSGFactory = true;
        result = cdf(Flags, riid, ppFactory);
        skipDLSSGFactory = false;
#else
        result = o_CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG, riid, (IDXGIFactory2**) ppFactory);
#endif
    }
    else
    {
#ifndef ENABLE_DEBUG_LAYER_DX12
        result = cdf(Flags, riid, ppFactory);
#else
        result = o_CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG, riid, (IDXGIFactory2**) ppFactory);
#endif
    }
    State::EnableChecks(owner);

    if (result != S_OK)
        return result;

    // if (State::Instance().activeFgOutput != FGOutput::DLSSG ||
    //     (StreamlineProxy::CreateDxgiFactory() != nullptr && !skipDLSSGFactory))
    {
        IDXGIFactory2* real = nullptr;

        if (State::Instance().activeFgOutput != FGOutput::DLSSG &&
            Util::CheckForRealObject(__FUNCTION__, *ppFactory, (IUnknown**) &real))
        {
            *ppFactory = real;
        }

        if (Config::Instance()->DxgiFactoryWrapping.value_or_default())
        {
            *ppFactory = (IDXGIFactory2*) (new WrappedIDXGIFactory7(*ppFactory));
        }
        else
        {
            if (!dlssgFactory)
            {
                DxgiFactoryHooks::HookToFactory(*ppFactory);
            }
            else
            {
                DxgiFactoryHooks::HookToDLSSGFactory(*ppFactory);
            }
        }
    }

    CheckLumaAndReShade(*ppFactory);

    return result;
}

void DxgiHooks::Hook()
{
    std::lock_guard<std::mutex> lock(hookMutex);

    // If not spoofing and
    // using no frame generation (or Nukem's) and
    // not using DXGI spoofing we don't need DXGI hooks
    // Probably I forgot something but we can add it later
    if (!Config::Instance()->OverlayMenu.value_or_default() &&
        (Config::Instance()->FGInput.value_or_default() == FGInput::NoFG ||
         Config::Instance()->FGInput.value_or_default() == FGInput::Nukems) &&
        !Config::Instance()->DxgiSpoofing.value_or_default())
    {
        return;
    }

    if (o_CreateDXGIFactory != nullptr)
        return;

    LOG_DEBUG("");

    o_CreateDXGIFactory = DxgiProxy::Hook_CreateDxgiFactory(hkCreateDXGIFactory);
    o_CreateDXGIFactory1 = DxgiProxy::Hook_CreateDxgiFactory1(hkCreateDXGIFactory1);
    o_CreateDXGIFactory2 = DxgiProxy::Hook_CreateDxgiFactory2(hkCreateDXGIFactory2);
}
