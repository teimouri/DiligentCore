/*
 *  Copyright 2019-2020 Diligent Graphics LLC
 *  Copyright 2015-2019 Egor Yusov
 *  
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *  
 *      http://www.apache.org/licenses/LICENSE-2.0
 *  
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
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

#pragma once

/// \file
/// Declaration of Diligent::PipelineStateVkImpl class

#include <array>

#include "RenderDeviceVk.h"
#include "PipelineStateVk.h"
#include "PipelineStateBase.h"
#include "PipelineLayout.h"
#include "ShaderResourceLayoutVk.h"
#include "ShaderVariableVk.h"
#include "FixedBlockMemoryAllocator.h"
#include "SRBMemoryAllocator.h"
#include "VulkanUtilities/VulkanObjectWrappers.h"
#include "VulkanUtilities/VulkanCommandBuffer.h"
#include "PipelineLayout.h"
#include "RenderDeviceVkImpl.h"

namespace Diligent
{

class FixedBlockMemoryAllocator;
class ShaderVariableManagerVk;

/// Pipeline state object implementation in Vulkan backend.
class PipelineStateVkImpl final : public PipelineStateBase<IPipelineStateVk, RenderDeviceVkImpl>
{
public:
    using TPipelineStateBase = PipelineStateBase<IPipelineStateVk, RenderDeviceVkImpl>;

    PipelineStateVkImpl(IReferenceCounters* pRefCounters, RenderDeviceVkImpl* pDeviceVk, const PipelineStateDesc& PipelineDesc);
    ~PipelineStateVkImpl();

    virtual void QueryInterface(const INTERFACE_ID& IID, IObject** ppInterface) override final;

    /// Implementation of IPipelineState::CreateShaderResourceBinding() in Vulkan backend.
    virtual void CreateShaderResourceBinding(IShaderResourceBinding** ppShaderResourceBinding, bool InitStaticResources) override final;

    /// Implementation of IPipelineState::IsCompatibleWith() in Vulkan backend.
    virtual bool IsCompatibleWith(const IPipelineState* pPSO) const override final;

    /// Implementation of IPipelineStateVk::GetVkRenderPass().
    virtual VkRenderPass GetVkRenderPass() const override final { return m_RenderPass; }

    /// Implementation of IPipelineStateVk::GetVkPipeline().
    virtual VkPipeline GetVkPipeline() const override final { return m_Pipeline; }

    /// Implementation of IPipelineState::BindStaticResources() in Vulkan backend.
    virtual void BindStaticResources(Uint32 ShaderFlags, IResourceMapping* pResourceMapping, Uint32 Flags) override final;

    /// Implementation of IPipelineState::GetStaticVariableCount() in Vulkan backend.
    virtual Uint32 GetStaticVariableCount(SHADER_TYPE ShaderType) const override final;

    /// Implementation of IPipelineState::GetStaticVariableByName() in Vulkan backend.
    virtual IShaderResourceVariable* GetStaticVariableByName(SHADER_TYPE ShaderType, const Char* Name) override final;

    /// Implementation of IPipelineState::GetStaticVariableByIndex() in Vulkan backend.
    virtual IShaderResourceVariable* GetStaticVariableByIndex(SHADER_TYPE ShaderType, Uint32 Index) override final;

    void CommitAndTransitionShaderResources(IShaderResourceBinding*                pShaderResourceBinding,
                                            DeviceContextVkImpl*                   pCtxVkImpl,
                                            bool                                   CommitResources,
                                            RESOURCE_STATE_TRANSITION_MODE         StateTransitionMode,
                                            PipelineLayout::DescriptorSetBindInfo* pDescrSetBindInfo) const;

    __forceinline void BindDescriptorSetsWithDynamicOffsets(VulkanUtilities::VulkanCommandBuffer&  CmdBuffer,
                                                            Uint32                                 CtxId,
                                                            DeviceContextVkImpl*                   pCtxVkImpl,
                                                            PipelineLayout::DescriptorSetBindInfo& BindInfo)
    {
        m_PipelineLayout.BindDescriptorSetsWithDynamicOffsets(CmdBuffer, CtxId, pCtxVkImpl, BindInfo);
    }

    const PipelineLayout& GetPipelineLayout() const { return m_PipelineLayout; }

    const ShaderResourceLayoutVk& GetShaderResLayout(Uint32 ShaderInd) const
    {
        VERIFY_EXPR(ShaderInd < m_NumShaders);
        return m_ShaderResourceLayouts[ShaderInd];
    }

    SRBMemoryAllocator& GetSRBMemoryAllocator()
    {
        return m_SRBMemAllocator;
    }

    static VkRenderPassCreateInfo GetRenderPassCreateInfo(Uint32                                                     NumRenderTargets,
                                                          const TEXTURE_FORMAT                                       RTVFormats[],
                                                          TEXTURE_FORMAT                                             DSVFormat,
                                                          Uint32                                                     SampleCount,
                                                          std::array<VkAttachmentDescription, MaxRenderTargets + 1>& Attachments,
                                                          std::array<VkAttachmentReference, MaxRenderTargets + 1>&   AttachmentReferences,
                                                          VkSubpassDescription&                                      SubpassDesc);


    void InitializeStaticSRBResources(ShaderResourceCacheVk& ResourceCache) const;

private:
    const ShaderResourceLayoutVk& GetStaticShaderResLayout(Uint32 ShaderInd) const
    {
        VERIFY_EXPR(ShaderInd < m_NumShaders);
        return m_ShaderResourceLayouts[m_NumShaders + ShaderInd];
    }

    const ShaderResourceCacheVk& GetStaticResCache(Uint32 ShaderInd) const
    {
        VERIFY_EXPR(ShaderInd < m_NumShaders);
        return m_StaticResCaches[ShaderInd];
    }

    ShaderVariableManagerVk& GetStaticVarMgr(Uint32 ShaderInd) const
    {
        VERIFY_EXPR(ShaderInd < m_NumShaders);
        return m_StaticVarsMgrs[ShaderInd];
    }

    ShaderResourceLayoutVk*  m_ShaderResourceLayouts = nullptr;
    ShaderResourceCacheVk*   m_StaticResCaches       = nullptr;
    ShaderVariableManagerVk* m_StaticVarsMgrs        = nullptr;

    // SRB memory allocator must be declared before m_pDefaultShaderResBinding
    SRBMemoryAllocator m_SRBMemAllocator;

    std::array<VulkanUtilities::ShaderModuleWrapper, MaxShadersInPipeline> m_ShaderModules;

    VkRenderPass                     m_RenderPass = VK_NULL_HANDLE; // Render passes are managed by the render device
    VulkanUtilities::PipelineWrapper m_Pipeline;
    PipelineLayout                   m_PipelineLayout;

    Int8 m_ResourceLayoutIndex[6] = {-1, -1, -1, -1, -1, -1};
    bool m_HasStaticResources     = false;
    bool m_HasNonStaticResources  = false;
};

} // namespace Diligent
