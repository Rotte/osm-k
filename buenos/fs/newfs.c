/*
 * NewFS skeleton for OSM exam at DIKU 2012.
 *
 * Based on FlatFS implementation by Søren Dahlgaard.  Prepared by
 * Troels Henriksen.  You will need to change this file.
 */

#include "kernel/kmalloc.h"
#include "kernel/assert.h"
#include "vm/pagepool.h"
#include "drivers/gbd.h"
#include "fs/vfs.h"
#include "fs/newfs.h"
#include "lib/libc.h"
#include "lib/bitmap.h"

/* Data structure for use internally in newfs. We allocate space for this
 * dynamically during initialization */
typedef struct {
  /* Total number of blocks of the disk */
  uint32_t       totalblocks;

  /* Pointer to gbd device performing tfs */
  gbd_t          *disk;

  /* lock for mutual exclusion of fs-operations (we support only
     one operation at a time in any case) */
  semaphore_t    *lock;

  /* Buffers for read/write operations on disk. */
  newfs_inode_t     *buffer_inode;   /* buffer for inode blocks */
  bitmap_t          *buffer_bat;     /* buffer for allocation block */
  newfs_direntry_t  *buffer_md;      /* buffer for directory block */
  uint32_t          *buffer_si;      /* buffer for single indirect block */
  uint32_t          *buffer_di;      /* buffer for double indirect block */
} newfs_t;

enum { READ_OP, WRITE_OP };

/* Auxiliary function declarations */
/* Do stuff with the disk. read/write. */
int newfs_disk_action(gbd_t *disk, uint32_t block, uint32_t buf, int rw);
/* Get a slot in the bitmap. Release the lock if it wasn't possible.
   You have to modify this to handle multi-block allocation
   bitmaps. */
int newfs_bm_alloc(newfs_t *newfs, uint32_t *target);
/* Allocate a single indirect block on the disk and write it. We
 * assume that the buffers in the newfs are the ``working
 * buffers''. So the inode is the file, etc.  todbl indicates the
 * index of the double-indir we're writing. -1 if none. */
int newfs_alloc_indir(newfs_t *newfs, int todbl, uint32_t numblocks, uint32_t offset);
int newfs_remove_indir(newfs_t *newfs, int todbl);
uint32_t newfs_getblock(newfs_t *newfs, uint32_t block, int indirbuf);
int newfs_alloc_blocks(newfs_t *newfs, uint32_t numblocks, uint32_t offset);
void newfs_printfile_blocks(newfs_t *newfs, uint32_t inode);

/* The block of the root directory. */
int newfs_directory_block(newfs_t *newfs) {
 return NEWFS_ALLOCATION_BLOCK +
   newfs->totalblocks/(NEWFS_BLOCK_BITS) +
   ((newfs->totalblocks % NEWFS_BLOCK_BITS == 0) ? 0 : 1);
}

/* The following functions have not been implemented, but are a good
   baseline design for your NewFS implementation.  You are strongly
   recommended to implement and use them. */

/* Split a string at the first directory separator (forward slash).
   The first seperator will be replaced with a NUL byte, and a pointer
   to the immediately following character returned.  Hence, if given
   the string foo/bar/baz, the function should return a pointer to the
   string bar/baz, whilst the original pointer will now point to foo.
   Returns NULL if there is no directory seperator in the string. */
char *split_path(char *filepath);

/* Return the inode for the given filename in the given directory
   inode, or zero if it does not exist.  The filename should not
   contain directory separators. */
uint32_t newfs_inode_of_file(newfs_t *newfs, newfs_inode_t *dir, char *filename);

/* Starting from the root directory, find the inode of the file given
   by the path (which may contain directory separators).  Implement
   using split_path() and newfs_inode_of_file(). */
uint32_t newfs_inode_of_path(newfs_t *newfs, char *filepath);

/* Functions for dealing with multi-block bitmaps. */

/**
 * Gets the value of a given bit in the bitmap.
 *
 * @param pos The position of the bit, whose value will be returned.
 *
 * @param l The number of blocks (== size of multi-block bitmap in bits)
 *
 * @return The value (0 or 1) of the given bit in the bitmap.
 */
int blockbitmap_get(newfs_t *newfs, int pos, int l)
{
    uint32_t block_buffer[NEWFS_BLOCK_INTS];
    KERNEL_ASSERT(pos >= 0);
    KERNEL_ASSERT(pos < l);

    /* first, load correct block into block buffer */
    /* using a disk_action function similar to the one for flatfs, 0 for
       read access */
    newfs_disk_action(newfs->disk /* which we do not have*/, 
               (pos / NEWFS_BLOCK_BITS) + ((pos % NEWFS_BLOCK_BITS == 0) ? 0 : 1) + 1, (uint32_t) block_buffer, 0);

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
 *
 * @return 0 on failure.
 */
int blockbitmap_set(newfs_t *newfs, int pos, int l, int value)
{
    uint32_t block_buffer[NEWFS_BLOCK_INTS];
    KERNEL_ASSERT(pos >= 0);
    KERNEL_ASSERT(pos < l);

    /* first, load correct block into block buffer */
    /* using a disk_action function similar to the one for flatfs, 0 for
       read access */
    newfs_disk_action(newfs->disk /* which we do not have*/, 
                (pos / NEWFS_BLOCK_BITS) + ((pos % NEWFS_BLOCK_BITS == 0) ? 0 : 1) + 1, (uint32_t) block_buffer, 0);

    /* then use bitmap function */
    bitmap_set(block_buffer, pos % NEWFS_BLOCK_BITS, value);

    /* and write back the block */
    newfs_disk_action(newfs->disk /* which we do not have*/, 
		      (pos / NEWFS_BLOCK_BITS) + ((pos % NEWFS_BLOCK_BITS == 0) ? 0 : 1) + 1, (uint32_t) block_buffer, 1);
    
    return 1;
}
/**
 * Finds first zero and sets it to one.
 * 
 * @param l The number of blocks (== size of multi-block bitmap in bits)
 * 
 * @return Number of bit set. Negative if failed.
 */
int blockbitmap_findnset(newfs_t *newfs, int l)
{
  uint32_t block_buffer[NEWFS_BLOCK_INTS];
  int block, result, max_block;

  KERNEL_ASSERT(l >= 0);

  /* Loop through blocks until a free bit is found. When found, write back
     the block to disk */
  max_block = (l + NEWFS_BLOCK_BITS - 1) / NEWFS_BLOCK_BITS;
  result = -1;
  block = 0;
  while ((result == -1) && (block < max_block)) {
      
    /* read a block and try to find a free bit */
    newfs_disk_action(newfs->disk /* which we do not have*/, 
                block, (uint32_t) block_buffer, 0);
    result = bitmap_findnset(block_buffer, 
                             l < NEWFS_BLOCK_BITS ? l : NEWFS_BLOCK_BITS);
    
    if (result != -1) {
      /* read a block and try to find a free bit */
      newfs_disk_action(newfs->disk /* which we do not have*/, 
                  block, (uint32_t) block_buffer, 1);
      return (block * NEWFS_BLOCK_BITS + result);
    }

    l = l - NEWFS_BLOCK_BITS;
  }

  /* No free slots found */
  return -1;
}
/* For writing zero blocks. Should be all zeros because it's static! */
static uint32_t zeroblock[128];

/* Initialize newfs. We allocate one page of dynamic memory for the fs_t and
 * newfs_t structures. */
fs_t * newfs_init(gbd_t *disk) {
  uint32_t addr;
  char name[NEWFS_VOLUMENAME_MAX];
  fs_t *fs;
  newfs_t *newfs;
  int r;
  semaphore_t *sem;
  newfs_superblock *superblock;
  int totalblocks;

  if(disk->block_size(disk) != NEWFS_BLOCK_SIZE)
    return NULL;

  /* check semaphore availability before memory allocation */
  sem = semaphore_create(1);
  if (sem == NULL) {
    kprintf("newfs_init: could not create a new semaphore.\n");
    return NULL;
  }

  addr = pagepool_get_phys_page();
  if(addr == 0) {
    semaphore_destroy(sem);
    kprintf("newfs_init: could not allocate memory.\n");
    return NULL;
  }
  addr = ADDR_PHYS_TO_KERNEL(addr);   /* transform to vm address */
  superblock = (newfs_superblock*) addr;

  /* Assert that one page is enough */
  KERNEL_ASSERT(PAGE_SIZE >= (5*NEWFS_BLOCK_SIZE+sizeof(newfs_t)+sizeof(fs_t)));

  /* Read header block, and make sure this is newfs drive */
  r = newfs_disk_action(disk, 0, addr, READ_OP);
  if(r == 0) {
    semaphore_destroy(sem);
    pagepool_free_phys_page(ADDR_KERNEL_TO_PHYS(addr));
    kprintf("newfs_init: Error during disk read. Initialization failed.\n");
    return NULL;
  }
  if(superblock->magic != NEWFS_MAGIC) {
    semaphore_destroy(sem);
    pagepool_free_phys_page(ADDR_KERNEL_TO_PHYS(addr));
    return NULL;
  }

  /* Copy volume name from superblock. */
  stringcopy(name, superblock->name, NEWFS_VOLUMENAME_MAX);
  totalblocks = superblock->n_blocks;

  /* Now we reuse the memory where we put the superblock for our fs_t
     structure. */

  /* fs_t, newfs_t and all buffers in newfs_t fit in one page, so obtain
     addresses for each structure and buffer inside the allocated
     memory page. */
  fs  = (fs_t *)addr;
  newfs = (newfs_t *)(addr + sizeof(fs_t));
  newfs->buffer_inode = (newfs_inode_t *)((uint32_t)newfs + sizeof(newfs_t));
  newfs->buffer_bat  = (bitmap_t *)((uint32_t)newfs->buffer_inode +
                                    NEWFS_BLOCK_SIZE);
  newfs->buffer_md   = (newfs_direntry_t *)((uint32_t)newfs->buffer_bat +
                                            NEWFS_BLOCK_SIZE);
  newfs->buffer_si   = (uint32_t *)((uint32_t)newfs->buffer_md + NEWFS_BLOCK_SIZE);
  newfs->buffer_di   = (uint32_t *)((uint32_t)newfs->buffer_si + NEWFS_BLOCK_SIZE);

  newfs->totalblocks = totalblocks;
  newfs->disk        = disk;

  /* save the semaphore to the tfs_t */
  newfs->lock = sem;

  fs->internal = (void *)newfs;
  stringcopy(fs->volume_name, name, VFS_NAME_LENGTH);

  fs->unmount   = newfs_unmount;
  fs->open      = newfs_open;
  fs->close     = newfs_close;
  fs->create    = newfs_create;
  fs->remove    = newfs_remove;
  fs->read      = newfs_read;
  fs->write     = newfs_write;
  fs->getfree   = newfs_getfree;
  fs->filecount = newfs_filecount;
  fs->file      = newfs_file;
  fs->mkdir     = newfs_mkdir;
  fs->rmdir     = newfs_rmdir;

  /* Print some debugging data - you should probably remove this. */
  kprintf("NewFS: Total number of blocks: %d\n", newfs->totalblocks);
  kprintf("NewFS: Directory inode: %d\n", newfs_directory_block(newfs));
  newfs_disk_action(newfs->disk, newfs_directory_block(newfs), (uint32_t)newfs->buffer_inode, READ_OP);
  kprintf("Attr of root: %d\n", newfs->buffer_inode->properties);
  return fs;
}


/* Unmount a newfs system. */
int newfs_unmount(fs_t *fs)
{
  newfs_t *newfs = (newfs_t *)fs->internal;

  semaphore_P(newfs->lock);

  /* free semaphore and allocated memory */
  semaphore_destroy(newfs->lock);
  pagepool_free_phys_page(ADDR_KERNEL_TO_PHYS((uint32_t)fs));
  return VFS_OK;
}

/* Open a file. This simply returns the file's inode block number or
 * VFS_NOT_FOUND if the file doesn't exist. */
int newfs_open(fs_t *fs, char *filename)
{
  newfs_t *newfs;
  uint32_t i;
  int r;
  
  newfs = (newfs_t *)fs->internal;
  
  semaphore_P(newfs->lock);

  r = newfs_disk_action(newfs->disk, newfs_directory_block(newfs), 
			(uint32_t)newfs->buffer_md, 0);
  if(r == 0) {
    semaphore_V(newfs->lock);
    return VFS_ERROR;
  }

  for(i=0; i < NEWFS_MAX_FILES; ++i) {
    if(stringcmp(newfs->buffer_md[i].name, filename) == 0) {
      semaphore_V(newfs->lock);
      return newfs->buffer_md[i].inode;
    }
  }

  semaphore_V(newfs->lock);
  return VFS_NOT_FOUND;
}

/* Close a file. This does nothing */
int newfs_close(fs_t *fs, int fileid)
{
  fs = fs;
  fileid = fileid;

  return VFS_OK;
}

/* Create a file of the given size. This will potentially allocate both
 * single and double-indirect blocks.
 * This returns a file with all zeros!
 * Because we overwrite buffers we zero the blocks right away. This is okay
 * because we only write to unused blocks. The changed bitmap is only written
 * if everything succeeds! */
int newfs_create(fs_t *fs, char *filename, int size)
{
  newfs_t *newfs = (newfs_t *)fs->internal;
  uint32_t i;
  uint32_t numblocks = (size + NEWFS_BLOCK_SIZE - 1)/NEWFS_BLOCK_SIZE;
  int index = -1;
  int r;

  semaphore_P(newfs->lock);

  if (numblocks > NEWFS_MAX_BLOCKS) {
    semaphore_V(newfs->lock);
    return VFS_ERROR;
  }

  r = newfs_disk_action(newfs->disk, newfs_directory_block(newfs), 
			(uint32_t)newfs->buffer_md, 0);
  if(r == 0) {
    semaphore_V(newfs->lock);
    return VFS_ERROR;
  }

  for(i=0;i<NEWFS_MAX_FILES;i++) {
    if(stringcmp(newfs->buffer_md[i].name, filename) == 0) {
      semaphore_V(newfs->lock);
      return VFS_ERROR;
    }

    if(newfs->buffer_md[i].inode == 0)
      index = i;
  }

  if(index == -1) {
    semaphore_V(newfs->lock);
    return VFS_ERROR;
  }

  stringcopy(newfs->buffer_md[index].name,filename,NEWFS_FILENAME_MAX);

  r = newfs_disk_action(newfs->disk,NEWFS_ALLOCATION_BLOCK,
			(uint32_t)newfs->buffer_bat,0);
  if(r==0) {
    semaphore_V(newfs->lock);
    return VFS_ERROR;
  }

  if (newfs_bm_alloc(newfs, &(newfs->buffer_md[index].inode)) == -1) {
    semaphore_V(newfs->lock);
    return VFS_ERROR;
  }

  newfs->buffer_inode->filesize = size;
  newfs->buffer_inode->block_indir = 0;
  newfs->buffer_inode->block_dbl_indir = 0;

  if (numblocks > NEWFS_BLOCKS_DIRECT + NEWFS_BLOCKS_SINDIR && (newfs_bm_alloc(newfs, &(newfs->buffer_inode->block_dbl_indir)) == -1 || newfs_disk_action(newfs->disk, newfs->buffer_inode->block_dbl_indir, (uint32_t)zeroblock, 1) == 0))
    {
      semaphore_V(newfs->lock);
      return VFS_ERROR;
    }

  if (newfs_alloc_blocks(newfs, numblocks, 0) == VFS_ERROR) {
    semaphore_V(newfs->lock);
    return VFS_ERROR;
  }

  r = newfs_disk_action(newfs->disk, newfs_directory_block(newfs), (uint32_t)newfs->buffer_md, 1);
  if(r==0) {
    semaphore_V(newfs->lock);
    return VFS_ERROR;
  }

  r = newfs_disk_action(newfs->disk, newfs->buffer_md[index].inode, (uint32_t)newfs->buffer_inode, 1);
  if(r==0) {
    semaphore_V(newfs->lock);
    return VFS_ERROR;
  }

  semaphore_V(newfs->lock);
  return VFS_OK;					       
}

/* Delete a file from the system. */
int newfs_remove(fs_t *fs, char *filename)
{
  newfs_t *newfs = (newfs_t *)fs->internal;
  uint32_t i;
  int index = -1;
  int r;
  uint32_t numblocks;
  
  semaphore_P(newfs->lock);

  r = newfs_disk_action(newfs->disk, newfs_directory_block(newfs), (uint32_t)newfs->buffer_md, 0);
  if(r==0) {
    semaphore_V(newfs->lock);
    return VFS_ERROR;
  }
  
  for(i=0;i<NEWFS_MAX_FILES;i++) {
    if(stringcmp(newfs->buffer_md[i].name, filename) == 0) {
      index = i;
      break;
    }
  }
  if(index == -1) {
    semaphore_V(newfs->lock);
    return VFS_NOT_FOUND;
  }

  r = newfs_disk_action(newfs->disk, NEWFS_ALLOCATION_BLOCK, (uint32_t)newfs->buffer_bat, 0);
  if(r==0) {
    semaphore_V(newfs->lock);
    return VFS_ERROR;
  }

  r = newfs_disk_action(newfs->disk, newfs->buffer_md[index].inode, (uint32_t)newfs->buffer_inode, 0);
  if (r==0) {
    semaphore_V(newfs->lock);
    return VFS_ERROR;
  }

  numblocks = (newfs->buffer_inode->filesize + NEWFS_BLOCK_SIZE - 1) / NEWFS_BLOCK_SIZE;

  if (numblocks > NEWFS_BLOCKS_DIRECT + NEWFS_BLOCKS_SINDIR && newfs->buffer_inode->block_dbl_indir != 0) {
    if (newfs_disk_action(newfs->disk, newfs->buffer_inode->block_dbl_indir, (uint32_t)newfs->buffer_di, 0) == 0) {
      semaphore_V(newfs->lock);
      return VFS_ERROR;
    }

    uint32_t tmpblocks = numblocks - NEWFS_BLOCKS_DIRECT - NEWFS_BLOCKS_SINDIR;
    for (i=0; tmpblocks > 0; tmpblocks -= NEWFS_BLOCKS_SINDIR, ++i) {
      if (newfs_remove_indir(newfs, i) == VFS_ERROR)
	{
	  semaphore_V(newfs->lock);
	  return VFS_ERROR;
	}
    }
  }

  if(newfs->buffer_inode->block_indir > 0 && newfs_remove_indir(newfs, -1) == VFS_ERROR)
    {
      semaphore_V(newfs->lock);
      return VFS_ERROR;
    }

  blockbitmap_set(newfs, newfs->buffer_md[index].inode, newfs->totalblocks, 0);
  
  for(i=0; i < NEWFS_BLOCKS_DIRECT; ++i)
    if(newfs->buffer_inode->block[i] > 0)
      blockbitmap_set(newfs, newfs->buffer_inode->block[i], newfs->totalblocks, 0);

  newfs->buffer_md[index].inode = 0;
  newfs->buffer_md[index].name[0] = 0;

  r = newfs_disk_action(newfs->disk, NEWFS_ALLOCATION_BLOCK, (uint32_t)newfs->buffer_bat, 1);
  if(r == 0) {
    semaphore_V(newfs->lock);
    return VFS_ERROR;
  }

  r=newfs_disk_action(newfs->disk, newfs_directory_block(newfs), (uint32_t)newfs->buffer_bat, 1);
  if(r==0) {
    semaphore_V(newfs->lock);
    return VFS_ERROR;
  }

  r = newfs_disk_action(newfs->disk, newfs_directory_block(newfs), (uint32_t)newfs->buffer_md, 1);
  if(r==0) {
    semaphore_V(newfs->lock);
    return VFS_ERROR;
  }

  semaphore_V(newfs->lock);
  return VFS_OK;
}


/* Read at most bufsize bytes from file to the buffer. */
int newfs_read(fs_t *fs, int fileid, void *buffer, int bufsize, int offset)
{
    newfs_t *newfs = (newfs_t *)fs->internal;
    int b1, b2, b;
    int read=0;
    int r;
    uint32_t block;

    semaphore_P(newfs->lock);

    /* fileid is blocknum so ensure that we don't read system blocks
       or outside the disk */
    if(fileid < 2 || fileid > (int)newfs->totalblocks) {
        semaphore_V(newfs->lock);
        return VFS_ERROR;
    }

    r = newfs_disk_action(newfs->disk, fileid, (uint32_t)newfs->buffer_inode, 0);
    if(r == 0) {
        semaphore_V(newfs->lock);
        return VFS_ERROR;
    }

    /* Check that offset is inside the file */
    if(offset < 0 || offset > (int)newfs->buffer_inode->filesize) {
        semaphore_V(newfs->lock);
        return VFS_INVALID_PARAMS;
    }

    /* Read at most what is left from the file. */
    bufsize = MIN(bufsize,((int)newfs->buffer_inode->filesize) - offset);

    if(bufsize==0) {
        semaphore_V(newfs->lock);
        return 0;
    }

    /* first block to be read from the disk */
    b1 = offset / NEWFS_BLOCK_SIZE;

    /* last block to be read from the disk */
    b2 = (offset+bufsize-1) / NEWFS_BLOCK_SIZE;

    /* If the file has double-indirect blocks, read in the double-indirect
     * block right away. */
    if (newfs->buffer_inode->block_dbl_indir > 0 &&
            newfs_disk_action(newfs->disk, newfs->buffer_inode->block_dbl_indir,
                (uint32_t)newfs->buffer_di, 0) == 0)
    {
        semaphore_V(newfs->lock);
        return VFS_ERROR;
    }

    /* We do this to get the single-indirect buffer we need to start off
     * the loop. Now we can just check if the block is the first in a
     * single-indir block and only buffer the single-indir block in that case. */
    newfs_getblock(newfs, b1, 0);
    for (b = b1; b <= b2; ++b)
    {
        /* Get the physical block of the logical block. Potentially read a new
         * single-indir block into the buffer. */
        block = newfs_getblock(newfs, b,
                (b - NEWFS_BLOCKS_DIRECT) % NEWFS_BLOCKS_SINDIR);
        /* We read the data into the BAT buffer because we don't use it for
         * anything else. */
        r = newfs_disk_action(newfs->disk, block, (uint32_t)newfs->buffer_bat, 0);
        if (r == 0)
        {
            semaphore_V(newfs->lock);
            return VFS_ERROR;
        }
        if (b == b1)
        {
            /* First block. Handle the offset. */
            read = MIN(NEWFS_BLOCK_SIZE - (offset % NEWFS_BLOCK_SIZE), bufsize);
            memcopy(read, buffer, (const uint32_t *)(((uint32_t)newfs->buffer_bat) +
                        (offset % NEWFS_BLOCK_SIZE)));
            buffer = (void *)((uint32_t)buffer + read);
        }
        else if (b == b2)
        {
            memcopy(bufsize - read, buffer, (const uint32_t *)newfs->buffer_bat);
            read = bufsize;
        }
        else
        {
	   memcopy(NEWFS_BLOCK_SIZE, buffer, (const uint32_t *)newfs->buffer_bat);
            read += NEWFS_BLOCK_SIZE;
            buffer = (void *)((uint32_t)buffer + NEWFS_BLOCK_SIZE);
        }
    }

    semaphore_V(newfs->lock);
    return read;
}

/* Write at most datasize bytes to the file, possibly increasing the size of
 * file in the process. */
int newfs_write(fs_t *fs, int fileid, void *buffer, int datasize, int offset)
{
  newfs_t *newfs = (newfs_t *)fs->internal;
  uint32_t b1, b2, b, block;
  int written = 0;
  int r;

  semaphore_P(newfs->lock);

  if(fileid < 2 || fileid > (int)newfs->totalblocks || offset < 0)
    {
      semaphore_V(newfs->lock);
      return VFS_INVALID_PARAMS;
    }

  if (datasize == 0) {
    semaphore_V(newfs->lock);
    return 0;
  }

  r = newfs_disk_action(newfs->disk, fileid, (uint32_t)newfs->buffer_inode, 0);
  if(r==0) {
    semaphore_V(newfs->lock);
    return VFS_ERROR;
  }

  if(offset > (int)newfs->buffer_inode->filesize) {
    semaphore_V(newfs->lock);
    return VFS_ERROR;
  }

  b1 = offset / NEWFS_BLOCK_SIZE;

  b2 = (offset+datasize-1) / NEWFS_BLOCK_SIZE;

  uint32_t last_block = (newfs->buffer_inode->filesize - 1) / NEWFS_BLOCK_SIZE;
  if (newfs->buffer_inode->filesize == 0)
    last_block = 0;

  if (newfs->buffer_inode->block_dbl_indir > 0 && newfs_disk_action(newfs->disk, newfs->buffer_inode->block_dbl_indir, (uint32_t)newfs->buffer_di, 0) == 0) {
    semaphore_V(newfs->lock);
    return VFS_ERROR;
  }
 
  if (b2 > last_block || newfs->buffer_inode->filesize == 0) {
    if(newfs_disk_action(newfs->disk, NEWFS_ALLOCATION_BLOCK, (uint32_t)newfs->buffer_bat,0) == 0) {
      semaphore_V(newfs->lock);
      return VFS_ERROR;
    }

    if(newfs->buffer_inode->filesize == 0) {
      if(newfs_bm_alloc(newfs, &(newfs->buffer_inode->block[0])) == -1 || newfs_disk_action(newfs->disk, newfs->buffer_inode->block[0], (uint32_t)zeroblock, 1) == 0) {
	semaphore_V(newfs->lock);
	return VFS_ERROR;
      }
    }

    if (b2 >= NEWFS_BLOCKS_DIRECT + NEWFS_BLOCKS_SINDIR && last_block < NEWFS_BLOCKS_DIRECT + NEWFS_BLOCKS_SINDIR) {
      if(newfs_bm_alloc(newfs, &(newfs->buffer_inode->block_dbl_indir)) == -1 || newfs_disk_action(newfs->disk, newfs->buffer_inode->block_dbl_indir, (uint32_t)zeroblock, 1) == 0) {
	semaphore_V(newfs->lock);
	return VFS_ERROR;
      }
    }

    if (b2 > last_block)
      newfs_alloc_blocks(newfs,b2-last_block,last_block+1);
    else if (newfs_disk_action(newfs->disk, NEWFS_ALLOCATION_BLOCK, (uint32_t)newfs->buffer_bat, 1) == 0) {
      semaphore_V(newfs->lock);
      return VFS_ERROR;
    }
  }

  newfs_getblock(newfs, b1, 0);
  for (b = b1; b <= b2; ++b) {
    block = newfs_getblock(newfs,b,(b-NEWFS_BLOCKS_DIRECT) % NEWFS_BLOCKS_SINDIR);
    if (b == b1) {
      written = MIN(NEWFS_BLOCK_SIZE - (offset % NEWFS_BLOCK_SIZE), datasize);
      if (newfs_disk_action(newfs->disk, block, (uint32_t)newfs->buffer_bat, 0) == 0) {
	semaphore_V(newfs->lock);
	return VFS_ERROR;
      }
      memcopy(written, (uint32_t *)(((uint32_t)newfs->buffer_bat) + (offset % NEWFS_BLOCK_SIZE)), buffer);
      buffer = (void *)((uint32_t)buffer + written);
    }
    else if (b == b2) {
      if(newfs_disk_action(newfs->disk, block, (uint32_t)newfs->buffer_bat, 0) == 0) {
	semaphore_V(newfs->lock);
	return VFS_ERROR;
      }
      memcopy(datasize - written, (uint32_t *)newfs->buffer_bat, buffer);
      written = datasize;
    }
    else {
      memcopy(NEWFS_BLOCK_SIZE, (uint32_t *)newfs->buffer_bat, buffer);
      written += NEWFS_BLOCK_SIZE;
      buffer = (void *)((uint32_t)buffer + NEWFS_BLOCK_SIZE);
    }
    
    if(newfs_disk_action(newfs->disk, block, (uint32_t)newfs->buffer_bat, 1) == 0) {
      semaphore_V(newfs->lock);
      return VFS_ERROR;
    }
  }

  if ((uint32_t)offset + datasize > newfs->buffer_inode->filesize) {
    newfs->buffer_inode->filesize = (uint32_t)offset + datasize;
    if(newfs_disk_action(newfs->disk, fileid, (uint32_t)newfs->buffer_inode,1) == 0) {
      semaphore_V(newfs->lock);
      return VFS_ERROR;
    }
  }
  
  semaphore_V(newfs->lock);
  return written;
}

/* Get the number of free bytes on the disk. */
int newfs_getfree(fs_t *fs)
{
  newfs_t *newfs = (newfs_t *)fs->internal;
  int allocated = 0;
  uint32_t i;
  int r;

  semaphore_P(newfs->lock);

  r = newfs_disk_action(newfs->disk, NEWFS_ALLOCATION_BLOCK, (uint32_t)newfs->buffer_bat, 0);
  if(r == 0) {
    semaphore_V(newfs->lock);
    return VFS_ERROR;
  }

  for(i=0;i<newfs->totalblocks;i++) {
    allocated += blockbitmap_get(newfs, newfs->totalblocks, i);
  }

  semaphore_V(newfs->lock);
  return (newfs->totalblocks - allocated)*NEWFS_BLOCK_SIZE;
}

/* Get the count of files in the directory if it exists (ie. only the
 * master directory is accepted. */
int newfs_filecount(fs_t *fs, char *dirname)
{
  newfs_t *newfs = (newfs_t *)fs->internal;
  uint32_t i;
  int r;
  int count = 0;

  if(stringcmp(dirname, "") != 0)
    return -VFS_NOT_FOUND;

  semaphore_P(newfs->lock);

  r = newfs_disk_action(newfs->disk, newfs_directory_block(newfs), (uint32_t)newfs->buffer_md, 0);
  if(r == 0) {
    semaphore_V(newfs->lock);
    return VFS_ERROR;
  }

  for(i=0; i < NEWFS_MAX_FILES; ++i)
    if(newfs->buffer_md[i].inode != 0)
      ++count;

  semaphore_V(newfs->lock);
  return count;
}

/* Get the name of the file with index idx in the directory dirname.
 * There is only one directory in newfs, so we check that dirname == "".
 * This function is ineffective and we could improve it by guaranteeing
 * the file indexes used are the n first. */
int newfs_file(fs_t *fs, char *dirname, int idx, char *buffer)
{
  newfs_t *newfs = (newfs_t *)fs->internal;
  uint32_t i;
  int r;
  int count = 0;

  if(stringcmp(dirname, "") != 0 || idx < 0)
    return VFS_NOT_FOUND;
  
  semaphore_P(newfs->lock);

  r=newfs_disk_action(newfs->disk, newfs_directory_block(newfs), (uint32_t)newfs->buffer_md, 0);
  if(r == 0) {
    semaphore_V(newfs->lock);
    return VFS_ERROR;
  }

  for(i=0; i < NEWFS_MAX_FILES; ++i) {
    if(newfs->buffer_md[i].inode != 0 && count++ == idx) {
      stringcopy(buffer, newfs->buffer_md[i].name, NEWFS_FILENAME_MAX);
      semaphore_V(newfs->lock);
      return VFS_ERROR;
    }
  }
  
  semaphore_V(newfs->lock);
  return VFS_NOT_FOUND;
}

int newfs_mkdir(fs_t *fs, char *dirname)
{
    fs=fs;
    dirname=dirname;
    return VFS_NOT_SUPPORTED;
}

int newfs_rmdir(fs_t *fs, char *dirname)
{
    fs=fs;
    dirname=dirname;
    return VFS_NOT_SUPPORTED;
}


/* ====================================
 * Auxilliary functions
 * ==================================== */

/* Do something with the disk. rw = 1 for write, 0 for read. */
int newfs_disk_action(gbd_t *disk, uint32_t block, uint32_t buf, int rw)
{
  gbd_request_t req;
  int r = 0;

  req.block = block;
  req.sem   = NULL;
  req.buf = ADDR_KERNEL_TO_PHYS(buf);
  if (rw == READ_OP)
    r = disk->read_block(disk, &req);
  else if (rw == WRITE_OP)
    r = disk->write_block(disk, &req);

  return r;
}

int newfs_bm_alloc(newfs_t *newfs, uint32_t *target)
{
  *target = blockbitmap_findnset(newfs, newfs->totalblocks);
  return *target;
}

int newfs_alloc_indir(newfs_t *newfs, int todbl, uint32_t numblocks, uint32_t offset)
{
  uint32_t i;

  /* Are we writing to the single indir or to a double indir? */
  uint32_t *ind = &(newfs->buffer_inode->block_indir);
  if (todbl >= 0)
    ind = &(newfs->buffer_di[todbl]);

  /* Get the indirection block from the bitmap */
  if (*ind == 0 && newfs_bm_alloc(newfs, ind) == -1)
    return VFS_ERROR;
  if (newfs_disk_action(newfs->disk, *ind, (uint32_t)newfs->buffer_si, READ_OP) == 0)
    return VFS_ERROR;

  /* Get blocks for the data and zero them. */
  for (i = offset; i < offset+numblocks; ++i) {
    if (newfs_bm_alloc(newfs, &(newfs->buffer_si[i])) == -1)
      return VFS_ERROR;
    if (newfs_disk_action(newfs->disk, newfs->buffer_si[i],
                          (uint32_t)zeroblock, WRITE_OP) == 0)
      return VFS_ERROR;
  }

  /* Set extra spots to 0. */
  for (i = offset+numblocks; i < NEWFS_BLOCKS_SINDIR; ++i)
    newfs->buffer_si[i] = 0;

  /* Store the indirection block on the disk. */
  if (newfs_disk_action(newfs->disk, *ind, (uint32_t)newfs->buffer_si, WRITE_OP) == 0)
    return VFS_ERROR;

  return VFS_OK;
}

/* Allocate numblocks starting at offset. */
int newfs_alloc_blocks(newfs_t *newfs, uint32_t numblocks, uint32_t offset)
{
  uint32_t i, tmpblocks;
  uint32_t start, towrite;
  int r;

  if (offset < NEWFS_BLOCKS_DIRECT) {
    /* Allocate the direct blocks. */
    tmpblocks = MIN(NEWFS_BLOCKS_DIRECT - offset, numblocks);
    numblocks -= tmpblocks;
    for(i=offset; i<offset+tmpblocks; ++i) {
      if (newfs_bm_alloc(newfs, &(newfs->buffer_inode->block[i])) == -1)
        return VFS_ERROR;
      /* Zero the block right away. It doesn't matter if we fail because the
       * block is not used. As long as we don't write the bitmap we're good! */
      if (newfs_disk_action(newfs->disk, newfs->buffer_inode->block[i],
                            (uint32_t)zeroblock, WRITE_OP) == 0)
        return VFS_ERROR;
    }
    for (i = tmpblocks + offset; i < NEWFS_BLOCKS_DIRECT; ++i)
      newfs->buffer_inode->block[i] = 0;
    offset = NEWFS_BLOCKS_DIRECT;
  }

  if (numblocks > 0 && offset < NEWFS_BLOCKS_DIRECT + NEWFS_BLOCKS_SINDIR) {
    /* Allocate single indir blocks */
    tmpblocks = MIN(numblocks, NEWFS_BLOCKS_SINDIR + NEWFS_BLOCKS_DIRECT - offset);
    numblocks -= tmpblocks;

    if (newfs_alloc_indir(newfs, -1, tmpblocks,
                          offset - NEWFS_BLOCKS_DIRECT) == VFS_ERROR)
      return VFS_ERROR;
    offset = NEWFS_BLOCKS_DIRECT + NEWFS_BLOCKS_SINDIR;
  }

  if (numblocks > 0) {
    /* We assume newfs->buffer_inode->block_dbl_indir is the double-indir block. */
    r = newfs_disk_action(newfs->disk, newfs->buffer_inode->block_dbl_indir,
                          (uint32_t)newfs->buffer_di, READ_OP);
    if (r == 0)
      return VFS_ERROR;

    offset -= NEWFS_BLOCKS_DIRECT + NEWFS_BLOCKS_SINDIR;

    start = offset / NEWFS_BLOCKS_SINDIR;
    offset = offset % NEWFS_BLOCKS_SINDIR;

    for (i=start; numblocks > 0; ++i)
      {
        towrite = MIN(numblocks, NEWFS_BLOCKS_SINDIR - offset);
        if (newfs_alloc_indir(newfs, i, towrite, offset) == VFS_ERROR)
          return VFS_ERROR;
        numblocks -= towrite;
        offset = 0;
      }
    if (newfs_disk_action(newfs->disk, newfs->buffer_inode->block_dbl_indir,
                          (uint32_t)newfs->buffer_di, WRITE_OP) == 0)
      return VFS_ERROR;
  }

  r = newfs_disk_action(newfs->disk, NEWFS_ALLOCATION_BLOCK,
                        (uint32_t)newfs->buffer_bat, WRITE_OP);
  if(r==0)
    return VFS_ERROR;

  return VFS_OK;
}

int newfs_remove_indir(newfs_t *newfs, int todbl)
{
  uint32_t i;

  uint32_t *ind = &(newfs->buffer_inode->block_indir);
  if (todbl >= 0)
    ind = &(newfs->buffer_di[todbl]);

  if (newfs_disk_action(newfs->disk, *ind, (uint32_t)newfs->buffer_si, READ_OP) == 0)
    return VFS_ERROR;

  for (i = 0; i < NEWFS_BLOCKS_SINDIR; ++i)
    if (newfs->buffer_si[i] > 0)
      blockbitmap_set(newfs, newfs->buffer_si[i], newfs->totalblocks, 0);

  return VFS_OK;
}

uint32_t newfs_getblock(newfs_t *newfs, uint32_t block, int indirbuf)
{
  /* direct block? */
  uint32_t indir;
  if (block < NEWFS_BLOCKS_DIRECT)
    return newfs->buffer_inode->block[block];

  /* single indirect block? */
  block -= NEWFS_BLOCKS_DIRECT;

  if (block < NEWFS_BLOCKS_SINDIR) {
    if (indirbuf == 0 &&
        newfs_disk_action(newfs->disk, newfs->buffer_inode->block_indir,
                          (uint32_t)newfs->buffer_si, READ_OP) == 0)
      return VFS_ERROR;
    return newfs->buffer_si[block];
  }

  /* double indirect block? */
  block -= NEWFS_BLOCKS_SINDIR;
  indir = block / NEWFS_BLOCKS_SINDIR;
  block = block % NEWFS_BLOCKS_SINDIR;

  if (indirbuf == 0 &&
      newfs_disk_action(newfs->disk, newfs->buffer_di[indir],
                        (uint32_t)newfs->buffer_si, READ_OP) == 0)
    return VFS_ERROR;
  return newfs->buffer_si[block];
}

/* Split a string at the first directory separator (forward slash).
   The first seperator will be replaced with a NUL byte, and a pointer
   to the immediately following character returned.  Hence, if given
   the string foo/bar/baz, the function should return a pointer to the
   string bar/baz, whilst the original pointer will now point to foo.
   Returns NULL if there is no directory seperator in the string. */
char *split_path(char *filepath) {
  char *new;
  start = filepath;
  
  while (*filepath != '/' || *filepath != NULL)
    filepath++;

  if (*filepath == NULL)
    return NULL;

  *filepath = NULL;
  new = filepath++;

  return new;
}

/* Return the inode for the given filename in the given directory
   inode, or zero if it does not exist.  The filename should not
   contain directory separators. */
uint32_t newfs_inode_of_file(newfs_t *newfs, newfs_inode_t *dir, char *filename);

/* Starting from the root directory, find the inode of the file given
   by the path (which may contain directory separators).  Implement
   using split_path() and newfs_inode_of_file(). */
uint32_t newfs_inode_of_path(newfs_t *newfs, char *filepath);




/* TODO: Remove this when done debugging!
   void newfs_printfile_blocks(newfs_t *newfs, uint32_t inode)
   {
   uint32_t i, j;

   newfs_disk_action(newfs->disk, inode, (uint32_t)newfs->buffer_inode, 0);

   kprintf("========= File blocks =========\n");
   kprintf("direct blocks: %d %d %d %d %d %d %d\n", 
   newfs->buffer_inode->block[0],
   newfs->buffer_inode->block[1],
   newfs->buffer_inode->block[2],
   newfs->buffer_inode->block[3],
   newfs->buffer_inode->block[4],
   newfs->buffer_inode->block[5],
   newfs->buffer_inode->block[6]);
   if (newfs->buffer_inode->block_indir > 0)
   {
   kprintf("indir blocks: ");
   newfs_disk_action(newfs->disk, newfs->buffer_inode->block_indir,
   (uint32_t)newfs->buffer_si, 0);
   for (i = 0; i < NEWFS_BLOCKS_SINDIR; ++i)
   if (newfs->buffer_si[i] != 0)
   kprintf("%d ", newfs->buffer_si[i]);
   kprintf("\n");
   }
   if (newfs->buffer_inode->block_dbl_indir > 0)
   {
   newfs_disk_action(newfs->disk, newfs->buffer_inode->block_dbl_indir,
   (uint32_t)newfs->buffer_di, 0);
   kprintf("double blocks: ");
   for (j = 0; newfs->buffer_di[j] != 0; ++j)
   {
   newfs_disk_action(newfs->disk, newfs->buffer_di[j],
   (uint32_t)newfs->buffer_si, 0);
   for (i = 0; i < NEWFS_BLOCKS_SINDIR; ++i)
   if (newfs->buffer_si[i] != 0)
   kprintf("%d ", newfs->buffer_si[i]);
   kprintf("\n");
   }
   }
   kprintf("========= End  blocks =========\n");
   }
*/

/** @} */
