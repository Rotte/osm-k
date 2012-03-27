/*
 * FlatFS source file for OSM exam at DIKU 2012.
 *
 * This file implements the FlatFS file system driver. It also
 * defines the functions specified by the fs_t structure of buenos.
 *
 * This file also defines a lot of useful helper functions described
 * below. Some of these functions make a lot of assumptions and relies
 * on newly allocated blocks to be zeroed, so be careful when using them.
 *
 * @author Søren Dahlgaard
 */


#include "kernel/kmalloc.h"
#include "kernel/assert.h"
#include "vm/pagepool.h"
#include "drivers/gbd.h"
#include "fs/vfs.h"
#include "fs/flatfs.h"
#include "lib/libc.h"
#include "lib/bitmap.h"

/* Data structure for use internally in flatfs. We allocate space for this
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
    flatfs_inode_t    *buffer_inode;   /* buffer for inode blocks */
    bitmap_t          *buffer_bat;     /* buffer for allocation block */
    flatfs_direntry_t *buffer_md;      /* buffer for directory block */
    uint32_t          *buffer_si;      /* buffer for single indirect block */
    uint32_t          *buffer_di;      /* buffer for double indirect block */
} flatfs_t;

/************************************
 * Auxiliary function declarations
 ************************************/

/* Do stuff with the disk. read/write. */
int flatfs_disk_action(gbd_t *disk, uint32_t block, uint32_t buf, int rw);

/* Get a slot in the bitmap. */
int flatfs_bm_alloc(flatfs_t *flatfs, uint32_t *target);

/* Allocate data blocks for a single-indirect block. Possibly get a slot
 * for the single-indirect block itself. numblocks specifies the number
 * of data blocks to allocate and offset is the index of the first block.
 * todbl indicates whether this single-indirect block is in a
 * double-indirect or not. If it is, todbl is the index of the
 * single-indirect block. */
int flatfs_alloc_indir(flatfs_t *flatfs, int todbl, uint32_t numblocks, uint32_t offset);

/* Unset the block allocation table entries for a indirect block. */
int flatfs_remove_indir(flatfs_t *flatfs, int todbl);

/* Get the physical block for a "logical block". indirbuf indicates
 * whether the flatfs->buffer_si is the correct one for the logical block.
 * Eg. for use in a loop. Note that this makes the function a bit "hard"
 * to use properly! */
uint32_t flatfs_getblock(flatfs_t *flatfs, uint32_t block, int indirbuf);

/* Allocate numblocks blocks to a file starting at offset. This function
 * uses flatfs_alloc_indir a lot. */
int flatfs_alloc_blocks(flatfs_t *flatfs, uint32_t numblocks, uint32_t offset);

/* For writing zero blocks. Should be all zeros because it's static! */
static uint32_t zeroblock[128];

/***********************************
 * fs_t function implementations
 ***********************************/

/* Initialize flatfs. We allocate one page of dynamic memory for the fs_t and
 * flatfs_t structures. */
fs_t * flatfs_init(gbd_t *disk)
{
    uint32_t addr;
    char name[FLATFS_VOLUMENAME_MAX];
    fs_t *fs;
    flatfs_t *flatfs;
    int r;
    semaphore_t *sem;

    if(disk->block_size(disk) != FLATFS_BLOCK_SIZE)
        return NULL;

    /* check semaphore availability before memory allocation */
    sem = semaphore_create(1);
    if (sem == NULL) {
        kprintf("flatfs_init: could not create a new semaphore.\n");
        return NULL;
    }

    addr = pagepool_get_phys_page();
    if(addr == 0) {
        semaphore_destroy(sem);
        kprintf("flatfs_init: could not allocate memory.\n");
        return NULL;
    }
    addr = ADDR_PHYS_TO_KERNEL(addr);      /* transform to vm address */


    /* Assert that one page is enough */
    KERNEL_ASSERT(PAGE_SIZE >= (5*FLATFS_BLOCK_SIZE+sizeof(flatfs_t)+sizeof(fs_t)));

    /* Read header block, and make sure this is tfs drive */
    r = flatfs_disk_action(disk, 0, addr, 0);
    if(r == 0) {
        semaphore_destroy(sem);
        pagepool_free_phys_page(ADDR_KERNEL_TO_PHYS(addr));
        kprintf("flatfs_init: Error during disk read. Initialization failed.\n");
        return NULL;
    }

    if(((uint32_t *)addr)[0] != FLATFS_MAGIC) {
        semaphore_destroy(sem);
        pagepool_free_phys_page(ADDR_KERNEL_TO_PHYS(addr));
        return NULL;
    }

    /* Copy volume name from header block. */
    stringcopy(name, (char *)(addr+4), FLATFS_VOLUMENAME_MAX);

    /* fs_t, flatfs_t and all buffers in flatfs_t fit in one page, so obtain
       addresses for each structure and buffer inside the allocated
       memory page. */
    fs  = (fs_t *)addr;
    flatfs = (flatfs_t *)(addr + sizeof(fs_t));
    flatfs->buffer_inode = (flatfs_inode_t *)((uint32_t)flatfs + sizeof(flatfs_t));
    flatfs->buffer_bat  = (bitmap_t *)((uint32_t)flatfs->buffer_inode +
            FLATFS_BLOCK_SIZE);
    flatfs->buffer_md   = (flatfs_direntry_t *)((uint32_t)flatfs->buffer_bat +
            FLATFS_BLOCK_SIZE);
    flatfs->buffer_si   = (uint32_t *)((uint32_t)flatfs->buffer_md + FLATFS_BLOCK_SIZE);
    flatfs->buffer_di   = (uint32_t *)((uint32_t)flatfs->buffer_si + FLATFS_BLOCK_SIZE);

    flatfs->totalblocks = MIN(disk->total_blocks(disk), 8*FLATFS_BLOCK_SIZE);
    flatfs->disk        = disk;

    /* save the semaphore to the tfs_t */
    flatfs->lock = sem;

    fs->internal = (void *)flatfs;
    stringcopy(fs->volume_name, name, VFS_NAME_LENGTH);

    fs->unmount   = flatfs_unmount;
    fs->open      = flatfs_open;
    fs->close     = flatfs_close;
    fs->create    = flatfs_create;
    fs->remove    = flatfs_remove;
    fs->read      = flatfs_read;
    fs->write     = flatfs_write;
    fs->getfree   = flatfs_getfree;
    fs->filecount = flatfs_filecount;
    fs->file      = flatfs_file;
    fs->mkdir     = flatfs_mkdir;
    fs->rmdir     = flatfs_rmdir;

    return fs;
}


/* Unmount a flatfs system. */
int flatfs_unmount(fs_t *fs)
{
    flatfs_t *flatfs = (flatfs_t *)fs->internal;

    semaphore_P(flatfs->lock);

    /* free semaphore and allocated memory */
    semaphore_destroy(flatfs->lock);
    pagepool_free_phys_page(ADDR_KERNEL_TO_PHYS((uint32_t)fs));
    return VFS_OK;
}


/* Open a file. This simply returns the file's inode block number or
 * VFS_NOT_FOUND if the file doesn't exist. */
int flatfs_open(fs_t *fs, char *filename)
{
    flatfs_t *flatfs;
    uint32_t i;
    int r;

    flatfs = (flatfs_t *)fs->internal;

    semaphore_P(flatfs->lock);

    /* Read the directory block and search through its entries comparing
     * the file names to the one we're searching for. */
    r = flatfs_disk_action(flatfs->disk, FLATFS_DIRECTORY_BLOCK,
            (uint32_t)flatfs->buffer_md, 0);
    if(r == 0) {
        semaphore_V(flatfs->lock);
        return VFS_ERROR;
    }

    for(i=0; i < FLATFS_MAX_FILES; ++i)
    {
        if(stringcmp(flatfs->buffer_md[i].name, filename) == 0) {
            semaphore_V(flatfs->lock);
            return flatfs->buffer_md[i].inode;
        }
    }

    semaphore_V(flatfs->lock);
    return VFS_NOT_FOUND;
}


/* Close a file. This does nothing */
int flatfs_close(fs_t *fs, int fileid)
{
    fs = fs;
    fileid = fileid;

    return VFS_OK;
}


/* Create a file of the given size. This will potentially allocate both
 * single and double-indirect blocks.
 * This returns a file with all zeros!
 * Because we overwrite the single-indirect buffers we have to zero the
 * data blocks right away - even though the create operation might not succeed!
 * This is okay because we only write to unused blocks and don't have to
 * worry about their values. The changed bitmap is only written
 * if everything succeeds! */
int flatfs_create(fs_t *fs, char *filename, int size)
{
    flatfs_t *flatfs = (flatfs_t *)fs->internal;
    uint32_t i;
    uint32_t numblocks = (size + FLATFS_BLOCK_SIZE - 1)/FLATFS_BLOCK_SIZE;
    int index = -1;
    int r;

    semaphore_P(flatfs->lock);

    /* File too big? */
    if (numblocks > FLATFS_MAX_BLOCKS) {
        semaphore_V(flatfs->lock);
        return VFS_ERROR;
    }

    r = flatfs_disk_action(flatfs->disk, FLATFS_DIRECTORY_BLOCK,
            (uint32_t)flatfs->buffer_md, 0);
    if(r == 0) {
        semaphore_V(flatfs->lock);
        return VFS_ERROR;
    }

    /* Find slot for file if it doesn't already exist. */
    for(i=0;i<FLATFS_MAX_FILES;i++) {
        if(stringcmp(flatfs->buffer_md[i].name, filename) == 0) {
            semaphore_V(flatfs->lock);
            return VFS_ERROR;
        }

        if(flatfs->buffer_md[i].inode == 0)
            index = i;
    }

    if(index == -1) {
        /* there was no space in directory, because index is not set */
        semaphore_V(flatfs->lock);
        return VFS_ERROR;
    }

    stringcopy(flatfs->buffer_md[index].name,filename, FLATFS_FILENAME_MAX);

    r = flatfs_disk_action(flatfs->disk, FLATFS_ALLOCATION_BLOCK,
            (uint32_t)flatfs->buffer_bat, 0);
    if(r==0) {
        semaphore_V(flatfs->lock);
        return VFS_ERROR;
    }

    /* Find a block for the file's inode. */
    if (flatfs_bm_alloc(flatfs, &(flatfs->buffer_md[index].inode)) == -1)
    {
        semaphore_V(flatfs->lock);
        return VFS_ERROR;
    }
    /* Store the default values in the inode buffer.
     * The alloc function relies on this! */
    flatfs->buffer_inode->filesize = size;
    flatfs->buffer_inode->block_indir = 0;
    flatfs->buffer_inode->block_dbl_indir = 0;
    /* If we need a double-indirect block, just allocate it right away. */
    if (numblocks > FLATFS_BLOCKS_DIRECT + FLATFS_BLOCKS_SINDIR &&
        (flatfs_bm_alloc(flatfs, &(flatfs->buffer_inode->block_dbl_indir)) == -1
         || flatfs_disk_action(flatfs->disk, flatfs->buffer_inode->block_dbl_indir, (uint32_t)zeroblock, 1) == 0))
    {
        semaphore_V(flatfs->lock);
        return VFS_ERROR;
    }

    /* Allocate ALL the blocks! This also writes the BAT to the disk. */
    if (flatfs_alloc_blocks(flatfs, numblocks, 0) == VFS_ERROR)
    {
        semaphore_V(flatfs->lock);
        return VFS_ERROR;
    }

    r = flatfs_disk_action(flatfs->disk, FLATFS_DIRECTORY_BLOCK,
            (uint32_t)flatfs->buffer_md, 1);
    if(r==0) {
        semaphore_V(flatfs->lock);
        return VFS_ERROR;
    }

    r = flatfs_disk_action(flatfs->disk, flatfs->buffer_md[index].inode,
            (uint32_t)flatfs->buffer_inode, 1);
    if(r==0) {
        semaphore_V(flatfs->lock);
        return VFS_ERROR;
    }

    semaphore_V(flatfs->lock);
    return VFS_OK;
}

/* Delete a file from the system. */
int flatfs_remove(fs_t *fs, char *filename)
{
    flatfs_t *flatfs = (flatfs_t *)fs->internal;
    uint32_t i;
    int index = -1;
    int r;
    uint32_t numblocks;

    semaphore_P(flatfs->lock);

    /* Find file and inode block number from directory block.
       If not found return VFS_NOT_FOUND. */
    r = flatfs_disk_action(flatfs->disk, FLATFS_DIRECTORY_BLOCK,
            (uint32_t)flatfs->buffer_md, 0);
    if(r == 0) {
        semaphore_V(flatfs->lock);
        return VFS_ERROR;
    }

    for(i=0;i<FLATFS_MAX_FILES;i++) {
        if(stringcmp(flatfs->buffer_md[i].name, filename) == 0) {
            index = i;
            break;
        }
    }
    if(index == -1) {
        semaphore_V(flatfs->lock);
        return VFS_NOT_FOUND;
    }

    /* Read allocation block of the device and inode block of the file.
       Free reserved blocks (marked in inode) from allocation block. */
    r = flatfs_disk_action(flatfs->disk, FLATFS_ALLOCATION_BLOCK,
            (uint32_t)flatfs->buffer_bat, 0);
    if(r == 0) {
        semaphore_V(flatfs->lock);
        return VFS_ERROR;
    }

    r = flatfs_disk_action(flatfs->disk, flatfs->buffer_md[index].inode,
            (uint32_t)flatfs->buffer_inode, 0);
    if(r == 0) {
        semaphore_V(flatfs->lock);
        return VFS_ERROR;
    }

    /* How many blocks do we need to free? */
    numblocks = (flatfs->buffer_inode->filesize + FLATFS_BLOCK_SIZE - 1) /
        FLATFS_BLOCK_SIZE;

    /* Clear the double-indirects. Remember we only have to clear the bitmap entries,
     * so this action is still "atomic". */
    if (numblocks > FLATFS_BLOCKS_DIRECT + FLATFS_BLOCKS_SINDIR &&
            flatfs->buffer_inode->block_dbl_indir != 0)
    {
        if (flatfs_disk_action(flatfs->disk, flatfs->buffer_inode->block_dbl_indir,
                    (uint32_t)flatfs->buffer_di, 0) == 0)
        {
            semaphore_V(flatfs->lock);
            return VFS_ERROR;
        }

        uint32_t tmpblocks = numblocks - FLATFS_BLOCKS_DIRECT - FLATFS_BLOCKS_SINDIR;
        for (i = 0; tmpblocks > 0; tmpblocks -= FLATFS_BLOCKS_SINDIR, ++i)
        {
            if (flatfs_remove_indir(flatfs, i) == VFS_ERROR)
            {
                semaphore_V(flatfs->lock);
                return VFS_ERROR;
            }
        }
    }

    /* Clear single indirect. */
    if (flatfs->buffer_inode->block_indir > 0 &&
            flatfs_remove_indir(flatfs, -1) == VFS_ERROR)
    {
        semaphore_V(flatfs->lock);
        return VFS_ERROR;
    }

    /* Clear the file inode and its direct blocks */
    bitmap_set(flatfs->buffer_bat, flatfs->buffer_md[index].inode, 0);
    for (i = 0; i < FLATFS_BLOCKS_DIRECT; ++i)
        if (flatfs->buffer_inode->block[i] > 0)
            bitmap_set(flatfs->buffer_bat, flatfs->buffer_inode->block[i], 0);

    /* Free directory entry. */
    flatfs->buffer_md[index].inode   = 0;
    flatfs->buffer_md[index].name[0] = 0;

    r = flatfs_disk_action(flatfs->disk, FLATFS_ALLOCATION_BLOCK,
            (uint32_t)flatfs->buffer_bat, 1);
    if(r == 0) {
        semaphore_V(flatfs->lock);
        return VFS_ERROR;
    }

    r = flatfs_disk_action(flatfs->disk, FLATFS_DIRECTORY_BLOCK,
            (uint32_t)flatfs->buffer_md, 1);
    if(r == 0) {
        semaphore_V(flatfs->lock);
        return VFS_ERROR;
    }

    semaphore_V(flatfs->lock);
    return VFS_OK;
}


/* Read at most bufsize bytes from file to the buffer. */
int flatfs_read(fs_t *fs, int fileid, void *buffer, int bufsize, int offset)
{
    flatfs_t *flatfs = (flatfs_t *)fs->internal;
    int b1, b2, b;
    int read=0;
    int r;
    uint32_t block;

    semaphore_P(flatfs->lock);

    /* fileid is blocknum so ensure that we don't read system blocks
       or outside the disk */
    if(fileid < 2 || fileid > (int)flatfs->totalblocks) {
        semaphore_V(flatfs->lock);
        return VFS_ERROR;
    }

    r = flatfs_disk_action(flatfs->disk, fileid, (uint32_t)flatfs->buffer_inode, 0);
    if(r == 0) {
        semaphore_V(flatfs->lock);
        return VFS_ERROR;
    }

    /* Check that offset is inside the file */
    if(offset < 0 || offset > (int)flatfs->buffer_inode->filesize) {
        semaphore_V(flatfs->lock);
        return VFS_INVALID_PARAMS;
    }

    /* Read at most what is left from the file. */
    bufsize = MIN(bufsize,((int)flatfs->buffer_inode->filesize) - offset);

    if(bufsize==0) {
        semaphore_V(flatfs->lock);
        return 0;
    }

    /* first block to be read from the disk */
    b1 = offset / FLATFS_BLOCK_SIZE;

    /* last block to be read from the disk */
    b2 = (offset+bufsize-1) / FLATFS_BLOCK_SIZE;

    /* If the file has double-indirect blocks, read in the double-indirect
     * block right away. */
    if (flatfs->buffer_inode->block_dbl_indir > 0 &&
            flatfs_disk_action(flatfs->disk, flatfs->buffer_inode->block_dbl_indir,
                (uint32_t)flatfs->buffer_di, 0) == 0)
    {
        semaphore_V(flatfs->lock);
        return VFS_ERROR;
    }

    /* We do this to get the single-indirect buffer we need to start off
     * the loop. Now we can just check if the block is the first in a
     * single-indir block and only buffer the single-indir block in that case. */
    flatfs_getblock(flatfs, b1, 0);
    for (b = b1; b <= b2; ++b)
    {
        /* Get the physical block of the logical block. Potentially read a new
         * single-indir block into the buffer. */
        block = flatfs_getblock(flatfs, b,
                (b - FLATFS_BLOCKS_DIRECT) % FLATFS_BLOCKS_SINDIR);
        /* We read the data into the BAT buffer because we don't use it for
         * anything else. */
        r = flatfs_disk_action(flatfs->disk, block, (uint32_t)flatfs->buffer_bat, 0);
        if (r == 0)
        {
            semaphore_V(flatfs->lock);
            return VFS_ERROR;
        }
        if (b == b1)
        {
            /* First block. Handle the offset. */
            read = MIN(FLATFS_BLOCK_SIZE - (offset % FLATFS_BLOCK_SIZE), bufsize);
            memcopy(read, buffer, (const uint32_t *)(((uint32_t)flatfs->buffer_bat) +
                        (offset % FLATFS_BLOCK_SIZE)));
            buffer = (void *)((uint32_t)buffer + read);
        }
        else if (b == b2)
        {
            memcopy(bufsize - read, buffer, (const uint32_t *)flatfs->buffer_bat);
            read = bufsize;
        }
        else
        {
            memcopy(FLATFS_BLOCK_SIZE, buffer, (const uint32_t *)flatfs->buffer_bat);
            read += FLATFS_BLOCK_SIZE;
            buffer = (void *)((uint32_t)buffer + FLATFS_BLOCK_SIZE);
        }
    }

    semaphore_V(flatfs->lock);
    return read;
}



/* Write at most datasize bytes to the file, possibly increasing the size of
 * file in the process. */
int flatfs_write(fs_t *fs, int fileid, void *buffer, int datasize, int offset)
{
    flatfs_t *flatfs = (flatfs_t *)fs->internal;
    uint32_t b1, b2, b, block;
    int written=0;
    int r;

    semaphore_P(flatfs->lock);

    /* fileid is blocknum so ensure that we don't read system blocks
       or outside the disk */
    if(fileid < 2 || fileid > (int)flatfs->totalblocks || offset < 0) {
        semaphore_V(flatfs->lock);
        return VFS_INVALID_PARAMS;
    }

    /* Nothing to write? Then we're done! */
    if (datasize == 0)
    {
        semaphore_V(flatfs->lock);
        return 0;
    }

    r = flatfs_disk_action(flatfs->disk, fileid, (uint32_t)flatfs->buffer_inode, 0);
    if(r == 0) {
        semaphore_V(flatfs->lock);
        return VFS_ERROR;
    }

    /* check that start position is inside the file or just past the last byte.
     * If it is not we will have a "whole" in the file, which makes no sense. */
    if(offset > (int)flatfs->buffer_inode->filesize) {
        semaphore_V(flatfs->lock);
        return VFS_ERROR;
    }

    /* first block to be written into */
    b1 = offset / FLATFS_BLOCK_SIZE;

    /* last block to be written into */
    b2 = (offset+datasize-1) / FLATFS_BLOCK_SIZE;

    /* Read in the dbl-indirect buffer if it's there. */
    uint32_t last_block = (flatfs->buffer_inode->filesize - 1) / FLATFS_BLOCK_SIZE;
    if (flatfs->buffer_inode->filesize == 0)
        last_block = 0;

    if (flatfs->buffer_inode->block_dbl_indir > 0 &&
            flatfs_disk_action(flatfs->disk, flatfs->buffer_inode->block_dbl_indir,
                (uint32_t)flatfs->buffer_di, 0) == 0)
    {
        semaphore_V(flatfs->lock);
        return VFS_ERROR;
    }

    /* if we need to expand the file we do this before doing any writing.
     * This makes handling the buffers a lot easier. */
    if (b2 > last_block || flatfs->buffer_inode->filesize == 0)
    {
        /* Read in the allocation table! We only need this when expanding
         * the file. */
        if (flatfs_disk_action(flatfs->disk, FLATFS_ALLOCATION_BLOCK,
                    (uint32_t)flatfs->buffer_bat, 0) == 0)
        {
            semaphore_V(flatfs->lock);
            return VFS_ERROR;
        }

        /* If the filesize is 0 we have to allocate the first block right away
         * to avoid an off-by-one error. */
        if (flatfs->buffer_inode->filesize == 0)
        {
            if (flatfs_bm_alloc(flatfs, &(flatfs->buffer_inode->block[0])) == -1 ||
                    flatfs_disk_action(flatfs->disk, flatfs->buffer_inode->block[0],
                        (uint32_t)zeroblock, 1) == 0)
            {
                semaphore_V(flatfs->lock);
                return VFS_ERROR;
            }
        }

        /* We allocate the dbl-indir block right away as well if needed. */
        if (b2 >= FLATFS_BLOCKS_DIRECT + FLATFS_BLOCKS_SINDIR &&
                last_block < FLATFS_BLOCKS_DIRECT + FLATFS_BLOCKS_SINDIR)
        {
            if (flatfs_bm_alloc(flatfs, &(flatfs->buffer_inode->block_dbl_indir)) == -1 ||
                    flatfs_disk_action(flatfs->disk, flatfs->buffer_inode->block_dbl_indir,
                        (uint32_t)zeroblock, 1) == 0)
            {
                semaphore_V(flatfs->lock);
                return VFS_ERROR;
            }
        }

        /* Allocate the blocks. This also writes the BAT to the disk!
         * This means that the write operation is not entirely atomic. :(
         * if b2 <= last_block, we store the bat manually (this is needed
         * when allocating the first block - ie. writing to a file of size 0.
         * stupid border cases :((( */
        if (b2 > last_block)
            flatfs_alloc_blocks(flatfs, b2 - last_block, last_block + 1);
        else if (flatfs_disk_action(flatfs->disk, FLATFS_ALLOCATION_BLOCK,
                    (uint32_t)flatfs->buffer_bat, 1) == 0)
        {
            semaphore_V(flatfs->lock);
            return VFS_ERROR;
        }
    }

    /* Get the single-indirect block of the first data block. */
    flatfs_getblock(flatfs, b1, 0);
    for (b = b1; b <= b2; ++b)
    {
        /* Get the physical block */
        block = flatfs_getblock(flatfs, b,
                (b - FLATFS_BLOCKS_DIRECT) % FLATFS_BLOCKS_SINDIR);
        if (b == b1)
        {
            /* First block to write. We need to read in the data block first
             * and overwrite only part of it! */
            written = MIN(FLATFS_BLOCK_SIZE - (offset % FLATFS_BLOCK_SIZE),
                        datasize);
            if (flatfs_disk_action(flatfs->disk, block,
                        (uint32_t)flatfs->buffer_bat, 0) == 0)
            {
                semaphore_V(flatfs->lock);
                return VFS_ERROR;
            }
            memcopy(written, (uint32_t *)(((uint32_t)flatfs->buffer_bat) +
                        (offset % FLATFS_BLOCK_SIZE)), buffer);
            buffer = (void *)((uint32_t)buffer + written);
        }
        else if (b == b2)
        {
            if (flatfs_disk_action(flatfs->disk, block,
                        (uint32_t)flatfs->buffer_bat, 0) == 0)
            {
                semaphore_V(flatfs->lock);
                return VFS_ERROR;
            }
            memcopy(datasize - written, (uint32_t *)flatfs->buffer_bat, buffer);
            written = datasize;
        }
        else
        {
            memcopy(FLATFS_BLOCK_SIZE, (uint32_t *)flatfs->buffer_bat, buffer);
            written += FLATFS_BLOCK_SIZE;
            buffer = (void *)((uint32_t)buffer + FLATFS_BLOCK_SIZE);
        }
        /* Finally, write the data block to the disk. */
        if (flatfs_disk_action(flatfs->disk, block,
                    (uint32_t)flatfs->buffer_bat, 1) == 0)
        {
            semaphore_V(flatfs->lock);
            return VFS_ERROR;
        }
    }

    /* If we increased the file size, this should be stored! */
    if ((uint32_t)offset + datasize > flatfs->buffer_inode->filesize)
    {
        flatfs->buffer_inode->filesize = (uint32_t)offset + datasize;
        if (flatfs_disk_action(flatfs->disk, fileid,
                    (uint32_t)flatfs->buffer_inode,1) == 0)
        {
            semaphore_V(flatfs->lock);
            return VFS_ERROR;
        }
    }

    semaphore_V(flatfs->lock);
    return written;
}

/* Get the number of free bytes on the disk. */
int flatfs_getfree(fs_t *fs)
{
    flatfs_t *flatfs = (flatfs_t *)fs->internal;
    int allocated = 0;
    uint32_t i;
    int r;

    semaphore_P(flatfs->lock);

    r = flatfs_disk_action(flatfs->disk, FLATFS_ALLOCATION_BLOCK,
            (uint32_t)flatfs->buffer_bat, 0);
    if(r == 0) {
        semaphore_V(flatfs->lock);
        return VFS_ERROR;
    }

    /* Count the amount of bits set in the bitmap. */
    for(i=0;i<flatfs->totalblocks;i++) {
        allocated += bitmap_get(flatfs->buffer_bat,i);
    }

    semaphore_V(flatfs->lock);
    return (flatfs->totalblocks - allocated)*FLATFS_BLOCK_SIZE;
}

/* Get the count of files in the directory if it exists (ie. only the
 * master directory is accepted. */
int flatfs_filecount(fs_t *fs, char *dirname)
{
    flatfs_t *flatfs = (flatfs_t *)fs->internal;
    uint32_t i;
    int r;
    int count = 0;

    if (stringcmp(dirname, "") != 0)
        return -VFS_NOT_FOUND;

    semaphore_P(flatfs->lock);

    r = flatfs_disk_action(flatfs->disk, FLATFS_DIRECTORY_BLOCK,
            (uint32_t)flatfs->buffer_md, 0);
    if(r == 0) {
        semaphore_V(flatfs->lock);
        return VFS_ERROR;
    }

    /* We have to go through all files because deleting a file will leave
     * a gap in the list. This could be fixed when deleting, but it is harder.
     * We don't need to handle indirect blocks because the master-directory of
     * flatfs is only one block. */
    for(i=0; i < FLATFS_MAX_FILES; ++i)
        if(flatfs->buffer_md[i].inode != 0)
            ++count;

    semaphore_V(flatfs->lock);
    return count;
}

/* Get the name of the file with index idx in the directory dirname.
 * There is only one directory in flatfs, so we check that dirname == "".
 * This function is ineffective and we could improve it by guaranteeing
 * the file indexes used are the n first. */
int flatfs_file(fs_t *fs, char *dirname, int idx, char *buffer)
{
    flatfs_t *flatfs = (flatfs_t *)fs->internal;
    uint32_t i;
    int r;
    int count = 0;

    if (stringcmp(dirname, "") != 0 || idx < 0)
        return VFS_NOT_FOUND;

    semaphore_P(flatfs->lock);

    r = flatfs_disk_action(flatfs->disk, FLATFS_DIRECTORY_BLOCK,
            (uint32_t)flatfs->buffer_md, 0);
    if(r == 0) {
        semaphore_V(flatfs->lock);
        return VFS_ERROR;
    }

    /* We can't just access buffer_mid[idx].name because there might be
     * gaps. We have to count our way through! */
    for(i=0; i < FLATFS_MAX_FILES; ++i)
    {
        if(flatfs->buffer_md[i].inode != 0 &&
                count++ == idx)
        {
            stringcopy(buffer, flatfs->buffer_md[i].name,
                    FLATFS_FILENAME_MAX);
            semaphore_V(flatfs->lock);
            return VFS_ERROR;
        }
    }

    semaphore_V(flatfs->lock);
    return VFS_NOT_FOUND;
}

int flatfs_mkdir(fs_t *fs, char *dirname)
{
    fs=fs;
    dirname=dirname;
    return VFS_NOT_SUPPORTED;
}

int flatfs_rmdir(fs_t *fs, char *dirname)
{
    fs=fs;
    dirname=dirname;
    return VFS_NOT_SUPPORTED;
}

/* ====================================
 * Auxilliary functions
 * ==================================== */

/* These functions are described at the top of the file, so not here! */

int flatfs_disk_action(gbd_t *disk, uint32_t block, uint32_t buf, int rw)
{
    gbd_request_t req;
    int r = 0;

    req.block = block;
    req.sem   = NULL;
    req.buf = ADDR_KERNEL_TO_PHYS(buf);
    if (rw == 0)
        r = disk->read_block(disk, &req);
    else if (rw == 1)
        r = disk->write_block(disk, &req);

    return r;
}

int flatfs_bm_alloc(flatfs_t *flatfs, uint32_t *target)
{
    /* This function is kinda unnecessary, but if we had bigger volumes it
     * could be nice as the bitmap-findnset function would be more
     * complicated. */
    *target = bitmap_findnset(flatfs->buffer_bat, flatfs->totalblocks);
    return *target;
}

int flatfs_alloc_indir(flatfs_t *flatfs, int todbl, uint32_t numblocks, uint32_t offset)
{
    uint32_t i;

    /* Are we writing to the single indir or to a double indir? */
    uint32_t *ind = &(flatfs->buffer_inode->block_indir);
    if (todbl >= 0)
        ind = &(flatfs->buffer_di[todbl]);

    /* Get the indirection block from the bitmap */
    if (*ind == 0 && flatfs_bm_alloc(flatfs, ind) == -1)
        return VFS_ERROR;
    /* If we just allocated the block it doesn't matter if it is zeroed or not
     * because we will be overwriting the entries anyway! */
    if (flatfs_disk_action(flatfs->disk, *ind, (uint32_t)flatfs->buffer_si, 0) == 0)
        return VFS_ERROR;

    /* Get blocks for the data and zero them. */
    for (i = offset; i < offset+numblocks; ++i)
    {
        if (flatfs_bm_alloc(flatfs, &(flatfs->buffer_si[i])) == -1)
            return VFS_ERROR;
        if (flatfs_disk_action(flatfs->disk, flatfs->buffer_si[i],
                    (uint32_t)zeroblock, 1) == 0)
            return VFS_ERROR;
    }

    /* Set extra spots to 0. */
    for (i = offset+numblocks; i < FLATFS_BLOCKS_SINDIR; ++i)
        flatfs->buffer_si[i] = 0;

    /* Store the indirection block on the disk. */
    if (flatfs_disk_action(flatfs->disk, *ind, (uint32_t)flatfs->buffer_si, 1) == 0)
        return VFS_ERROR;

    return VFS_OK;
}

/* Allocate numblocks starting at offset. */
int flatfs_alloc_blocks(flatfs_t *flatfs, uint32_t numblocks, uint32_t offset)
{
    uint32_t i, tmpblocks;
    uint32_t start, towrite;
    int r;

    /* Do we need to allocate direct blocks? */
    if (offset < FLATFS_BLOCKS_DIRECT)
    {
        tmpblocks = MIN(FLATFS_BLOCKS_DIRECT - offset, numblocks);
        numblocks -= tmpblocks;
        for(i=offset; i<offset+tmpblocks; ++i) {
            if (flatfs_bm_alloc(flatfs, &(flatfs->buffer_inode->block[i])) == -1)
                return VFS_ERROR;
            /* Zero the block right away. It doesn't matter if we fail because the
             * block is not used. As long as we don't write the bitmap we're good! */
            if (flatfs_disk_action(flatfs->disk, flatfs->buffer_inode->block[i],
                        (uint32_t)zeroblock, 1) == 0)
                return VFS_ERROR;
        }
        for (i = tmpblocks + offset; i < FLATFS_BLOCKS_DIRECT; ++i)
            flatfs->buffer_inode->block[i] = 0;
        /* New offset. */
        offset = FLATFS_BLOCKS_DIRECT;
    }

    if (numblocks > 0 && offset < FLATFS_BLOCKS_DIRECT + FLATFS_BLOCKS_SINDIR)
    {
        /* Allocate single indir blocks */
        tmpblocks = MIN(numblocks, FLATFS_BLOCKS_SINDIR + FLATFS_BLOCKS_DIRECT - offset);
        numblocks -= tmpblocks;

        if (flatfs_alloc_indir(flatfs, -1, tmpblocks,
                    offset - FLATFS_BLOCKS_DIRECT) == VFS_ERROR)
            return VFS_ERROR;
        offset = FLATFS_BLOCKS_DIRECT + FLATFS_BLOCKS_SINDIR;
    }

    if (numblocks > 0)
    {
        /* We assume flatfs->buffer_inode->block_dbl_indir is the double-indir block. */
        r = flatfs_disk_action(flatfs->disk, flatfs->buffer_inode->block_dbl_indir,
                (uint32_t)flatfs->buffer_di, 0);
        if (r == 0)
            return VFS_ERROR;

        /* Make sure we work with the correct offset. */
        offset -= FLATFS_BLOCKS_DIRECT + FLATFS_BLOCKS_SINDIR;

        start = offset / FLATFS_BLOCKS_SINDIR;
        offset = offset % FLATFS_BLOCKS_SINDIR;

        for (i=start; numblocks > 0; ++i)
        {
            towrite = MIN(numblocks, FLATFS_BLOCKS_SINDIR - offset);
            if (flatfs_alloc_indir(flatfs, i, towrite, offset) == VFS_ERROR)
                return VFS_ERROR;
            numblocks -= towrite;
            /* Only the first write can have an offset. */
            offset = 0;
        }
        if (flatfs_disk_action(flatfs->disk, flatfs->buffer_inode->block_dbl_indir,
                    (uint32_t)flatfs->buffer_di, 1) == 0)
            return VFS_ERROR;
    }

    /* Store the BAT */
    r = flatfs_disk_action(flatfs->disk, FLATFS_ALLOCATION_BLOCK,
            (uint32_t)flatfs->buffer_bat, 1);
    if(r==0)
        return VFS_ERROR;

    return VFS_OK;
}

int flatfs_remove_indir(flatfs_t *flatfs, int todbl)
{
    uint32_t i;

    uint32_t *ind = &(flatfs->buffer_inode->block_indir);
    if (todbl >= 0)
        ind = &(flatfs->buffer_di[todbl]);

    if (flatfs_disk_action(flatfs->disk, *ind, (uint32_t)flatfs->buffer_si, 0) == 0)
        return VFS_ERROR;

    /* Simply go through the data blocks and unset them in the bitmap. */
    for (i = 0; i < FLATFS_BLOCKS_SINDIR; ++i)
        if (flatfs->buffer_si[i] > 0)
            bitmap_set(flatfs->buffer_bat, flatfs->buffer_si[i], 0);

    return VFS_OK;
}

uint32_t flatfs_getblock(flatfs_t *flatfs, uint32_t block, int indirbuf)
{
    /* direct block? */
    uint32_t indir;
    if (block < FLATFS_BLOCKS_DIRECT)
        return flatfs->buffer_inode->block[block];

    /* single indirect block? */
    block -= FLATFS_BLOCKS_DIRECT;

    if (block < FLATFS_BLOCKS_SINDIR)
    {
        /* Check if we need to read in the single-indir buffer. */
        if (indirbuf == 0 && flatfs_disk_action(flatfs->disk,
                    flatfs->buffer_inode->block_indir, (uint32_t)flatfs->buffer_si, 0) == 0)
            return VFS_ERROR;
        return flatfs->buffer_si[block];
    }

    /* If we get here it's a double indirect block. so find the correct
     * one. We assume that the dbl-indir buffer is already loaded. */
    block -= FLATFS_BLOCKS_SINDIR;
    indir = block / FLATFS_BLOCKS_SINDIR;
    block = block % FLATFS_BLOCKS_SINDIR;

    if (indirbuf == 0 && flatfs_disk_action(flatfs->disk, flatfs->buffer_di[indir],
                (uint32_t)flatfs->buffer_si, 0) == 0)
        return VFS_ERROR;
    return flatfs->buffer_si[block];
}

/** @} */
