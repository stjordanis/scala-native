#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "LargeAllocator.h"
#include "utils/MathUtils.h"
#include "Object.h"
#include "Log.h"
#include "headers/ObjectHeader.h"

inline static int LargeAllocator_sizeToLinkedListIndex(size_t size) {
    assert(size >= MIN_BLOCK_SIZE);
    return log2_floor(size) - LARGE_OBJECT_MIN_SIZE_BITS;
}

Chunk *LargeAllocator_chunkAddOffset(Chunk *chunk, size_t words) {
    return (Chunk *)((ubyte_t *)chunk + words);
}

void LargeAllocator_freeListAddBlockLast(FreeList *freeList, Chunk *chunk) {
    if (freeList->first == NULL) {
        freeList->first = chunk;
    }
    freeList->last = chunk;
    chunk->next = NULL;
}

Chunk *LargeAllocator_freeListRemoveFirstBlock(FreeList *freeList) {
    if (freeList->first == NULL) {
        return NULL;
    }
    Chunk *chunk = freeList->first;
    if (freeList->first == freeList->last) {
        freeList->last = NULL;
    }

    freeList->first = chunk->next;
    return chunk;
}

void LargeAllocator_freeListInit(FreeList *freeList) {
    freeList->first = NULL;
    freeList->last = NULL;
}

void LargeAllocator_Init(LargeAllocator *allocator, word_t *offset, size_t size,
                         Bytemap *bytemap) {
    allocator->offset = offset;
    allocator->size = size;
    allocator->bytemap = bytemap;

    for (int i = 0; i < FREE_LIST_COUNT; i++) {
        LargeAllocator_freeListInit(&allocator->freeLists[i]);
    }

    LargeAllocator_AddChunk(allocator, (Chunk *)offset, size);
}

void LargeAllocator_AddChunk(LargeAllocator *allocator, Chunk *chunk,
                             size_t total_block_size) {
    assert(total_block_size >= MIN_BLOCK_SIZE);
    assert(total_block_size % MIN_BLOCK_SIZE == 0);

    size_t remaining_size = total_block_size;
    ubyte_t *current = (ubyte_t *)chunk;
    while (remaining_size > 0) {
        int log2_f = log2_floor(remaining_size);
        size_t chunkSize = 1UL << log2_f;
        chunkSize = chunkSize > MAX_BLOCK_SIZE ? MAX_BLOCK_SIZE : chunkSize;
        assert(chunkSize >= MIN_BLOCK_SIZE && chunkSize <= MAX_BLOCK_SIZE);
        int listIndex = LargeAllocator_sizeToLinkedListIndex(chunkSize);

        Chunk *currentChunk = (Chunk *)current;
        LargeAllocator_freeListAddBlockLast(&allocator->freeLists[listIndex],
                                            (Chunk *)current);

        currentChunk->nothing = NULL;
        currentChunk->size = chunkSize;
        ObjectMeta *currentMeta =
            Bytemap_Get(allocator->bytemap, (word_t *)current);
        ObjectMeta_SetPlaceholder(currentMeta);

        current += chunkSize;
        remaining_size -= chunkSize;
    }
}

Object *LargeAllocator_GetBlock(LargeAllocator *allocator,
                                size_t requestedBlockSize) {
    size_t actualBlockSize =
        MathUtils_RoundToNextMultiple(requestedBlockSize, MIN_BLOCK_SIZE);
    size_t requiredChunkSize = 1UL << MathUtils_Log2Ceil(actualBlockSize);

    int listIndex = LargeAllocator_sizeToLinkedListIndex(requiredChunkSize);
    Chunk *chunk = NULL;
    while (listIndex <= FREE_LIST_COUNT - 1 &&
           (chunk = allocator->freeLists[listIndex].first) == NULL) {
        ++listIndex;
    }

    if (chunk == NULL) {
        return NULL;
    }

    size_t chunkSize = chunk->size;
    assert(chunkSize >= MIN_BLOCK_SIZE);

    if (chunkSize - MIN_BLOCK_SIZE >= actualBlockSize) {
        Chunk *remainingChunk =
            LargeAllocator_chunkAddOffset(chunk, actualBlockSize);
        LargeAllocator_freeListRemoveFirstBlock(
            &allocator->freeLists[listIndex]);
        size_t remainingChunkSize = chunkSize - actualBlockSize;
        LargeAllocator_AddChunk(allocator, remainingChunk, remainingChunkSize);
    } else {
        LargeAllocator_freeListRemoveFirstBlock(
            &allocator->freeLists[listIndex]);
    }

    ObjectMeta *objectMeta = Bytemap_Get(allocator->bytemap, (word_t *)chunk);
    ObjectMeta_SetAllocated(objectMeta);
    Object *object = (Object *)chunk;
    memset(object, 0, actualBlockSize);
    return object;
}

void LargeAllocator_clearFreeLists(LargeAllocator *allocator) {
    for (int i = 0; i < FREE_LIST_COUNT; i++) {
        allocator->freeLists[i].first = NULL;
        allocator->freeLists[i].last = NULL;
    }
}

void LargeAllocator_Sweep(LargeAllocator *allocator) {
    LargeAllocator_clearFreeLists(allocator);

    Object *current = (Object *)allocator->offset;
    void *heapEnd = (ubyte_t *)allocator->offset + allocator->size;

    while (current != heapEnd) {
        ObjectMeta *currentMeta =
            Bytemap_Get(allocator->bytemap, (word_t *)current);
        assert(!ObjectMeta_IsFree(currentMeta));
        if (ObjectMeta_IsMarked(currentMeta)) {
            ObjectMeta_SetAllocated(currentMeta);

            current = Object_NextLargeObject(current);
        } else {
            size_t currentSize = Object_ChunkSize(current);
            Object *next = Object_NextLargeObject(current);
            ObjectMeta *nextMeta =
                Bytemap_Get(allocator->bytemap, (word_t *)next);
            while (next != heapEnd && !ObjectMeta_IsMarked(nextMeta)) {
                currentSize += Object_ChunkSize(next);
                ObjectMeta_SetFree(nextMeta);
                next = Object_NextLargeObject(next);
                nextMeta = Bytemap_Get(allocator->bytemap, (word_t *)next);
            }
            LargeAllocator_AddChunk(allocator, (Chunk *)current, currentSize);
            current = next;
        }
    }
}
