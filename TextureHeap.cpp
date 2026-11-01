#include "TextureHeap.h"

#include <algorithm>

TextureHeapAllocator::TextureHeapAllocator(ID3D12Device* device)
    : device_(device)
{
}

bool TextureHeapAllocator::CreateHeap()
{
    D3D12_HEAP_DESC desc{};
    desc.SizeInBytes = kHeapSize;
    desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    desc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;

    Heap heap;

    if (FAILED(device_->CreateHeap(
            &desc,
            IID_PPV_ARGS(&heap.resource))))
    {
        return false;
    }

    heap.free_blocks.push_back({
        0,
        kHeapSize
    });

    heaps_.push_back(std::move(heap));
    return true;
}

bool TextureHeapAllocator::Allocate(
    uint64_t size,
    uint64_t alignment,
    TextureHeapAllocation& allocation)
{
    // A single resource larger than one heap can NEVER be satisfied by a
    // freshly created heap of the same fixed size - without this guard,
    // the recursive CreateHeap()-then-retry below would loop, allocating
    // heap after heap, until CreateHeap() itself fails from exhausting
    // GPU memory. Bail out immediately so the caller falls back to its
    // own CreateCommittedResource path instead.
    if (size > kHeapSize)
    {
        return false;
    }

    for (size_t heap_index = 0; heap_index < heaps_.size(); ++heap_index)
    {
        auto& heap = heaps_[heap_index];

        for (size_t i = 0; i < heap.free_blocks.size(); ++i)
        {
            auto block = heap.free_blocks[i];

            const uint64_t aligned =
                (block.offset + alignment - 1) &
                ~(alignment - 1);

            const uint64_t padding =
                aligned - block.offset;

            if (padding + size > block.size)
                continue;

            const uint64_t consumed = padding + size;

            heap.free_blocks.erase(
                heap.free_blocks.begin() + i);

            if (padding != 0)
            {
                heap.free_blocks.insert(
                    heap.free_blocks.begin() + i,
                    { block.offset, padding });
                ++i;
            }

            const uint64_t tail =
                block.size - consumed;

            if (tail != 0)
            {
                heap.free_blocks.insert(
                    heap.free_blocks.begin() + i,
                    { aligned + size, tail });
            }

            allocation.heap = heap.resource;
            allocation.offset = aligned;
            allocation.size = size;
            allocation.heap_index = heap_index;
            allocation.from_heap = true;
            return true;
        }
    }

    if (!CreateHeap())
        return false;

    return Allocate(size, alignment, allocation);
}

void TextureHeapAllocator::Free(
    const TextureHeapAllocation& allocation,
    uint64_t submission_current)
{
    if (!allocation.from_heap)
    {
        // This allocation was a committed-resource fallback, never placed
        // in a shared heap - nothing for the allocator to track.
        return;
    }

    if (allocation.heap_index >= heaps_.size())
    {
        return;
    }

    // Do NOT touch free_blocks yet. The GPU may still be reading/writing
    // this memory via queued command lists that haven't executed. Record
    // it as pending, tagged with the current submission, so a later
    // ProcessPendingFrees() call can safely return it once the GPU has
    // actually finished with it.
    heaps_[allocation.heap_index].pending_frees.push_back({
        allocation.offset,
        allocation.size,
        submission_current
    });
}

void TextureHeapAllocator::ProcessPendingFrees(uint64_t submission_completed)
{
    for (auto& heap : heaps_)
    {
        if (heap.pending_frees.empty())
        {
            continue;
        }

        std::vector<PendingFree> still_pending;
        still_pending.reserve(heap.pending_frees.size());

        for (const auto& pending : heap.pending_frees)
        {
            if (pending.submission > submission_completed)
            {
                // GPU hasn't caught up to this resource's last use yet -
                // keep waiting, don't return it to the free list.
                still_pending.push_back(pending);
                continue;
            }

            heap.free_blocks.push_back({
                pending.offset,
                pending.size
            });
        }

        heap.pending_frees = std::move(still_pending);

        if (heap.free_blocks.size() <= 1)
        {
            continue;
        }

        std::sort(
            heap.free_blocks.begin(),
            heap.free_blocks.end(),
            [](const FreeBlock& a, const FreeBlock& b)
            {
                return a.offset < b.offset;
            });

        std::vector<FreeBlock> merged;
        merged.reserve(heap.free_blocks.size());

        for (const auto& block : heap.free_blocks)
        {
            if (!merged.empty())
            {
                auto& previous = merged.back();

                if (previous.offset + previous.size == block.offset)
                {
                    previous.size += block.size;
                    continue;
                }
            }

            merged.push_back(block);
        }

        heap.free_blocks = std::move(merged);
    }
}
