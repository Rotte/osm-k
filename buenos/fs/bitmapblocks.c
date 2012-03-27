/*
 * Access to bitmap blocks for NewFS
 *
 *
 */

/** 
 * This module implements bitmap operations that span blocks on disk.  All
 * operations on bitmap blocks use a fixed block size (given by the file
 * system, and a parameter indicating how many blocks are used for the
 * bitmap.
 */

#include "lib/bitmap.h"
#include "kernel/panic.h"
#include "kernel/assert.h"

#include "lib/libc.h"

/* In NEWFS block size is 512 bytes */
#define NEWFS_BLOCK_SIZE 512
/* 128 uint32s fit into a block. Used in declaration */
#define NEWFS_BLOCK_INTS 128
/* and the number of bits (always used where an expression is OK) */
#define NEWFS_BLOCK_BITS (NEWFS_BLOCK_SIZE*8)


/* should be removed when the real struct is in place */
struct dummy {
  void *disk;
};
struct dummy *newfs;
void disk_action(void *disk, uint32_t block, uint32_t buf, int rw) { 
  disk = (void*) buf + block + rw; 
}

/* Bitmap functions defined here */
int blockbitmap_get(int pos, int l);
void blockbitmap_set(int pos, int l, int value);
int blockbitmap_findnset(int l);

/* buffer for bitmap blocks to work on */
uint32_t block_buffer[NEWFS_BLOCK_INTS];

/**
 * Gets the value of a given bit in the bitmap.
 *
 * @param pos The position of the bit, whose value will be returned.
 *
 * @param l The number of blocks (== size of multi-block bitmap in bits)
 *
 * @return The value (0 or 1) of the given bit in the bitmap.
 */
int blockbitmap_get(int pos, int l)
{
    KERNEL_ASSERT(pos >= 0);
    KERNEL_ASSERT(pos < l);

    /* first, load correct block into block buffer */
    /* using a disk_action function similar to the one for flatfs, 0 for
       read access */
    disk_action(newfs->disk /* which we do not have*/, 
                pos / (NEWFS_BLOCK_BITS), (uint32_t) block_buffer, 0);

    /* then use bitmap function */
    return bitmap_get(block_buffer, pos % NEWFS_BLOCK_BITS);
}

/**
 * Sets the given bit in the bitmap.
 *
 * @param pos The index of the bit to set
 *
 * @param l The number of blocks (== size of multi-block bitmap in bits)
 *
 * @param value The new value of the given bit. Valid values are 0 and
 * 1.
 */
void blockbitmap_set(int pos, int l, int value)
{
    KERNEL_ASSERT(pos >= 0);
    KERNEL_ASSERT(pos < l);

    /* first, load correct block into block buffer */
    /* using a disk_action function similar to the one for flatfs, 0 for
       read access */
    disk_action(newfs->disk /* which we do not have*/, 
                pos / NEWFS_BLOCK_BITS, (uint32_t) block_buffer, 0);

    /* then use bitmap function */
    bitmap_set(block_buffer, pos % NEWFS_BLOCK_BITS, value);

    /* and write back the block */
    disk_action(newfs->disk /* which we do not have*/, 
                pos / NEWFS_BLOCK_BITS, (uint32_t) block_buffer, 1);
}


/**
 * Finds first zero and sets it to one.
 * 
 * @param l The number of blocks (== size of multi-block bitmap in bits)
 * 
 * @return Number of bit set. Negative if failed.
 */

int blockbitmap_findnset(int l)
{
  int block, result, max_block;

  KERNEL_ASSERT(l >= 0);

  /* Loop through blocks until a free bit is found. When found, write back
     the block to disk */
  max_block = (l + NEWFS_BLOCK_BITS - 1) / NEWFS_BLOCK_BITS;
  result = -1;
  block = 0;
  while ((result == -1) && (block < max_block)) {
      
    /* read a block and try to find a free bit */
    disk_action(newfs->disk /* which we do not have*/, 
                block, (uint32_t) block_buffer, 0);
    result = bitmap_findnset(block_buffer, 
                             l < NEWFS_BLOCK_BITS ? l : NEWFS_BLOCK_BITS);
    
    if (result != -1) {
      /* read a block and try to find a free bit */
      disk_action(newfs->disk /* which we do not have*/, 
                  block, (uint32_t) block_buffer, 1);
      return (block * NEWFS_BLOCK_BITS + result);
    }

    l = l - NEWFS_BLOCK_BITS;
  }

  /* No free slots found */
  return -1;

}

/** @} */
