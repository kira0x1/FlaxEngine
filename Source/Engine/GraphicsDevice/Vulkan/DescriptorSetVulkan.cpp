// Copyright (c) 2012-2020 Wojciech Figat. All rights reserved.

#if GRAPHICS_API_VULKAN

#include "DescriptorSetVulkan.h"
#include "Engine/Core/Collections/Dictionary.h"
#include "Engine/Utilities/Crc.h"
#include "GPUDeviceVulkan.h"
#include "RenderToolsVulkan.h"
#include "GPUContextVulkan.h"
#include "GPUAdapterVulkan.h"
#include "CmdBufferVulkan.h"
#include "Engine/Threading/Threading.h"
#if VULKAN_USE_DESCRIPTOR_POOL_MANAGER
#include "Engine/Engine/Engine.h"
#endif

#if VULKAN_USE_DESCRIPTOR_POOL_MANAGER

void DescriptorSetLayoutInfoVulkan::CacheTypesUsageID()
{
    static CriticalSection locker;
    static uint32 uniqueID = 1;
    static Dictionary<uint32, uint32> typesUsageHashMap;

    const uint32 typesUsageHash = Crc::MemCrc32(LayoutTypes, sizeof(LayoutTypes));
    ScopeLock lock(locker);
    uint32 id;
    if (!typesUsageHashMap.TryGet(typesUsageHash, id))
    {
        id = uniqueID++;
        typesUsageHashMap.Add(typesUsageHash, id);
    }
    _typesUsageID = id;
}

#endif

void DescriptorSetLayoutInfoVulkan::AddDescriptor(int32 descriptorSetIndex, const VkDescriptorSetLayoutBinding& descriptor)
{
    // Increment type usage
    LayoutTypes[descriptor.descriptorType]++;

    if (descriptorSetIndex >= SetLayouts.Count())
    {
        SetLayouts.Resize(descriptorSetIndex + 1);
    }

    SetLayout& descSetLayout = SetLayouts[descriptorSetIndex];
    descSetLayout.LayoutBindings.Add(descriptor);

    // TODO: manual hash update method?
    _hash = Crc::MemCrc32(&descriptor, sizeof(descriptor), _hash);
}

void DescriptorSetLayoutInfoVulkan::AddBindingsForStage(VkShaderStageFlagBits stageFlags, DescriptorSet::Stage descSet, const SpirvShaderDescriptorInfo* descriptorInfo)
{
    const int32 descriptorSetIndex = (int32)descSet;

    VkDescriptorSetLayoutBinding binding;
    binding.descriptorCount = 1;
    binding.stageFlags = stageFlags;
    binding.pImmutableSamplers = nullptr;

    for (uint32 descriptorIndex = 0; descriptorIndex < descriptorInfo->DescriptorTypesCount; descriptorIndex++)
    {
        auto& descriptor = descriptorInfo->DescriptorTypes[descriptorIndex];
        binding.binding = descriptorIndex;
        binding.descriptorType = descriptor.DescriptorType;
        AddDescriptor(descriptorSetIndex, binding);
    }
}

DescriptorSetLayoutVulkan::DescriptorSetLayoutVulkan(GPUDeviceVulkan* device)
    : _device(device)
{
}

DescriptorSetLayoutVulkan::~DescriptorSetLayoutVulkan()
{
    for (VkDescriptorSetLayout& handle : _handles)
    {
        _device->DeferredDeletionQueue.EnqueueResource(DeferredDeletionQueueVulkan::Type::DescriptorSetLayout, handle);
    }
}

void DescriptorSetLayoutVulkan::Compile()
{
    ASSERT(_handles.IsEmpty());

    // Check if we obey limits
    const VkPhysicalDeviceLimits& limits = _device->PhysicalDeviceLimits;

    // Check for maxDescriptorSetSamplers
    ASSERT(LayoutTypes[VK_DESCRIPTOR_TYPE_SAMPLER]
        + LayoutTypes[VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER]
        < limits.maxDescriptorSetSamplers);

    // Check for maxDescriptorSetUniformBuffers
    ASSERT(LayoutTypes[VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER]
        + LayoutTypes[VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC]
        < limits.maxDescriptorSetUniformBuffers);

    // Check for maxDescriptorSetUniformBuffersDynamic
    if (!_device->Adapter->IsAMD())
    {
        ASSERT(LayoutTypes[VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC]
            < limits.maxDescriptorSetUniformBuffersDynamic);
    }

    // Check for maxDescriptorSetStorageBuffers
    ASSERT(LayoutTypes[VK_DESCRIPTOR_TYPE_STORAGE_BUFFER]
        + LayoutTypes[VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC]
        < limits.maxDescriptorSetStorageBuffers);

    // Check for maxDescriptorSetStorageBuffersDynamic
    if (LayoutTypes[VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC] > limits.maxDescriptorSetUniformBuffersDynamic)
    {
        // TODO: Downgrade to non-dynamic?
    }
    ASSERT(LayoutTypes[VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC]
        < limits.maxDescriptorSetStorageBuffersDynamic);

    // Check for maxDescriptorSetSampledImages
    ASSERT(LayoutTypes[VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER]
        + LayoutTypes[VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE]
        + LayoutTypes[VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER]
        < limits.maxDescriptorSetSampledImages);

    // Check for maxDescriptorSetStorageImages
    ASSERT(LayoutTypes[VK_DESCRIPTOR_TYPE_STORAGE_IMAGE]
        + LayoutTypes[VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER]
        < limits.maxDescriptorSetStorageImages);

    _handles.Resize(SetLayouts.Count());

    for (int32 i = 0; i < SetLayouts.Count(); i++)
    {
        auto& layout = SetLayouts[i];

        VkDescriptorSetLayoutCreateInfo layoutInfo;
        RenderToolsVulkan::ZeroStruct(layoutInfo, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
        layoutInfo.bindingCount = layout.LayoutBindings.Count();
        layoutInfo.pBindings = layout.LayoutBindings.Get();

        VALIDATE_VULKAN_RESULT(vkCreateDescriptorSetLayout(_device->Device, &layoutInfo, nullptr, &_handles[i]));
    }

#if VULKAN_USE_DESCRIPTOR_POOL_MANAGER
    if (_typesUsageID == ~0)
    {
        CacheTypesUsageID();
    }

    RenderToolsVulkan::ZeroStruct(_allocateInfo, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
    _allocateInfo.descriptorSetCount = _handles.Count();
    _allocateInfo.pSetLayouts = _handles.Get();
#endif
}

#if VULKAN_USE_DESCRIPTOR_POOL_MANAGER
DescriptorPoolVulkan::DescriptorPoolVulkan(GPUDeviceVulkan* device, const DescriptorSetLayoutVulkan& layout)
#else
DescriptorPoolVulkan::DescriptorPoolVulkan(GPUDeviceVulkan* device)
#endif
    : _device(device)
    , _handle(VK_NULL_HANDLE)
    , MaxDescriptorSets(0)
    , NumAllocatedDescriptorSets(0)
    , PeakAllocatedDescriptorSets(0)
#if VULKAN_USE_DESCRIPTOR_POOL_MANAGER
    , Layout(layout)
#endif
{
    Array<VkDescriptorPoolSize, FixedAllocation<VULKAN_DESCRIPTOR_TYPE_END + 1>> types;

#if VULKAN_USE_DESCRIPTOR_POOL_MANAGER
    // Max number of descriptor sets layout allocations
    const uint32 MaxSetsAllocations = 256;

    // Descriptor sets number required to allocate the max number of descriptor sets layout.
    // When we're hashing pools with types usage ID the descriptor pool can be used for different layouts so the initial layout does not make much sense.
    // In the latter case we'll be probably over-allocating the descriptor types but given the relatively small number of max allocations this should not have
    // a serious impact.
    MaxDescriptorSets = MaxSetsAllocations * (VULKAN_HASH_POOLS_WITH_TYPES_USAGE_ID ? 1 : Layout.GetLayouts().Count());
    for (uint32 typeIndex = VULKAN_DESCRIPTOR_TYPE_BEGIN; typeIndex <= VULKAN_DESCRIPTOR_TYPE_END; typeIndex++)
    {
        const VkDescriptorType descriptorType = (VkDescriptorType)typeIndex;
        const uint32 typesUsed = Layout.GetTypesUsed(descriptorType);
        if (typesUsed > 0)
        {
            VkDescriptorPoolSize& type = types.AddOne();
            Platform::MemoryClear(&type, sizeof(type));
            type.type = descriptorType;
            type.descriptorCount = typesUsed * MaxSetsAllocations;
        }
    }
#else
	MaxDescriptorSets = 16384;

	Platform::MemoryClear(MaxAllocatedTypes, sizeof(MaxAllocatedTypes));
	Platform::MemoryClear(NumAllocatedTypes, sizeof(NumAllocatedTypes));
	Platform::MemoryClear(PeakAllocatedTypes, sizeof(PeakAllocatedTypes));

	VkDescriptorPoolSize type;

	type.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	type.descriptorCount = 2048;
	types.Add(type);

	type.type = VK_DESCRIPTOR_TYPE_SAMPLER;
	type.descriptorCount = 1024;
	types.Add(type);

	type.type = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
	type.descriptorCount = 512;
	types.Add(type);

	type.type = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
	type.descriptorCount = 512;
	types.Add(type);

	type.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	type.descriptorCount = 512;
	types.Add(type);

	type.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	type.descriptorCount = 512;
	types.Add(type);

	type.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	type.descriptorCount = 2048;
	types.Add(type);

	for (const VkDescriptorPoolSize& poolSize : types)
	{
		MaxAllocatedTypes[poolSize.type] = poolSize.descriptorCount;
	}
#endif

    VkDescriptorPoolCreateInfo createInfo;
    RenderToolsVulkan::ZeroStruct(createInfo, VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
    createInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    createInfo.poolSizeCount = types.Count();
    createInfo.pPoolSizes = types.Get();
    createInfo.maxSets = MaxDescriptorSets;
    VALIDATE_VULKAN_RESULT(vkCreateDescriptorPool(_device->Device, &createInfo, nullptr, &_handle));
}

DescriptorPoolVulkan::~DescriptorPoolVulkan()
{
    if (_handle != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(_device->Device, _handle, nullptr);
    }
}

void DescriptorPoolVulkan::TrackAddUsage(const DescriptorSetLayoutVulkan& layout)
{
    // Check and increment our current type usage
    for (uint32 typeIndex = VULKAN_DESCRIPTOR_TYPE_BEGIN; typeIndex <= VULKAN_DESCRIPTOR_TYPE_END; typeIndex++)
    {
#if VULKAN_USE_DESCRIPTOR_POOL_MANAGER
        ASSERT(Layout.GetTypesUsed((VkDescriptorType)typeIndex) == layout.GetTypesUsed((VkDescriptorType)typeIndex));
#else
		NumAllocatedTypes[typeIndex] += (int32)layout.GetTypesUsed((VkDescriptorType)typeIndex);
		PeakAllocatedTypes[typeIndex] = Math::Max(PeakAllocatedTypes[typeIndex], NumAllocatedTypes[typeIndex]);
#endif
    }

    NumAllocatedDescriptorSets += layout.GetLayouts().Count();
    PeakAllocatedDescriptorSets = Math::Max(NumAllocatedDescriptorSets, PeakAllocatedDescriptorSets);
}

void DescriptorPoolVulkan::TrackRemoveUsage(const DescriptorSetLayoutVulkan& layout)
{
    // Check and increment our current type usage
    for (uint32 typeIndex = VULKAN_DESCRIPTOR_TYPE_BEGIN; typeIndex <= VULKAN_DESCRIPTOR_TYPE_END; typeIndex++)
    {
#if VULKAN_USE_DESCRIPTOR_POOL_MANAGER
        ASSERT(Layout.GetTypesUsed((VkDescriptorType)typeIndex) == layout.GetTypesUsed((VkDescriptorType)typeIndex));
#else
		NumAllocatedTypes[typeIndex] -= (int32)layout.GetTypesUsed((VkDescriptorType)typeIndex);
		ASSERT(NumAllocatedTypes[typeIndex] >= 0);
#endif
    }

    NumAllocatedDescriptorSets -= layout.GetLayouts().Count();
}

#if VULKAN_USE_DESCRIPTOR_POOL_MANAGER

void DescriptorPoolVulkan::Reset()
{
    if (_handle != VK_NULL_HANDLE)
    {
        VALIDATE_VULKAN_RESULT(vkResetDescriptorPool(_device->Device, _handle, 0));
    }
    NumAllocatedDescriptorSets = 0;
}

bool DescriptorPoolVulkan::AllocateDescriptorSets(const VkDescriptorSetAllocateInfo& descriptorSetAllocateInfo, VkDescriptorSet* result)
{
    VkDescriptorSetAllocateInfo allocateInfo = descriptorSetAllocateInfo;
    allocateInfo.descriptorPool = _handle;
    return vkAllocateDescriptorSets(_device->Device, &allocateInfo, result) == VK_SUCCESS;
}

TypedDescriptorPoolSetVulkan::~TypedDescriptorPoolSetVulkan()
{
    for (auto pool = _poolListHead; pool;)
    {
        const auto next = pool->Next;
        Delete(pool->Element);
        Delete(pool);
        pool = next;
    }
}

bool TypedDescriptorPoolSetVulkan::AllocateDescriptorSets(const DescriptorSetLayoutVulkan& layout, VkDescriptorSet* outSets)
{
    const auto& layoutHandles = layout.GetHandles();
    if (layoutHandles.HasItems())
    {
        auto* pool = _poolListCurrent->Element;
        while (!pool->AllocateDescriptorSets(layout.GetAllocateInfo(), outSets))
        {
            pool = GetFreePool(true);
        }
        return true;
    }
    return true;
}

DescriptorPoolVulkan* TypedDescriptorPoolSetVulkan::GetFreePool(bool forceNewPool)
{
    if (!forceNewPool)
    {
        return _poolListCurrent->Element;
    }
    if (_poolListCurrent->Next)
    {
        _poolListCurrent = _poolListCurrent->Next;
        return _poolListCurrent->Element;
    }
    return PushNewPool();
}

DescriptorPoolVulkan* TypedDescriptorPoolSetVulkan::PushNewPool()
{
    auto* newPool = New<DescriptorPoolVulkan>(_device, _layout);
    if (_poolListCurrent)
    {
        _poolListCurrent->Next = New<PoolList>(newPool);
        _poolListCurrent = _poolListCurrent->Next;
    }
    else
    {
        _poolListCurrent = _poolListHead = New<PoolList>(newPool);
    }
    return newPool;
}

void TypedDescriptorPoolSetVulkan::Reset()
{
    for (PoolList* pool = _poolListHead; pool; pool = pool->Next)
    {
        pool->Element->Reset();
    }
    _poolListCurrent = _poolListHead;
}

#endif

#if VULKAN_USE_DESCRIPTOR_POOL_MANAGER

DescriptorPoolSetContainerVulkan::DescriptorPoolSetContainerVulkan(GPUDeviceVulkan* device)
    : _device(device)
    , _lastFrameUsed(Engine::FrameCount)
    , _used(true)
{
}

DescriptorPoolSetContainerVulkan::~DescriptorPoolSetContainerVulkan()
{
    _typedDescriptorPools.ClearDelete();
}

TypedDescriptorPoolSetVulkan* DescriptorPoolSetContainerVulkan::AcquireTypedPoolSet(const DescriptorSetLayoutVulkan& layout)
{
    const uint32 hash = VULKAN_HASH_POOLS_WITH_TYPES_USAGE_ID ? layout.GetTypesUsageID() : GetHash(layout);
    TypedDescriptorPoolSetVulkan* typedPool;
    if (!_typedDescriptorPools.TryGet(hash, typedPool))
    {
        typedPool = New<TypedDescriptorPoolSetVulkan>(_device, this, layout);
        _typedDescriptorPools.Add(hash, typedPool);
    }
    return typedPool;
}

void DescriptorPoolSetContainerVulkan::Reset()
{
    for (auto i = _typedDescriptorPools.Begin(); i.IsNotEnd(); ++i)
    {
        TypedDescriptorPoolSetVulkan* typedPool = i->Value;
        typedPool->Reset();
    }
}

void DescriptorPoolSetContainerVulkan::SetUsed(bool used)
{
    _used = used;
    _lastFrameUsed = used ? Engine::FrameCount : _lastFrameUsed;
}

DescriptorPoolsManagerVulkan::DescriptorPoolsManagerVulkan(GPUDeviceVulkan* device)
    : _device(device)
{
}

DescriptorPoolsManagerVulkan::~DescriptorPoolsManagerVulkan()
{
    _poolSets.ClearDelete();
}

DescriptorPoolSetContainerVulkan& DescriptorPoolsManagerVulkan::AcquirePoolSetContainer()
{
    ScopeLock lock(_locker);

    for (auto* poolSet : _poolSets)
    {
        if (poolSet->IsUnused())
        {
            poolSet->SetUsed(true);
            poolSet->Reset();
            return *poolSet;
        }
    }

    const auto poolSet = New<DescriptorPoolSetContainerVulkan>(_device);
    _poolSets.Add(poolSet);
    return *poolSet;
}

void DescriptorPoolsManagerVulkan::ReleasePoolSet(DescriptorPoolSetContainerVulkan& poolSet)
{
    poolSet.SetUsed(false);
}

void DescriptorPoolsManagerVulkan::GC()
{
    ScopeLock lock(_locker);

    // Pool sets are forward allocated - iterate from the back to increase the chance of finding an unused one
    for (int32 i = _poolSets.Count() - 1; i >= 0; i--)
    {
        const auto poolSet = _poolSets[i];
        if (poolSet->IsUnused() && Engine::FrameCount - poolSet->GetLastFrameUsed() > VULKAN_RESOURCE_DELETE_SAFE_FRAMES_COUNT)
        {
            _poolSets.RemoveAt(i);
            Delete(poolSet);
            break;
        }
    }
}

#endif

PipelineLayoutVulkan::PipelineLayoutVulkan(GPUDeviceVulkan* device, const DescriptorSetLayoutInfoVulkan& layout)
    : _device(device)
    , _handle(VK_NULL_HANDLE)
    , _descriptorSetLayout(device)
{
    _descriptorSetLayout.CopyFrom(layout);
    _descriptorSetLayout.Compile();

    const auto& layoutHandles = _descriptorSetLayout.GetHandles();

    VkPipelineLayoutCreateInfo createInfo;
    RenderToolsVulkan::ZeroStruct(createInfo, VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
    createInfo.setLayoutCount = layoutHandles.Count();
    createInfo.pSetLayouts = layoutHandles.Get();
    VALIDATE_VULKAN_RESULT(vkCreatePipelineLayout(_device->Device, &createInfo, nullptr, &_handle));
}

PipelineLayoutVulkan::~PipelineLayoutVulkan()
{
    if (_handle != VK_NULL_HANDLE)
    {
        _device->DeferredDeletionQueue.EnqueueResource(DeferredDeletionQueueVulkan::Type::PipelineLayout, _handle);
    }
}

#if !VULKAN_USE_DESCRIPTOR_POOL_MANAGER

DescriptorSetsVulkan::DescriptorSetsVulkan(GPUDeviceVulkan* device, const DescriptorSetLayoutVulkan& layout, GPUContextVulkan* context)
	: _device(device)
	, _pool(nullptr)
	, _layout(&layout)
{
	const auto& layoutHandles = layout.GetHandles();
	if (layoutHandles.HasItems())
	{
		VkDescriptorSetAllocateInfo allocateInfo;
		RenderToolsVulkan::ZeroStruct(allocateInfo, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
		allocateInfo.descriptorSetCount = layoutHandles.Count();
		allocateInfo.pSetLayouts = layoutHandles.Get();

		_sets.AddZeroed(layoutHandles.Count());

		_pool = context->AllocateDescriptorSets(allocateInfo, layout, _sets.Get());
		_pool->TrackAddUsage(layout);
	}
}

DescriptorSetsVulkan::~DescriptorSetsVulkan()
{
}

DescriptorSetRingBufferVulkan::DescriptorSetRingBufferVulkan(GPUDeviceVulkan* device)
	: _device(device)
	, _currDescriptorSets(nullptr)
{
}

DescriptorSetsVulkan* DescriptorSetRingBufferVulkan::RequestDescriptorSets(GPUContextVulkan* context, CmdBufferVulkan* cmdBuffer, const PipelineLayoutVulkan* layout)
{
	DescriptorSetsEntry* foundEntry = nullptr;
	for (DescriptorSetsEntry* descriptorSetsEntry : DescriptorSetsEntries)
	{
		if (descriptorSetsEntry->CmdBuffer == cmdBuffer)
		{
			foundEntry = descriptorSetsEntry;
		}
	}

	if (!foundEntry)
	{
		if (!layout->HasDescriptors())
		{
			return nullptr;
		}

		foundEntry = New<DescriptorSetsEntry>(cmdBuffer);
		DescriptorSetsEntries.Add(foundEntry);
	}

	const uint64 cmdBufferFenceSignaledCounter = cmdBuffer->GetFenceSignaledCounter();
	for (int32 i = 0; i < foundEntry->Pairs.Count(); i++)
	{
		DescriptorSetsPair& entry = foundEntry->Pairs[i];
		if (entry.FenceCounter < cmdBufferFenceSignaledCounter)
		{
			entry.FenceCounter = cmdBufferFenceSignaledCounter;
			return entry.DescriptorSets;
		}
	}

	DescriptorSetsPair newEntry;
	newEntry.DescriptorSets = New<DescriptorSetsVulkan>(_device, layout->GetDescriptorSetLayout(), context);
	newEntry.FenceCounter = cmdBufferFenceSignaledCounter;
	foundEntry->Pairs.Add(newEntry);
	return newEntry.DescriptorSets;
}

#endif

uint32 DescriptorSetWriterVulkan::SetupDescriptorWrites(const SpirvShaderDescriptorInfo& info, VkWriteDescriptorSet* writeDescriptors, VkDescriptorImageInfo* imageInfo, VkDescriptorBufferInfo* bufferInfo, uint8* bindingToDynamicOffsetMap)
{
    WriteDescriptors = writeDescriptors;
    NumWrites = info.DescriptorTypesCount;
    ASSERT(info.DescriptorTypesCount <= 64 && TEXT("Out of bits for Dirty Mask! More than 64 resources in one descriptor set!"));
    BindingToDynamicOffsetMap = bindingToDynamicOffsetMap;

    uint32 dynamicOffsetIndex = 0;
    for (uint32 i = 0; i < info.DescriptorTypesCount; i++)
    {
        writeDescriptors->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writeDescriptors->dstBinding = i;
        writeDescriptors->descriptorCount = 1;
        writeDescriptors->descriptorType = info.DescriptorTypes[i].DescriptorType;

        switch (writeDescriptors->descriptorType)
        {
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
            BindingToDynamicOffsetMap[i] = dynamicOffsetIndex;
            ++dynamicOffsetIndex;
            writeDescriptors->pBufferInfo = bufferInfo++;
            break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            writeDescriptors->pBufferInfo = bufferInfo++;
            break;
        case VK_DESCRIPTOR_TYPE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            writeDescriptors->pImageInfo = imageInfo++;
            break;
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            break;
        default:
        CRASH;
            break;
        }

        writeDescriptors++;
    }

    return dynamicOffsetIndex;
}

#endif
