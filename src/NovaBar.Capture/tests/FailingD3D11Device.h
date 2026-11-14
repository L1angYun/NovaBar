#pragma once
#include <d3d11.h>
#include <wrl/client.h>
// Test boundary wraps real D3D11 allocation; it never removes/resets the device.
class FailingD3D11Device final : public ID3D11Device {
    ULONG References=1;
public:
    Microsoft::WRL::ComPtr<ID3D11Device> Real;
    unsigned FailTextureAt=0,TextureCalls=0,FailViewAt=0,ViewCalls=0;
    explicit FailingD3D11Device(ID3D11Device* real):Real(real) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out) override {
        if(!out)return E_POINTER;
        if(id==__uuidof(IUnknown)||id==__uuidof(ID3D11Device)){*out=static_cast<ID3D11Device*>(this);AddRef();return S_OK;}
        return Real->QueryInterface(id,out);
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return ++References;}
    ULONG STDMETHODCALLTYPE Release() override {auto left=--References;if(!left)delete this;return left;}
    HRESULT STDMETHODCALLTYPE CreateBuffer(const D3D11_BUFFER_DESC *pDesc, const D3D11_SUBRESOURCE_DATA *pInitialData, ID3D11Buffer **ppBuffer) override { return Real->CreateBuffer(pDesc,pInitialData,ppBuffer); }
    HRESULT STDMETHODCALLTYPE CreateTexture1D(const D3D11_TEXTURE1D_DESC *pDesc, const D3D11_SUBRESOURCE_DATA *pInitialData, ID3D11Texture1D **ppTexture1D) override { return Real->CreateTexture1D(pDesc,pInitialData,ppTexture1D); }
    HRESULT STDMETHODCALLTYPE CreateTexture2D(const D3D11_TEXTURE2D_DESC *pDesc, const D3D11_SUBRESOURCE_DATA *pInitialData, ID3D11Texture2D **ppTexture2D) override { if(FailTextureAt && ++TextureCalls==FailTextureAt){if(ppTexture2D)*ppTexture2D=nullptr;return E_OUTOFMEMORY;} return Real->CreateTexture2D(pDesc,pInitialData,ppTexture2D); }
    HRESULT STDMETHODCALLTYPE CreateTexture3D(const D3D11_TEXTURE3D_DESC *pDesc, const D3D11_SUBRESOURCE_DATA *pInitialData, ID3D11Texture3D **ppTexture3D) override { return Real->CreateTexture3D(pDesc,pInitialData,ppTexture3D); }
    HRESULT STDMETHODCALLTYPE CreateShaderResourceView(ID3D11Resource *pResource, const D3D11_SHADER_RESOURCE_VIEW_DESC *pDesc, ID3D11ShaderResourceView **ppSRView) override { if(FailViewAt && ++ViewCalls==FailViewAt){if(ppSRView)*ppSRView=nullptr;return E_OUTOFMEMORY;} return Real->CreateShaderResourceView(pResource,pDesc,ppSRView); }
    HRESULT STDMETHODCALLTYPE CreateUnorderedAccessView(ID3D11Resource *pResource, const D3D11_UNORDERED_ACCESS_VIEW_DESC *pDesc, ID3D11UnorderedAccessView **ppUAView) override { return Real->CreateUnorderedAccessView(pResource,pDesc,ppUAView); }
    HRESULT STDMETHODCALLTYPE CreateRenderTargetView(ID3D11Resource *pResource, const D3D11_RENDER_TARGET_VIEW_DESC *pDesc, ID3D11RenderTargetView **ppRTView) override { return Real->CreateRenderTargetView(pResource,pDesc,ppRTView); }
    HRESULT STDMETHODCALLTYPE CreateDepthStencilView(ID3D11Resource *pResource, const D3D11_DEPTH_STENCIL_VIEW_DESC *pDesc, ID3D11DepthStencilView **ppDepthStencilView) override { return Real->CreateDepthStencilView(pResource,pDesc,ppDepthStencilView); }
    HRESULT STDMETHODCALLTYPE CreateInputLayout(const D3D11_INPUT_ELEMENT_DESC *pInputElementDescs, UINT NumElements, const void *pShaderBytecodeWithInputSignature, SIZE_T BytecodeLength, ID3D11InputLayout **ppInputLayout) override { return Real->CreateInputLayout(pInputElementDescs,NumElements,pShaderBytecodeWithInputSignature,BytecodeLength,ppInputLayout); }
    HRESULT STDMETHODCALLTYPE CreateVertexShader(const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage *pClassLinkage, ID3D11VertexShader **ppVertexShader) override { return Real->CreateVertexShader(pShaderBytecode,BytecodeLength,pClassLinkage,ppVertexShader); }
    HRESULT STDMETHODCALLTYPE CreateGeometryShader(const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage *pClassLinkage, ID3D11GeometryShader **ppGeometryShader) override { return Real->CreateGeometryShader(pShaderBytecode,BytecodeLength,pClassLinkage,ppGeometryShader); }
    HRESULT STDMETHODCALLTYPE CreateGeometryShaderWithStreamOutput(const void *pShaderBytecode, SIZE_T BytecodeLength, const D3D11_SO_DECLARATION_ENTRY *pSODeclaration, UINT NumEntries, const UINT *pBufferStrides, UINT NumStrides, UINT RasterizedStream, ID3D11ClassLinkage *pClassLinkage, ID3D11GeometryShader **ppGeometryShader) override { return Real->CreateGeometryShaderWithStreamOutput(pShaderBytecode,BytecodeLength,pSODeclaration,NumEntries,pBufferStrides,NumStrides,RasterizedStream,pClassLinkage,ppGeometryShader); }
    HRESULT STDMETHODCALLTYPE CreatePixelShader(const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage *pClassLinkage, ID3D11PixelShader **ppPixelShader) override { return Real->CreatePixelShader(pShaderBytecode,BytecodeLength,pClassLinkage,ppPixelShader); }
    HRESULT STDMETHODCALLTYPE CreateHullShader(const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage *pClassLinkage, ID3D11HullShader **ppHullShader) override { return Real->CreateHullShader(pShaderBytecode,BytecodeLength,pClassLinkage,ppHullShader); }
    HRESULT STDMETHODCALLTYPE CreateDomainShader(const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage *pClassLinkage, ID3D11DomainShader **ppDomainShader) override { return Real->CreateDomainShader(pShaderBytecode,BytecodeLength,pClassLinkage,ppDomainShader); }
    HRESULT STDMETHODCALLTYPE CreateComputeShader(const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage *pClassLinkage, ID3D11ComputeShader **ppComputeShader) override { return Real->CreateComputeShader(pShaderBytecode,BytecodeLength,pClassLinkage,ppComputeShader); }
    HRESULT STDMETHODCALLTYPE CreateClassLinkage(ID3D11ClassLinkage **ppLinkage) override { return Real->CreateClassLinkage(ppLinkage); }
    HRESULT STDMETHODCALLTYPE CreateBlendState(const D3D11_BLEND_DESC *pBlendStateDesc, ID3D11BlendState **ppBlendState) override { return Real->CreateBlendState(pBlendStateDesc,ppBlendState); }
    HRESULT STDMETHODCALLTYPE CreateDepthStencilState(const D3D11_DEPTH_STENCIL_DESC *pDepthStencilDesc, ID3D11DepthStencilState **ppDepthStencilState) override { return Real->CreateDepthStencilState(pDepthStencilDesc,ppDepthStencilState); }
    HRESULT STDMETHODCALLTYPE CreateRasterizerState(const D3D11_RASTERIZER_DESC *pRasterizerDesc, ID3D11RasterizerState **ppRasterizerState) override { return Real->CreateRasterizerState(pRasterizerDesc,ppRasterizerState); }
    HRESULT STDMETHODCALLTYPE CreateSamplerState(const D3D11_SAMPLER_DESC *pSamplerDesc, ID3D11SamplerState **ppSamplerState) override { return Real->CreateSamplerState(pSamplerDesc,ppSamplerState); }
    HRESULT STDMETHODCALLTYPE CreateQuery(const D3D11_QUERY_DESC *pQueryDesc, ID3D11Query **ppQuery) override { return Real->CreateQuery(pQueryDesc,ppQuery); }
    HRESULT STDMETHODCALLTYPE CreatePredicate(const D3D11_QUERY_DESC *pPredicateDesc, ID3D11Predicate **ppPredicate) override { return Real->CreatePredicate(pPredicateDesc,ppPredicate); }
    HRESULT STDMETHODCALLTYPE CreateCounter(const D3D11_COUNTER_DESC *pCounterDesc, ID3D11Counter **ppCounter) override { return Real->CreateCounter(pCounterDesc,ppCounter); }
    HRESULT STDMETHODCALLTYPE CreateDeferredContext(UINT ContextFlags, ID3D11DeviceContext **ppDeferredContext) override { return Real->CreateDeferredContext(ContextFlags,ppDeferredContext); }
    HRESULT STDMETHODCALLTYPE OpenSharedResource(HANDLE hResource, REFIID ReturnedInterface, void **ppResource) override { return Real->OpenSharedResource(hResource,ReturnedInterface,ppResource); }
    HRESULT STDMETHODCALLTYPE CheckFormatSupport(DXGI_FORMAT Format, UINT *pFormatSupport) override { return Real->CheckFormatSupport(Format,pFormatSupport); }
    HRESULT STDMETHODCALLTYPE CheckMultisampleQualityLevels(DXGI_FORMAT Format, UINT SampleCount, UINT *pNumQualityLevels) override { return Real->CheckMultisampleQualityLevels(Format,SampleCount,pNumQualityLevels); }
    void STDMETHODCALLTYPE CheckCounterInfo(D3D11_COUNTER_INFO *pCounterInfo) override { return Real->CheckCounterInfo(pCounterInfo); }
    HRESULT STDMETHODCALLTYPE CheckCounter(const D3D11_COUNTER_DESC *pDesc, D3D11_COUNTER_TYPE *pType, UINT *pActiveCounters, LPSTR szName, UINT *pNameLength, LPSTR szUnits, UINT *pUnitsLength, LPSTR szDescription, UINT *pDescriptionLength) override { return Real->CheckCounter(pDesc,pType,pActiveCounters,szName,pNameLength,szUnits,pUnitsLength,szDescription,pDescriptionLength); }
    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(D3D11_FEATURE Feature, void *pFeatureSupportData, UINT FeatureSupportDataSize) override { return Real->CheckFeatureSupport(Feature,pFeatureSupportData,FeatureSupportDataSize); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *pDataSize, void *pData) override { return Real->GetPrivateData(guid,pDataSize,pData); }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void *pData) override { return Real->SetPrivateData(guid,DataSize,pData); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown *pData) override { return Real->SetPrivateDataInterface(guid,pData); }
    D3D_FEATURE_LEVEL STDMETHODCALLTYPE GetFeatureLevel(void) override { return Real->GetFeatureLevel(); }
    UINT STDMETHODCALLTYPE GetCreationFlags(void) override { return Real->GetCreationFlags(); }
    HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason(void) override { return Real->GetDeviceRemovedReason(); }
    void STDMETHODCALLTYPE GetImmediateContext(ID3D11DeviceContext **ppImmediateContext) override { return Real->GetImmediateContext(ppImmediateContext); }
    HRESULT STDMETHODCALLTYPE SetExceptionMode(UINT RaiseFlags) override { return Real->SetExceptionMode(RaiseFlags); }
    UINT STDMETHODCALLTYPE GetExceptionMode(void) override { return Real->GetExceptionMode(); }
};
