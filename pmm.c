/* 
    BitMap based Physcial Memoru Manager. Uses a first fit allocator for 
    Normal zone memory and a buddy allocator for DMA zone allocation
*/

#include <lumos/pmm.h>
#include <lumos/multiboot.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdbool.h>
#include <utils.h>

#define getBitOffset(start, target, blockSize) ((uint32_t)target - (uint32_t)start) / blockSize
#define CEIL(x, y) ((x / y) + ((x % y) != 0))

uintptr_t kernel_end = (uintptr_t)&_kernel_end;
uintptr_t kernel_start = (uintptr_t)&_kernel_start;
uintptr_t VIRTUAL_KERNEL_OFFSET = (uintptr_t)&VIRTUAL_KERNEL_OFFSET_LD;

/*
    The available physical memory is divided into two
    types of zones. DMA zone handles a maximum of 256K 
    of the available physical memory space, upto a max address
    of 16M. Normal zone contains the rest 
*/
struct zone *zone_DMA = (struct zone *)&_kernel_end;
struct zone *zone_normal = NULL;

// debugging
void printZoneInfo(struct zone *zone);
void printBuddyBitMap(uint32_t *map, uint32_t mapWordCount);

// utils
void makeBuddies(struct pool *pool);
void set_bit(uint32_t *mapStart, uint32_t offset);                             // set a single bit
void set_bits(uint32_t *mapStart, uint32_t offsetStart, uint32_t offsetEnd);   // set a section of bits
void unset_bit(uint32_t *mapStart, uint32_t offset);                           // unset a single bit
void unset_bits(uint32_t *mapStart, uint32_t offsetStart, uint32_t offsetEnd); // unset a section of bits
bool test_bit(uint32_t *mapStart, uint32_t offset);                            // return the value of a bit
void reserve_kernel();                                                         // Mark the space used by the kernel and the pmm structures as reserved
intmax_t findFirstFreeBit(uint32_t *map, uint32_t maxWords);                   // return the offset of the first unset bit. -1 if no such bit

/* -------------------- API FUNCTION DEFINITIONS ----------------------- */

/* 
    This is the allocator for the NORMAL zone. This uses a basic first fit 
    allocator. Returns a pointer to the 
*/
void *pmm_alloc(uint32_t request)
{
    logf("\n[pmm_alloc] : Received request for %d bytes\n", request);

    if (request == 0)
    {
        logf("Returning NULL because NULL request\n");
        return NULL;
    }

    request = CEIL(request, BLOCK_SIZE);
    logf("[pmm_alloc] : Rounded request upto %d blocks\n", request);

    if (zone_normal->freeBlocks < request)
    {
        // TODO : Try and allocate from the DMA zone instead
        logf("Returning NULL because insufficient blocks.\n");
        return NULL;
    }

    struct pool *currentPool;
    currentPool = zone_normal->poolStart;
    uint32_t *bitMap = NULL;
    uint32_t offset = 0;
    uint32_t maxBlocks = 0;
    uint32_t startIndex = 0;
    bool isSet = 0;
    while (currentPool != NULL)
    {
        if (currentPool->freeBlocks >= request)
        {
            logf("[pmm_alloc] | Chose pool : Start: %x\tFree Blocks: %d\tMax Free Blocks: %d\n", currentPool->start, currentPool->freeBlocks, currentPool->poolBuddiesTop->maxFreeBlocks);
            bitMap = currentPool->poolBuddiesBottom->bitMap;
            maxBlocks = currentPool->poolBuddiesBottom->maxFreeBlocks;

            while (offset < maxBlocks)
            {
                // Start checking where the entire word isn't reserved
                if (bitMap[offset / 32] != 0xFFFFFFFF)
                {
                    // find the first available bit
                    startIndex = offset;
                    while (test_bit(bitMap, startIndex) == 1)
                        startIndex++;

                    // loop until we get through n bits (available ones)
                    while ((offset < startIndex + request) && (offset < maxBlocks))
                    {
                        // if any of these bits is set, we break
                        if (test_bit(bitMap, offset) == 1)
                        {
                            isSet = 1;
                            break;
                        }
                        offset++;
                    }

                    // if this is not set, we found n available bits
                    if (!isSet)
                    {
                        // todo: gotta return address starting at startIndex
                    }
                    else
                    {
                        // if continuous n bits weren't found, move to the next bit and start checking again
                        offset++;
                        continue;
                    }

                    if (offset >= maxBlocks)
                    {
                        logf("Returning null because internal loop ran too long");
                        return NULL;
                    }

                    // find the first index here that is available
                    // keep looping until n available bits are found or a reserved bit is encountered
                    // if there were n available bits, return starting position of bits
                    // else set offset to the next bit and continue;

                    // temporary return
                }
                offset += 32;
            }

            logf("[pmm_alloc] : Couldn't find an unset bit\n");
            return NULL;
        }
        currentPool = currentPool->nextPool;
    }

    logf("[pmm_alloc] : Returning null because no NORMAL free pools\n");
    return NULL;
}

void init_pmm(multiboot_info_t *mbtStructure)
{
    // temp pointers to work with pools inside loops
    struct pool *currentPool;
    struct pool *previousPool = NULL;
    struct buddy *currentBuddy;

    // make sure we have a valid memory map - 6th bit of flags indicates whether the mmap_addr & mmp_length fields are valid
    if (!(mbtStructure->flags & MBT_FLAG_IS_MMAP))
    {
        logf("[PMM] : No memory map available\n");
        abort();
    }

    // Initialize the DMA zone descriptor
    zone_DMA->zoneType = 0;
    zone_DMA->freeBlocks = 0;
    zone_DMA->poolStart = NULL;
    zone_DMA->zonePhysicalSize = sizeof(struct zone);

    // go through the memory map from the multiboot info structure
    struct mmap_entry_t *section = (struct mmap_entry_t *)(mbtStructure->mmap_addr + VIRTUAL_KERNEL_OFFSET);
    while (section < (struct mmap_entry_t *)(mbtStructure->mmap_addr + mbtStructure->mmap_length + VIRTUAL_KERNEL_OFFSET))
    {
        // Skip reserved sections and sections >4G
        if (section->base_high || section->type != 1)
        {
            section = (struct mmap_entry_t *)((uint32_t)section + (uint32_t)section->size + sizeof(section->size));
            continue;
        }

        // If we're done with the DMA, add the entire section as a NORMAL zone pool
        if (section->base_low > DMA_MAX_ADDRESS || zone_DMA->freeBlocks >= DMA_TOTAL_BLOCKS)
        {
            // Initialize the Zone descriptor if this is the first addition to the normal zone
            if (zone_normal == NULL)
            {
                zone_normal = (struct zone *)((uint32_t)zone_DMA + zone_DMA->zonePhysicalSize); // put the normal zone structure right after all the DMA zone - related data
                zone_normal->zoneType = 1;
                zone_normal->freeBlocks = 0;
                zone_normal->poolStart = NULL;
                zone_normal->zonePhysicalSize = sizeof(struct zone);
            }

            // Create a new pool and add it to the existing list of NORMAL pools
            currentPool = (struct pool *)((uint32_t)zone_normal + zone_normal->zonePhysicalSize);
            currentPool->start = section->base_low;
            currentPool->freeBlocks = (section->length_low / BLOCK_SIZE);
            currentPool->nextPool = NULL;
            currentPool->poolPhysicalSize = sizeof(struct pool);

            zone_normal->freeBlocks += (section->length_low / BLOCK_SIZE);

            // create a new bitmap descriptor and bitmap for the pool
            currentBuddy = (struct buddy *)((uint32_t)currentPool + currentPool->poolPhysicalSize);
            currentBuddy->buddyOrder = 1;
            currentBuddy->freeBlocks = currentPool->freeBlocks;
            currentBuddy->maxFreeBlocks = currentPool->freeBlocks;
            currentBuddy->mapWordCount = (currentBuddy->freeBlocks / 32) + (currentBuddy->freeBlocks % 32 != 0);
            currentBuddy->bitMap = (uint32_t *)((uint32_t)currentPool + currentPool->poolPhysicalSize + sizeof(struct buddy));
            currentBuddy->nextBuddy = NULL;
            currentBuddy->prevBuddy = NULL;

            currentPool->poolBuddiesTop = currentPool->poolBuddiesBottom = currentBuddy;
            currentPool->poolPhysicalSize += sizeof(struct buddy) + (currentBuddy->mapWordCount * 4);

            zone_normal->zonePhysicalSize += currentPool->poolPhysicalSize;

            // set the bits for available and reserved
            unset_bits(currentBuddy->bitMap, 0, currentBuddy->freeBlocks - 1);
            set_bits(currentBuddy->bitMap, currentBuddy->freeBlocks, (currentBuddy->mapWordCount * 32) - 1);

            if (zone_normal->poolStart == NULL)
                zone_normal->poolStart = currentPool; // this is the first NORMAL pool
            else
                previousPool->nextPool = currentPool; // update link from previous pool

            previousPool = currentPool;

            section = (struct mmap_entry_t *)((uint32_t)section + (uint32_t)section->size + sizeof(section->size));
        }
        // else, add the current section as a DMA pool either entirely or partially
        else
        {
            // create and init a new DMA pool
            currentPool = (struct pool *)((uint32_t)zone_DMA + zone_DMA->zonePhysicalSize);
            currentPool->start = section->base_low;
            currentPool->nextPool = NULL;
            currentPool->poolBuddiesTop = NULL;
            currentPool->poolPhysicalSize = sizeof(struct pool);

            // add to the existing linked list
            if (zone_DMA->poolStart == NULL)
                zone_DMA->poolStart = currentPool; // this is the first DMA pool
            else
                previousPool->nextPool = currentPool; // Add pool to the list

            // Make a decision on how much of the current section is to be added as DMA pool
            if (section->length_low < (DMA_TOTAL_BLOCKS - zone_DMA->freeBlocks) && (currentPool->start + section->length_low - 1) <= DMA_MAX_ADDRESS)
            {
                // Adding the entire section as a pool
                currentPool->freeBlocks = (section->length_low / BLOCK_SIZE);
                zone_DMA->freeBlocks += (section->length_low / BLOCK_SIZE);

                makeBuddies(currentPool);
                zone_DMA->zonePhysicalSize += currentPool->poolPhysicalSize;

                previousPool = currentPool;

                section = (struct mmap_entry_t *)((uint32_t)section + section->size + sizeof(section->size));
                continue;
            }
            else if ((DMA_MAX_ADDRESS - currentPool->start + 1) < (DMA_TOTAL_BLOCKS - zone_DMA->freeBlocks))
            {
                // Adding a partial section because of the 16MB condition
                currentPool->freeBlocks = (DMA_MAX_ADDRESS - currentPool->start + 1) / BLOCK_SIZE;
                section->base_low += (DMA_MAX_ADDRESS - currentPool->start + 1);   // Advance start of current section
                section->length_low -= (DMA_MAX_ADDRESS - currentPool->start + 1); // Reduce size of current section
            }
            else
            {
                // Adding a partial section because the 256KB condition
                currentPool->freeBlocks = DMA_TOTAL_BLOCKS - zone_DMA->freeBlocks;
                section->base_low += DMA_TOTAL_BYTES - (zone_DMA->freeBlocks * BLOCK_SIZE);   // Advance start of current section
                section->length_low -= DMA_TOTAL_BYTES - (zone_DMA->freeBlocks * BLOCK_SIZE); // Reduce size of current section
            }

            zone_DMA->freeBlocks += currentPool->freeBlocks;

            makeBuddies(currentPool);

            zone_DMA->zonePhysicalSize += currentPool->poolPhysicalSize;

            previousPool = currentPool;
        }
    }

    // Mark kernel and pmm spaces as reserved
    reserve_kernel();

    // log pmm structures
    logf("DMA ");
    printZoneInfo(zone_DMA);
    logf("Normal");
    printZoneInfo(zone_normal);
}

/* -------------------- UTIL FUNCTION DEFINITIONS ----------------------- */

/* 
    Mark the space used by the kernel and the pmm structures as reserved.
    This creates some free blocks. Mark them and add the count of free 
    blocks to their respective buddies.
*/
void reserve_kernel()
{
    logf("Reserving kernel\n-----------------\n");

    uint32_t resStart = (uint32_t)kernel_start - VIRTUAL_KERNEL_OFFSET;
    uint32_t resEnd = (uint32_t)kernel_end + zone_DMA->zonePhysicalSize + zone_normal->zonePhysicalSize - 1 - VIRTUAL_KERNEL_OFFSET;
    uint32_t startOffset = 0, endOffSet = 0;

    struct buddy *currentBuddy;
    struct pool *currentPool;
    currentPool = zone_normal->poolStart;

    logf("Kernel start: %x\tKernel End: %x\n", resStart, resEnd);
    while (currentPool != NULL)
    {
        // identify the pool that contains the kernel and pmm structures
        if (currentPool->start <= resStart && resStart < (uint32_t)currentPool->start + (currentPool->freeBlocks * BLOCK_SIZE))
        {
            logf("Pool @ %x\tStart : %x\tSize:%x\n", currentPool, currentPool->start, (currentPool->freeBlocks * BLOCK_SIZE));

            // Reserve the coresponding blocks in the highest order buddy
            currentBuddy = currentPool->poolBuddiesTop;
            startOffset = getBitOffset(currentPool->start, resStart, (currentBuddy->buddyOrder * BLOCK_SIZE));
            endOffSet = getBitOffset(currentPool->start, resEnd, (currentBuddy->buddyOrder * BLOCK_SIZE));
            logf("Order: %d\tFreeBlocks: %x\tMaxFree: %x\n", currentBuddy->buddyOrder, currentBuddy->freeBlocks, currentBuddy->maxFreeBlocks);
            logf("\tStart Block: %d\tEnd Block: %d\n", startOffset, endOffSet);
            set_bits(currentBuddy->bitMap, startOffset, endOffSet);
            currentBuddy->freeBlocks -= (endOffSet - startOffset + 1); // reduce the number of free blocks
            logf("\n------------------------------------------------------\n\n");
            return;
        }
        currentPool = currentPool->nextPool;
    }

    logf("\n------------------------------------------------------\n\n");
}

void set_bit(uint32_t *mapStart, uint32_t offset)
{
    mapStart[offset / 32] |= 1 << (offset % 32);
}

void set_bits(uint32_t *mapStart, uint32_t offsetStart, uint32_t offsetEnd)
{
    uint32_t i;
    // set bit by bit until we reach the start of a word
    for (i = offsetStart; i <= offsetEnd && i % 32 != 0; i++)
        set_bit(mapStart, i);

    // set word by word, until a word would be too big for offsetEnd
    for (; i + 32 <= offsetEnd; i = i + 32)
        mapStart[i / 32] = 0xFFFFFFFF;

    // set bit by bit until the end
    for (; i <= offsetEnd; i++)
        set_bit(mapStart, i);
}

void unset_bit(uint32_t *mapStart, uint32_t offset)
{
    mapStart[offset / 32] &= ~(1 << (offset % 32));
}

void unset_bits(uint32_t *mapStart, uint32_t offsetStart, uint32_t offsetEnd)
{
    uint32_t i;
    // unset bit by bit until we reach the start of a word
    for (i = offsetStart; i <= offsetEnd && i % 32 != 0; i++)
        unset_bit(mapStart, i);

    // set word by word, until a word would be too big for offsetEnd
    for (; i + 32 <= offsetEnd; i = i + 32)
        mapStart[i / 32] = 0;

    // set bit by bit until the end
    for (; i <= offsetEnd; i++)
        unset_bit(mapStart, i);
}

bool test_bit(uint32_t *mapStart, uint32_t offset)
{
    return (mapStart[offset / 32] & (1 << (offset % 32)));
}

/*
    Creates structures and initializes bitmaps for the buddies of a given pool.  
    The blocks of the highest order are all set to available, while the lower 
    order ones have all their blocks set to reserved (but the ones that do not 
    belong to a larger parent block are set to available).
*/
void makeBuddies(struct pool *pool)
{
    struct buddy *currentBuddy;
    struct buddy *previousBuddy = NULL;
    for (uint8_t i = MAX_BLOCK_ORDER; i > 0; i = i >> 1)
    {
        currentBuddy = (struct buddy *)((uint32_t)pool + pool->poolPhysicalSize); // put the current buddy right after the previous structures
        currentBuddy->buddyOrder = i;                                             // buddy order in terms of powers of 2
        currentBuddy->maxFreeBlocks = pool->freeBlocks / i;                       // max possible allocations for this order
        currentBuddy->freeBlocks = (previousBuddy == NULL) ? currentBuddy->maxFreeBlocks : currentBuddy->maxFreeBlocks - (previousBuddy->maxFreeBlocks * 2);
        currentBuddy->mapWordCount = (currentBuddy->maxFreeBlocks / 32) + (currentBuddy->maxFreeBlocks % 32 != 0);
        currentBuddy->bitMap = (uint32_t *)((uint32_t)pool + pool->poolPhysicalSize + sizeof(struct buddy));
        currentBuddy->nextBuddy = NULL;

        pool->poolPhysicalSize += sizeof(struct buddy) + (currentBuddy->mapWordCount * 4);

        // if maxFreeBlocks is equal to the actual number of free blocks, this is the highest order buddy
        if (currentBuddy->maxFreeBlocks == currentBuddy->freeBlocks)
            memset(currentBuddy->bitMap, 0, currentBuddy->mapWordCount * 4); // set entire region to available

        // else, this is a lower order buddy - an extra block at the end might be available
        else
        {
            memset(currentBuddy->bitMap, 0xFF, currentBuddy->mapWordCount * 4); // set entire region to reserved
            // check if a block at the end is actually available
            if (currentBuddy->freeBlocks > 0)
                unset_bit(currentBuddy->bitMap, (currentBuddy->maxFreeBlocks - currentBuddy->freeBlocks)); // set that one last directly usable block to 0
        }

        // linked list stuff
        if (pool->poolBuddiesTop == NULL)
        {
            pool->poolBuddiesTop = currentBuddy;
            currentBuddy->prevBuddy = NULL;
        }
        else
        {
            previousBuddy->nextBuddy = currentBuddy;
            currentBuddy->prevBuddy = previousBuddy;
        }
        previousBuddy = currentBuddy;
    }
    pool->poolBuddiesBottom = currentBuddy;
}

intmax_t findFirstFreeBit(uint32_t *map, uint32_t maxWords)
{
    uint32_t offset = 0;
    maxWords *= 32;
    logf("\n-------------------\nMax offset : %d\n", maxWords);
    while (offset < maxWords)
    {
        logf("Offset : %d\n", offset);
        // find the first non zero word
        if (map[offset / 32] != 0xFFFFFFFF)
        {
            for (; offset < maxWords; offset++)
            {
                logf("\tOffset : %d\n", offset);
                if (test_bit(map, offset) == 0)
                {
                    logf("-------------------------\n\n");
                    return offset;
                }
            }

            printf("Returning -1 : internal\n");
            logf("-------------------------\n\n");
            return -1;
        }
        offset += 32;
    }
    logf("Returning -1 : external\n");
    logf("-------------------------\n\n");
    return -1;
}

// debugging
void printBuddyBitMap(uint32_t *map, uint32_t wordCount)
{
    for (uint32_t i = 0; i < wordCount && i < 50; i++)
        logf(" %x | ", map[i]);
    logf("\n");
}

void printZoneInfo(struct zone *zone)
{
    logf("Zone info @ %x: \n", zone);
    logf("  free: %d blocks\n", zone->freeBlocks);
    logf("  physicalSize: %x\n\n", zone->zonePhysicalSize);
    struct pool *p = zone->poolStart;
    struct buddy *b;
    while (p != NULL)
    {
        logf("  Pool details: %x\n", p);
        logf("\tPoolStart : %x\n", p->start);
        logf("\tfree blocks : %d blocks\n", p->freeBlocks);
        b = p->poolBuddiesTop;
        while (b != NULL)
        {
            logf("\tBuddy of order %d @ %x:\n", b->buddyOrder, b);
            logf("\t\tMapWordCount : %d\n", b->mapWordCount);
            logf("\t\tMaxFreeBlocks: %d\n", b->maxFreeBlocks);
            logf("\t\tRealFreeBlocks: %d\n", b->freeBlocks);
            logf("\t\tPrevious Buddy is @: %x\n", (b->prevBuddy) ? b->prevBuddy : 0);
            logf("\t\tBitMap @ %x: ", b->bitMap);
            printBuddyBitMap(b->bitMap, b->mapWordCount);
            logf("\n");
            b = b->nextBuddy;
        }
        p = p->nextPool;
    }
    logf("---------------------------------------------\n\n");
}
