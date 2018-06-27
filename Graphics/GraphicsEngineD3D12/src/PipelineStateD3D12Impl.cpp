/*     Copyright 2015-2018 Egor Yusov
 *  
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 * 
 *     http://www.apache.org/licenses/LICENSE-2.0
 * 
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF ANY PROPRIETARY RIGHTS.
 *
 *  In no event and under no legal theory, whether in tort (including negligence), 
 *  contract, or otherwise, unless required by applicable law (such as deliberate 
 *  and grossly negligent acts) or agreed to in writing, shall any Contributor be
 *  liable for any damages, including any direct, indirect, special, incidental, 
 *  or consequential damages of any character arising as a result of this License or 
 *  out of the use or inability to use the software (including but not limited to damages 
 *  for loss of goodwill, work stoppage, computer failure or malfunction, or any and 
 *  all other commercial damages or losses), even if such Contributor has been advised 
 *  of the possibility of such damages.
 */

#include "pch.h"
#include <array>
#include "PipelineStateD3D12Impl.h"
#include "ShaderD3D12Impl.h"
#include "D3D12TypeConversions.h"
#include "RenderDeviceD3D12Impl.h"
#include "DXGITypeConversions.h"
#include "ShaderResourceBindingD3D12Impl.h"
#include "CommandContext.h"
#include "EngineMemory.h"
#include "StringTools.h"

namespace Diligent
{

class PrimitiveTopology_To_D3D12_PRIMITIVE_TOPOLOGY_TYPE
{
public:
    PrimitiveTopology_To_D3D12_PRIMITIVE_TOPOLOGY_TYPE()
    {
        m_Map[PRIMITIVE_TOPOLOGY_UNDEFINED]      = D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED;
        m_Map[PRIMITIVE_TOPOLOGY_TRIANGLE_LIST]  = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        m_Map[PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP] = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        m_Map[PRIMITIVE_TOPOLOGY_POINT_LIST]     = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        m_Map[PRIMITIVE_TOPOLOGY_LINE_LIST]      = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        for(int t = static_cast<int>(PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST); t < static_cast<int>(PRIMITIVE_TOPOLOGY_NUM_TOPOLOGIES); ++t)
            m_Map[t] = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    }
    
    D3D12_PRIMITIVE_TOPOLOGY_TYPE operator[](PRIMITIVE_TOPOLOGY Topology)const
    {
        return m_Map[static_cast<int>(Topology)];
    }

private:
    std::array<D3D12_PRIMITIVE_TOPOLOGY_TYPE, PRIMITIVE_TOPOLOGY_NUM_TOPOLOGIES> m_Map;
};

PipelineStateD3D12Impl :: PipelineStateD3D12Impl(IReferenceCounters*      pRefCounters,
                                                 RenderDeviceD3D12Impl*   pDeviceD3D12,
                                                 const PipelineStateDesc& PipelineDesc) : 
    TPipelineStateBase(pRefCounters, pDeviceD3D12, PipelineDesc),
    m_DummyVar(*this),
    m_SRBMemAllocator(GetRawAllocator()),
    m_pDefaultShaderResBinding(nullptr, STDDeleter<ShaderResourceBindingD3D12Impl, FixedBlockMemoryAllocator>(pDeviceD3D12->GetSRBAllocator()) )
{
    auto pd3d12Device = pDeviceD3D12->GetD3D12Device();
    
    m_RootSig.AllocateStaticSamplers( GetShaders(), GetNumShaders() );

    auto& ShaderResLayoutAllocator = GetRawAllocator();
    auto* pShaderResLayoutRawMem = ALLOCATE(ShaderResLayoutAllocator, "Raw memory for ShaderResourceLayoutD3D12", sizeof(ShaderResourceLayoutD3D12) * m_NumShaders);
    m_pShaderResourceLayouts = reinterpret_cast<ShaderResourceLayoutD3D12*>(pShaderResLayoutRawMem);
    for (Uint32 s=0; s < m_NumShaders; ++s)
    {
        auto* pShaderD3D12 = GetShader<ShaderD3D12Impl>(s);
        new (m_pShaderResourceLayouts+s) ShaderResourceLayoutD3D12(*this, GetRawAllocator());
        m_pShaderResourceLayouts[s].Initialize(pDeviceD3D12->GetD3D12Device(), pShaderD3D12->GetShaderResources(), GetRawAllocator(), nullptr, 0, nullptr, &m_RootSig);
    }
    m_RootSig.Finalize(pd3d12Device);

    if (PipelineDesc.IsComputePipeline)
    {
        auto& ComputePipeline = PipelineDesc.ComputePipeline;

        if( ComputePipeline.pCS == nullptr )
            LOG_ERROR_AND_THROW("Compute shader is not set in the pipeline desc");

        D3D12_COMPUTE_PIPELINE_STATE_DESC d3d12PSODesc = {};
        d3d12PSODesc.pRootSignature = nullptr;
        
        auto *pByteCode = ValidatedCast<ShaderD3D12Impl>(ComputePipeline.pCS)->GetShaderByteCode();
        d3d12PSODesc.CS.pShaderBytecode = pByteCode->GetBufferPointer();
        d3d12PSODesc.CS.BytecodeLength = pByteCode->GetBufferSize();

        // For single GPU operation, set this to zero. If there are multiple GPU nodes, 
        // set bits to identify the nodes (the device's physical adapters) for which the 
        // graphics pipeline state is to apply. Each bit in the mask corresponds to a single node. 
        d3d12PSODesc.NodeMask = 0;

        d3d12PSODesc.CachedPSO.pCachedBlob = nullptr;
        d3d12PSODesc.CachedPSO.CachedBlobSizeInBytes = 0;
        
        // The only valid bit is D3D12_PIPELINE_STATE_FLAG_TOOL_DEBUG, which can only be set on WARP devices.
        d3d12PSODesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

        d3d12PSODesc.pRootSignature = m_RootSig.GetD3D12RootSignature();

        HRESULT hr = pd3d12Device->CreateComputePipelineState(&d3d12PSODesc, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>( static_cast<ID3D12PipelineState**>(&m_pd3d12PSO)) );
        if(FAILED(hr))
            LOG_ERROR_AND_THROW("Failed to create pipeline state");
    }
    else
    {
        const auto& GraphicsPipeline = PipelineDesc.GraphicsPipeline;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d3d12PSODesc = {};
            
        for (Uint32 s=0; s < m_NumShaders; ++s)
        {
            auto* pShaderD3D12 = GetShader<ShaderD3D12Impl>(s);
            auto ShaderType = pShaderD3D12->GetDesc().ShaderType;
            D3D12_SHADER_BYTECODE *pd3d12ShaderBytecode = nullptr;
            switch(ShaderType)
            {
                case SHADER_TYPE_VERTEX:   pd3d12ShaderBytecode = &d3d12PSODesc.VS; break;
                case SHADER_TYPE_PIXEL:    pd3d12ShaderBytecode = &d3d12PSODesc.PS; break;
                case SHADER_TYPE_GEOMETRY: pd3d12ShaderBytecode = &d3d12PSODesc.GS; break;
                case SHADER_TYPE_HULL:     pd3d12ShaderBytecode = &d3d12PSODesc.HS; break;
                case SHADER_TYPE_DOMAIN:   pd3d12ShaderBytecode = &d3d12PSODesc.DS; break;
                default: UNEXPECTED("Unexpected shader type");
            }
            auto *pByteCode = pShaderD3D12->GetShaderByteCode();
            pd3d12ShaderBytecode->pShaderBytecode = pByteCode->GetBufferPointer();
            pd3d12ShaderBytecode->BytecodeLength  = pByteCode->GetBufferSize();
        }

        d3d12PSODesc.pRootSignature = m_RootSig.GetD3D12RootSignature();
        
        memset(&d3d12PSODesc.StreamOutput, 0, sizeof(d3d12PSODesc.StreamOutput));

        BlendStateDesc_To_D3D12_BLEND_DESC(GraphicsPipeline.BlendDesc, d3d12PSODesc.BlendState);
        // The sample mask for the blend state.
        d3d12PSODesc.SampleMask = GraphicsPipeline.SampleMask;
    
        RasterizerStateDesc_To_D3D12_RASTERIZER_DESC(GraphicsPipeline.RasterizerDesc, d3d12PSODesc.RasterizerState);
        DepthStencilStateDesc_To_D3D12_DEPTH_STENCIL_DESC(GraphicsPipeline.DepthStencilDesc, d3d12PSODesc.DepthStencilState);

        std::vector<D3D12_INPUT_ELEMENT_DESC, STDAllocatorRawMem<D3D12_INPUT_ELEMENT_DESC>> d312InputElements( STD_ALLOCATOR_RAW_MEM(D3D12_INPUT_ELEMENT_DESC, GetRawAllocator(), "Allocator for vector<D3D12_INPUT_ELEMENT_DESC>") );
        if (m_LayoutElements.size() > 0)
        {
            LayoutElements_To_D3D12_INPUT_ELEMENT_DESCs(m_LayoutElements, d312InputElements);
            d3d12PSODesc.InputLayout.NumElements = static_cast<UINT>(d312InputElements.size());
            d3d12PSODesc.InputLayout.pInputElementDescs = d312InputElements.data();
        }
        else
        {
            d3d12PSODesc.InputLayout.NumElements = 0;
            d3d12PSODesc.InputLayout.pInputElementDescs = nullptr;
        }

        d3d12PSODesc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
        static const PrimitiveTopology_To_D3D12_PRIMITIVE_TOPOLOGY_TYPE PrimTopologyToD3D12TopologyType;
        d3d12PSODesc.PrimitiveTopologyType = PrimTopologyToD3D12TopologyType[GraphicsPipeline.PrimitiveTopology];

        d3d12PSODesc.NumRenderTargets = GraphicsPipeline.NumRenderTargets;
        for (Uint32 rt = 0; rt < GraphicsPipeline.NumRenderTargets; ++rt)
            d3d12PSODesc.RTVFormats[rt] = TexFormatToDXGI_Format(GraphicsPipeline.RTVFormats[rt]);
        for (Uint32 rt = GraphicsPipeline.NumRenderTargets; rt < 8; ++rt)
            d3d12PSODesc.RTVFormats[rt] = TexFormatToDXGI_Format(GraphicsPipeline.RTVFormats[rt]);
        d3d12PSODesc.DSVFormat = TexFormatToDXGI_Format(GraphicsPipeline.DSVFormat);

        d3d12PSODesc.SampleDesc.Count = GraphicsPipeline.SmplDesc.Count;
        d3d12PSODesc.SampleDesc.Quality = GraphicsPipeline.SmplDesc.Quality;

        // For single GPU operation, set this to zero. If there are multiple GPU nodes, 
        // set bits to identify the nodes (the device's physical adapters) for which the 
        // graphics pipeline state is to apply. Each bit in the mask corresponds to a single node. 
        d3d12PSODesc.NodeMask = 0;

        d3d12PSODesc.CachedPSO.pCachedBlob = nullptr;
        d3d12PSODesc.CachedPSO.CachedBlobSizeInBytes = 0;

        // The only valid bit is D3D12_PIPELINE_STATE_FLAG_TOOL_DEBUG, which can only be set on WARP devices.
        d3d12PSODesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

        HRESULT hr = pd3d12Device->CreateGraphicsPipelineState(&d3d12PSODesc, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>( static_cast<ID3D12PipelineState**>(&m_pd3d12PSO)) );
        if(FAILED(hr))
            LOG_ERROR_AND_THROW("Failed to create pipeline state");
    }

    if (*m_Desc.Name != 0)
    {
        m_pd3d12PSO->SetName(WidenString(m_Desc.Name).c_str());
        String RootSignatureDesc("Root signature for PSO \"");
        RootSignatureDesc.append(m_Desc.Name);
        RootSignatureDesc.push_back('\"');
        m_RootSig.GetD3D12RootSignature()->SetName(WidenString(RootSignatureDesc).c_str());
    }

    if(PipelineDesc.SRBAllocationGranularity > 1)
    {
        std::array<size_t, MaxShadersInPipeline> ShaderResLayoutDataSizes = {};
        for (Uint32 s = 0; s < m_NumShaders; ++s)
        {
            std::array<SHADER_VARIABLE_TYPE, 3> AllowedVarTypes = { SHADER_VARIABLE_TYPE_STATIC, SHADER_VARIABLE_TYPE_MUTABLE, SHADER_VARIABLE_TYPE_DYNAMIC };
            ShaderResLayoutDataSizes[s] = ShaderResourceLayoutD3D12::GetRequiredMemorySize(m_pShaderResourceLayouts[s], AllowedVarTypes.data(), static_cast<Uint32>(AllowedVarTypes.size()));
        }

        auto CacheMemorySize = m_RootSig.GetResourceCacheRequiredMemSize();
        m_SRBMemAllocator.Initialize(PipelineDesc.SRBAllocationGranularity, m_NumShaders, ShaderResLayoutDataSizes.data(), 1, &CacheMemorySize);
    }

    auto& SRBAllocator = pDeviceD3D12->GetSRBAllocator();
    // Default shader resource binding must be initialized after resource layouts are parsed!
    m_pDefaultShaderResBinding.reset( NEW_RC_OBJ(SRBAllocator, "ShaderResourceBindingD3D12Impl instance", ShaderResourceBindingD3D12Impl, this)(this, true) );

    m_ShaderResourceLayoutHash = m_RootSig.GetHash();
}

PipelineStateD3D12Impl::~PipelineStateD3D12Impl()
{
    auto& ShaderResLayoutAllocator = GetRawAllocator();
    for(Uint32 s = 0; s < m_NumShaders; ++s)
    {
        m_pShaderResourceLayouts[s].~ShaderResourceLayoutD3D12();
    }
    ShaderResLayoutAllocator.Free(m_pShaderResourceLayouts);

    // D3D12 object can only be destroyed when it is no longer used by the GPU
    auto *pDeviceD3D12Impl = ValidatedCast<RenderDeviceD3D12Impl>(GetDevice());
    pDeviceD3D12Impl->SafeReleaseD3D12Object(m_pd3d12PSO);
}

IMPLEMENT_QUERY_INTERFACE( PipelineStateD3D12Impl, IID_PipelineStateD3D12, TPipelineStateBase )

void PipelineStateD3D12Impl::BindShaderResources(IResourceMapping* pResourceMapping, Uint32 Flags)
{
    if( m_Desc.IsComputePipeline )
    { 
        if(m_pCS)m_pCS->BindResources(pResourceMapping, Flags);
    }
    else
    {
        if(m_pVS)m_pVS->BindResources(pResourceMapping, Flags);
        if(m_pPS)m_pPS->BindResources(pResourceMapping, Flags);
        if(m_pGS)m_pGS->BindResources(pResourceMapping, Flags);
        if(m_pDS)m_pDS->BindResources(pResourceMapping, Flags);
        if(m_pHS)m_pHS->BindResources(pResourceMapping, Flags);
    }
}

void PipelineStateD3D12Impl::CreateShaderResourceBinding(IShaderResourceBinding** ppShaderResourceBinding)
{
    auto *pRenderDeviceD3D12 = ValidatedCast<RenderDeviceD3D12Impl>( GetDevice() );
    auto& SRBAllocator = pRenderDeviceD3D12->GetSRBAllocator();
    auto pResBindingD3D12 = NEW_RC_OBJ(SRBAllocator, "ShaderResourceBindingD3D12Impl instance", ShaderResourceBindingD3D12Impl)(this, false);
    pResBindingD3D12->QueryInterface(IID_ShaderResourceBinding, reinterpret_cast<IObject**>(ppShaderResourceBinding));
}

bool PipelineStateD3D12Impl::IsCompatibleWith(const IPipelineState* pPSO)const
{
    VERIFY_EXPR(pPSO != nullptr);

    if (pPSO == this)
        return true;

    const PipelineStateD3D12Impl *pPSOD3D12 = ValidatedCast<const PipelineStateD3D12Impl>(pPSO);
    if (m_ShaderResourceLayoutHash != pPSOD3D12->m_ShaderResourceLayoutHash)
        return false;

    auto IsSameRootSignature = m_RootSig.IsSameAs(pPSOD3D12->m_RootSig);

#ifdef _DEBUG
    {
        bool IsCompatibleShaders = true;
        if (m_NumShaders != pPSOD3D12->m_NumShaders)
            IsCompatibleShaders = false;

        if(IsCompatibleShaders)
        {
            for (Uint32 s = 0; s < m_NumShaders; ++s)
            {
                auto* pShader0 = GetShader<const ShaderD3D12Impl>(s);
                auto* pShader1 = pPSOD3D12->GetShader<const ShaderD3D12Impl>(s);
                if (pShader0->GetDesc().ShaderType != pShader1->GetDesc().ShaderType)
                {
                    IsCompatibleShaders = false;
                    break;
                }
                const ShaderResourcesD3D12 *pRes0 = pShader0->GetShaderResources().get();
                const ShaderResourcesD3D12 *pRes1 = pShader1->GetShaderResources().get();
                if (!pRes0->IsCompatibleWith(*pRes1))
                {
                    IsCompatibleShaders = false;
                    break;
                }
            }
        }

        if(IsCompatibleShaders)
            VERIFY(IsSameRootSignature, "Compatible shaders must have same root signatures");
    }
#endif
    
    return IsSameRootSignature;
}

ShaderResourceCacheD3D12* PipelineStateD3D12Impl::CommitAndTransitionShaderResources(IShaderResourceBinding* pShaderResourceBinding, 
                                                                                     CommandContext&         Ctx,
                                                                                     bool                    CommitResources,
                                                                                     bool                    TransitionResources)const
{
#ifdef VERIFY_SHADER_BINDINGS
    if (pShaderResourceBinding == nullptr &&
        (m_RootSig.GetTotalSrvCbvUavSlots(SHADER_VARIABLE_TYPE_MUTABLE) != 0 ||
         m_RootSig.GetTotalSrvCbvUavSlots(SHADER_VARIABLE_TYPE_DYNAMIC) != 0))
    {
        LOG_ERROR_MESSAGE("Pipeline state \"", m_Desc.Name, "\" contains mutable/dynamic shader variables and requires shader resource binding to commit all resources, but none is provided.");
    }
#endif

    // If the shaders contain no resources or static resources only, shader resource binding may be null. 
    // In this case use special internal SRB object
    auto *pResBindingD3D12Impl = pShaderResourceBinding ? ValidatedCast<ShaderResourceBindingD3D12Impl>(pShaderResourceBinding) : m_pDefaultShaderResBinding.get();
    
#ifdef VERIFY_SHADER_BINDINGS
    {
        auto *pRefPSO = pResBindingD3D12Impl->GetPipelineState();
        if ( IsIncompatibleWith(pRefPSO) )
        {
            LOG_ERROR_MESSAGE("Shader resource binding is incompatible with the pipeline state \"", m_Desc.Name, "\". Operation will be ignored.");
            return nullptr;
        }
    }
#endif

    // First time only, copy static shader resources to the cache
    if(!pResBindingD3D12Impl->StaticResourcesInitialized())
        pResBindingD3D12Impl->InitializeStaticResources(this);

#ifdef VERIFY_SHADER_BINDINGS
    pResBindingD3D12Impl->dbgVerifyResourceBindings(this);
#endif

    auto *pDeviceD3D12Impl = ValidatedCast<RenderDeviceD3D12Impl>( GetDevice() );
    auto& ResourceCache = pResBindingD3D12Impl->GetResourceCache();
    if(CommitResources)
    {
        if(m_Desc.IsComputePipeline)
            Ctx.AsComputeContext().SetRootSignature( GetD3D12RootSignature() );
        else
            Ctx.AsGraphicsContext().SetRootSignature( GetD3D12RootSignature() );

        if(TransitionResources)
            (m_RootSig.*m_RootSig.TransitionAndCommitDescriptorHandles)(pDeviceD3D12Impl, ResourceCache, Ctx, m_Desc.IsComputePipeline);
        else
            (m_RootSig.*m_RootSig.CommitDescriptorHandles)(pDeviceD3D12Impl, ResourceCache, Ctx, m_Desc.IsComputePipeline);
    }
    else
    {
        VERIFY(TransitionResources, "Resources should be transitioned or committed or both");
        m_RootSig.TransitionResources(ResourceCache, Ctx);
    }
    return &ResourceCache;
}


bool PipelineStateD3D12Impl::dbgContainsShaderResources()const
{
    return m_RootSig.GetTotalSrvCbvUavSlots(SHADER_VARIABLE_TYPE_STATIC) != 0 ||
           m_RootSig.GetTotalSrvCbvUavSlots(SHADER_VARIABLE_TYPE_MUTABLE) != 0 ||
           m_RootSig.GetTotalSrvCbvUavSlots(SHADER_VARIABLE_TYPE_DYNAMIC) != 0;
}

}
