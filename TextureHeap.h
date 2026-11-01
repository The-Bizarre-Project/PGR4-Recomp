#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

struct TextureHeapAllocation
{
    Microsoft::WRL::ComPtr<ID3D12Heap> heap;
    uint64_t offset = 0;
    uint64_t size = 0;
    // Index of the heap this allocation came from, so Free()/deferred-free
    // don't need to linear-search heaps_ by ComPtr comparison.
    size_t heap_index = 0;
    // True if this allocation was actually placed in a shared heap. False
    // means the caller fell back to a standalone committed resource (e.g.
    // because the request exceeded kHeapSize), and Free()/deferred-free
    // should be a no-op for it - the ComPtr's own refcounting handles that
    // case exactly like the original unmodified path did.
    bool from_heap = false;
};

class TextureHeapAllocator
{
public:
    static constexpr uint64_t kHeapSize = 64ull * 1024ull * 1024ull;

    explicit TextureHeapAllocator(ID3D12Device* device);

    // Returns false (leaving allocation untouched) if size exceeds kHeapSize
    // - the caller must fall back to CreateCommittedResource in that case,
    // exactly as TextureCreationExample.cpp already does. A single resource
    // larger than the heap size can never be satisfied by a freshly created
    // heap either, so this must be checked BEFORE calling CreateHeap(),
    // or CreateHeap() will be called repeatedly until it fails from OOM.
    bool Allocate(
        uint64_t size,
        uint64_t alignment,
        TextureHeapAllocation& allocation);

    // Does NOT return the block to the free list immediately. The GPU may
    // still have queued work referencing this resource's memory. Call this
    // when the resource is destroyed; it records the current submission
    // number, and the block is only returned to the free list once
    // ProcessPendingFrees() is called with a submission_completed value
    // that has passed it. This mirrors the resources_for_deletion_ pattern
    // already used elsewhere in the D3D12 command processor.
    //
    // Also requires an aliasing barrier: once a pending-free block is
    // reused by a later Allocate() for a NEW resource, the caller must
    // issue a D3D12_RESOURCE_BARRIER_TYPE_ALIASING barrier (old resource
    // pointer may be null if unknown/already destroyed; new resource
    // pointer is the freshly created placed resource) before the new
    // resource is used on the GPU. This is a D3D12 requirement for placed
    // resources sharing heap memory, not optional.
    void Free(
        const TextureHeapAllocation& allocation,
        uint64_t submission_current);

    // Call once per frame/submission boundary (e.g. alongside wherever
    // resources_for_deletion_ is processed in the command processor).
    // Moves any pending frees whose tagged submission has completed back
    // into the real free list, where a future Allocate() can reuse them.
    void ProcessPendingFrees(uint64_t submission_completed);

private:
    struct FreeBlock
    {
        uint64_t offset;
        uint64_t size;
    };

    struct PendingFree
    {
        uint64_t offset;
        uint64_t size;
        uint64_t submission;
    };

    struct Heap
    {
        Microsoft::WRL::ComPtr<ID3D12Heap> resource;
        std::vector<FreeBlock> free_blocks;
        std::vector<PendingFree> pending_frees;
    };

    ID3D12Device* device_;
    std::vector<Heap> heaps_;

    bool CreateHeap();
};
