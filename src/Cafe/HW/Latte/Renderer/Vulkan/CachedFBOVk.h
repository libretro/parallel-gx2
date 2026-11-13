#pragma once

#include "Cafe/HW/Latte/Core/LatteCachedFBO.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VKRBase.h"
#include "util/helpers/AtomicPtrBag.h"

// from a pool (PoolNew.h): made as a game draws to new targets
class CachedFBOVk : public LatteCachedFBO, public PoolNew<CachedFBOVk, 64>
{
public:
	CachedFBOVk(uint64 key, VkDevice device)
		: LatteCachedFBO(key), m_device(device)
	{
		CreateRenderPass();
		CreateFramebuffer();
		InitDynamicRenderingData();
	}

	~CachedFBOVk();

	VKRObjectFramebuffer* GetFramebufferObj() const
	{
		return m_vkrObjFramebuffer;
	}

	VKRObjectRenderPass* GetRenderPassObj() const
	{
		return m_vkrObjRenderPass;
	}

	// for KHR_dynamic_rendering
	VkRenderingInfoKHR* GetRenderingInfo()
	{
		return &m_vkRenderingInfo;
	}


	// Registered from the pipeline cache's compile threads as well as the GPU
	// thread: no lock, AtomicPtrBag.h
	void TrackDependency(class PipelineInfo* pipelineInfo)
	{
		m_usedByPipelines.Add(pipelineInfo);
	}

	void RemoveDependency(class PipelineInfo* pipelineInfo)
	{
		m_usedByPipelines.Remove(pipelineInfo);
	}

	[[nodiscard]] const VkExtent2D& GetExtend() const { return m_extend;}

	struct RendertargetSelfDependencyMask
	{
		VkImageAspectFlags aspectMaskFlags{}; // aspect flags which are simultaneously sampled and written
		bool hasNonPixelSelfDependency{false};

		VkImageAspectFlags GetAspectMask() const
		{
			return aspectMaskFlags;
		}

		bool HasSelfDependency() const
		{
			return GetAspectMask() != 0;
		}

		bool HasVertexOrGeometrySelfDependency() const
		{
			return hasNonPixelSelfDependency; // vertex or geometry shader samples texture which is written to
		}
	};

	// checks if any of the sampled textures are output by the FBO
	RendertargetSelfDependencyMask CheckForSelfDependency(VkDescriptorSetInfo* vsDS, VkDescriptorSetInfo* gsDS, VkDescriptorSetInfo* psDS) const;

private:

	void CreateRenderPass();
	void CreateFramebuffer();

	void InitDynamicRenderingData();

	VKRObjectTextureView* GetColorBufferImageView(uint32 index);
	VKRObjectTextureView* GetDepthStencilBufferImageView(bool& hasStencil);

	VkDevice m_device;
	VKRObjectRenderPass* m_vkrObjRenderPass{};
	VKRObjectFramebuffer* m_vkrObjFramebuffer{};

	VkExtent2D m_extend;

	// for KHR_dynamic_rendering
	VkRenderingInfoKHR m_vkRenderingInfo;
	VkRenderingAttachmentInfoKHR m_vkColorAttachments[8];
	VkRenderingAttachmentInfoKHR m_vkDepthAttachment;
	VkRenderingAttachmentInfoKHR m_vkStencilAttachment;

	AtomicPtrBag<class PipelineInfo> m_usedByPipelines; // PipelineInfo objects which use this renderpass/framebuffer
};
