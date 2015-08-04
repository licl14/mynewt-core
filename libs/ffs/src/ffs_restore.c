#include <assert.h>
#include <string.h>
#include "hal/hal_flash.h"
#include "os/os_mempool.h"
#include "os/os_malloc.h"
#include "ffs/ffs.h"
#include "ffs_priv.h"

static int
ffs_delete_if_trash(struct ffs_object *object)
{
    struct ffs_inode *inode;
    struct ffs_block *block;

    switch (object->fo_type) {
    case FFS_OBJECT_TYPE_INODE:
        inode = (void *)object;
        if (inode->fi_flags & FFS_INODE_F_DELETED) {
            ffs_inode_delete_from_ram(inode);
            return 1;
        } else if (inode->fi_flags & FFS_INODE_F_DUMMY) {
            /* This inode is referenced by other objects, but it does not exist
             * in the file system.  This indicates file system corruption.
             * Just delete the inode and everything that references it.
             */
            ffs_inode_delete_from_ram(inode);
            return 1;
        } else {
            return 0;
        }

    case FFS_OBJECT_TYPE_BLOCK:
        block = (void *)object;
        if (block->fb_flags & FFS_BLOCK_F_DELETED || block->fb_inode == NULL) {
            ffs_block_delete_from_ram(block);
            return 1;
        } else {
            return 0;
        }

    default:
        assert(0);
        return 0;
    }
}

void
ffs_restore_sweep(void)
{
    struct ffs_object_list *list;
    struct ffs_object *object;
    struct ffs_object *next;
    int i;

    for (i = 0; i < FFS_HASH_SIZE; i++) {
        list = ffs_hash + i;

        object = SLIST_FIRST(list);
        while (object != NULL) {
            next = SLIST_NEXT(object, fb_hash_next);
            ffs_delete_if_trash(object);

            object = next;
        }
    }
}

static int
ffs_restore_dummy_inode(struct ffs_inode **out_inode, uint32_t id, int is_dir)
{
    struct ffs_inode *inode;

    inode = ffs_inode_alloc();
    if (inode == NULL) {
        return FFS_ENOMEM;
    }
    inode->fi_object.fo_id = id;
    inode->fi_refcnt = 1;
    inode->fi_object.fo_area_idx = FFS_AREA_ID_NONE;
    inode->fi_flags = FFS_INODE_F_DUMMY;
    if (is_dir) {
        inode->fi_flags |= FFS_INODE_F_DIRECTORY;
    }

    ffs_hash_insert(&inode->fi_object);

    *out_inode = inode;

    return 0;
}

static int
ffs_restore_inode_gets_replaced(int *out_should_replace,
                                const struct ffs_inode *old_inode,
                                const struct ffs_disk_inode *disk_inode)
{
    assert(old_inode->fi_object.fo_id == disk_inode->fdi_id);

    if (old_inode->fi_flags & FFS_INODE_F_DUMMY) {
        *out_should_replace = 1;
        return 0;
    }

    if (old_inode->fi_object.fo_seq < disk_inode->fdi_seq) {
        *out_should_replace = 1;
        return 0;
    }

    if (old_inode->fi_object.fo_seq == disk_inode->fdi_seq) {
        /* This is a duplicate of an previously-read inode.  This should never
         * happen.
         */
        return FFS_ECORRUPT;
    }

    *out_should_replace = 0;
    return 0;
}

/**
 * Determines if the speciifed inode should be added to the RAM representation
 * and adds it if appropriate.
 *
 * @param disk_inode            The inode just read from flash.
 * @param area_idx              The index of the area containing the inode.
 * @param area_offset           The offset within the area of the inode.
 *
 * @return                      0 on success; nonzero on failure.
 */
static int
ffs_restore_inode(const struct ffs_disk_inode *disk_inode, uint16_t area_idx,
                  uint32_t area_offset)
{
    struct ffs_inode *parent;
    struct ffs_inode *inode;
    int new_inode;
    int do_add;
    int rc;

    new_inode = 0;

    rc = ffs_hash_find_inode(&inode, disk_inode->fdi_id);
    switch (rc) {
    case 0:
        rc = ffs_restore_inode_gets_replaced(&do_add, inode, disk_inode);
        if (rc != 0) {
            goto err;
        }

        if (do_add) {
            if (inode->fi_parent != NULL) {
                ffs_inode_remove_child(inode);
            }
            ffs_inode_from_disk(inode, disk_inode, area_idx, area_offset);
        }
        break;

    case FFS_ENOENT:
        inode = ffs_inode_alloc();
        if (inode == NULL) {
            rc = FFS_ENOMEM;
            goto err;
        }
        new_inode = 1;
        do_add = 1;

        rc = ffs_inode_from_disk(inode, disk_inode, area_idx, area_offset);
        if (rc != 0) {
            goto err;
        }
        inode->fi_refcnt = 1;

        ffs_hash_insert(&inode->fi_object);
        break;

    default:
        rc = FFS_ECORRUPT;
        goto err;
    }

    if (do_add) {
        if (disk_inode->fdi_parent_id != FFS_ID_NONE) {
            rc = ffs_hash_find_inode(&parent, disk_inode->fdi_parent_id);
            if (rc == FFS_ENOENT) {
                rc = ffs_restore_dummy_inode(&parent,
                                             disk_inode->fdi_parent_id, 1);
            }
            if (rc != 0) {
                goto err;
            }

            rc = ffs_inode_add_child(parent, inode);
            if (rc != 0) {
                goto err;
            }
        } 


        if (ffs_inode_is_root(disk_inode)) {
            ffs_root_dir = inode;
        }
    }

    if (inode->fi_object.fo_id >= ffs_next_id) {
        ffs_next_id = inode->fi_object.fo_id + 1;
    }

    return 0;

err:
    if (new_inode) {
        ffs_inode_free(inode);
    }
    return rc;
}

/**
 * Indicates whether the specified data block is superseded by the just-read
 * disk data block.  A data block supersedes another if its ID is equal and its
 * sequence number is greater than that of the other block.
 *
 * @param out_should_replace    On success, 0 or 1 gets written here, to
 *                                  indicate whether replacement should occur.
 * @param old_block             The data block which has already been read and
 *                                  converted to its RAM representation.  This
 *                                  is the block that may be superseded.
 * @param disk_block            The disk data block that was just read from
 *                                  flash.  This is the block which may
 *                                  supersede the other.
 *
 * @return                      0 on success; nonzero on failure.
 */
static int
ffs_restore_block_gets_replaced(int *out_should_replace,
                                const struct ffs_block *old_block,
                                const struct ffs_disk_block *disk_block)
{
    assert(old_block->fb_object.fo_id == disk_block->fdb_id);

    if (old_block->fb_flags & FFS_BLOCK_F_DUMMY) {
        *out_should_replace = 1;
        return 0;
    }

    if (old_block->fb_object.fo_seq < disk_block->fdb_seq) {
        *out_should_replace = 1;
        return 0;
    }

    if (old_block->fb_object.fo_seq == disk_block->fdb_seq) {
        /* This is a duplicate of an previously-read inode.  This should never
         * happen.
         */
        return FFS_ECORRUPT;
    }

    *out_should_replace = 0;
    return 0;
}

/**
 * Populates the ffs RAM state with the memory representation of the specified
 * disk data block.
 *
 * @param disk_block            The source disk block to convert.
 * @param area_idx              The ID of the area containing the block.
 * @param area_offset           The area_offset within the area of the block.
 *
 * @return                      0 on success; nonzero on failure.
 */
static int
ffs_restore_block(const struct ffs_disk_block *disk_block, uint16_t area_idx,
                  uint32_t area_offset)
{
    struct ffs_block *block;
    int do_replace;
    int new_block;
    int rc;

    new_block = 0;

    rc = ffs_hash_find_block(&block, disk_block->fdb_id);
    switch (rc) {
    case 0:
        rc = ffs_restore_block_gets_replaced(&do_replace, block, disk_block);
        if (rc != 0) {
            goto err;
        }

        if (do_replace) {
            ffs_block_from_disk(block, disk_block, area_idx, area_offset);
        }
        break;

    case FFS_ENOENT:
        block = ffs_block_alloc();
        if (block == NULL) {
            rc = FFS_ENOMEM;
            goto err;
        }
        new_block = 1;

        ffs_block_from_disk(block, disk_block, area_idx, area_offset);
        ffs_hash_insert(&block->fb_object);

        rc = ffs_hash_find_inode(&block->fb_inode, disk_block->fdb_inode_id);
        if (rc == FFS_ENOENT) {
            rc = ffs_restore_dummy_inode(&block->fb_inode,
                                         disk_block->fdb_inode_id, 0);
        }
        if (rc != 0) {
            goto err;
        }
        ffs_inode_insert_block(block->fb_inode, block);
        break;

    default:
        rc = FFS_ECORRUPT;
        goto err;
    }

    if (block->fb_object.fo_id >= ffs_next_id) {
        ffs_next_id = block->fb_object.fo_id + 1;
    }

    return 0;

err:
    if (new_block) {
        ffs_block_free(block);
    }
    return rc;
}

/**
 * Populates the ffs RAM state with the memory representation of the specified
 * disk object.
 *
 * @param disk_object           The source disk object to convert.
 *
 * @return                      0 on success; nonzero on failure.
 */
static int
ffs_restore_object(const struct ffs_disk_object *disk_object)
{
    int rc;

    switch (disk_object->fdo_type) {
    case FFS_OBJECT_TYPE_INODE:
        rc = ffs_restore_inode(&disk_object->fdo_disk_inode,
                               disk_object->fdo_area_idx,
                               disk_object->fdo_offset);
        break;

    case FFS_OBJECT_TYPE_BLOCK:
        rc = ffs_restore_block(&disk_object->fdo_disk_block,
                               disk_object->fdo_area_idx,
                               disk_object->fdo_offset);
        break;

    default:
        assert(0);
        rc = FFS_EINVAL;
        break;
    }

    return rc;
}

/**
 * Reads a single disk object from flash.
 *
 * @param out_disk_object       On success, the restored object gets written
 *                                  here.
 * @param area_idx              The area to read the object from.
 * @param area_offset           The offset within the area to read from.
 *
 * @return                      0 on success; nonzero on failure.
 */
static int
ffs_restore_disk_object(struct ffs_disk_object *out_disk_object,
                        int area_idx, uint32_t area_offset)
{
    uint32_t magic;
    int rc;

    rc = ffs_flash_read(area_idx, area_offset, &magic, sizeof magic);
    if (rc != 0) {
        return rc;
    }

    switch (magic) {
    case FFS_INODE_MAGIC:
        out_disk_object->fdo_type = FFS_OBJECT_TYPE_INODE;
        rc = ffs_inode_read_disk(&out_disk_object->fdo_disk_inode, NULL,
                                 area_idx, area_offset);
        break;

    case FFS_BLOCK_MAGIC:
        out_disk_object->fdo_type = FFS_OBJECT_TYPE_BLOCK;
        rc = ffs_block_read_disk(&out_disk_object->fdo_disk_block, area_idx,
                                 area_offset);
        break;

    case 0xffffffff:
        rc = FFS_EEMPTY;
        break;

    default:
        rc = FFS_ECORRUPT;
        break;
    }

    if (rc != 0) {
        return rc;
    }

    out_disk_object->fdo_area_idx = area_idx;
    out_disk_object->fdo_offset = area_offset;

    return 0;
}

/**
 * Calculates the disk space occupied by the specified disk object.
 *
 * @param disk_object
 */
static int
ffs_restore_disk_object_size(const struct ffs_disk_object *disk_object)
{
    switch (disk_object->fdo_type) {
    case FFS_OBJECT_TYPE_INODE:
        return sizeof disk_object->fdo_disk_inode +
                      disk_object->fdo_disk_inode.fdi_filename_len;

    case FFS_OBJECT_TYPE_BLOCK:
        return sizeof disk_object->fdo_disk_block +
                      disk_object->fdo_disk_block.fdb_data_len;

    default:
        assert(0);
        return 1;
    }
}

/**
 * Reads the specified area from disk and loads its contents into the RAM
 * representation.
 *
 * @param area_idx              The index of the area to read.
 *
 * @return                      0 on success; nonzero on failure.
 */
static int
ffs_restore_area_contents(int area_idx)
{
    struct ffs_area *area;
    struct ffs_disk_area disk_area;
    struct ffs_disk_object disk_object;
    int rc;

    area = ffs_areas + area_idx;

    area->fa_cur = sizeof disk_area;
    while (1) {
        rc = ffs_restore_disk_object(&disk_object, area_idx, area->fa_cur);
        switch (rc) {
        case 0:
            ffs_restore_object(&disk_object);
            area->fa_cur += ffs_restore_disk_object_size(&disk_object);
            break;

        case FFS_EEMPTY:
        case FFS_ERANGE:
            return 0;

        default:
            return rc;
        }
    }
}

/**
 * Reads and parses one area header.  This function does not read the area's
 * contents.
 *
 * @param out_is_scratch        On success, 0 or 1 gets written here,
 *                                  indicating whether the area is a scratch
 *                                  area.
 * @param area_offset           The flash offset of the start of the area.
 *
 * @return                      0 on success;
 *                              nonzero on failure.
 */
static int
ffs_restore_detect_one_area(struct ffs_disk_area *out_disk_area,
                            uint32_t area_offset)
{
    int rc;

    rc = flash_read(area_offset, out_disk_area, sizeof *out_disk_area);
    if (rc != 0) {
        return FFS_EFLASH_ERROR;
    }

    if (!ffs_area_magic_is_set(out_disk_area)) {
        return FFS_ECORRUPT;
    }

    return 0;
}

static int
ffs_restore_corrupt_flash(void)
{
    struct ffs_object *object;
    uint16_t good_idx;
    uint16_t bad_idx;
    int rc;
    int i;

    rc = ffs_area_find_corrupt_scratch(&good_idx, &bad_idx);
    if (rc != 0) {
        return rc;
    }

    FFS_HASH_FOREACH(object, i) {
        if (object->fo_area_idx == bad_idx) {
            switch (object->fo_type) {
            case FFS_OBJECT_TYPE_INODE:
                ((struct ffs_inode *) object)->fi_flags |= FFS_INODE_F_DUMMY;
                break;

            case FFS_OBJECT_TYPE_BLOCK:
                ((struct ffs_block *) object)->fb_flags |= FFS_BLOCK_F_DUMMY;
                break;

            default:
                assert(0);
                return FFS_ECORRUPT;
            }
        }
    }

    rc = ffs_restore_area_contents(good_idx);
    if (rc != 0) {
        return rc;
    }

    rc = ffs_format_area(bad_idx, 1);
    if (rc != 0) {
        return rc;
    }

    ffs_scratch_area_idx = bad_idx;

    return 0;
}

/**
 * Searches for a valid ffs file system among the specified areas.  This
 * function succeeds if a file system is detected among any subset of the
 * supplied areas.  If the area set does not contain a valid file system,
 * a new one can be created via a call to ffs_format().
 *
 * @param area_descs        The area set to search.  This array must be
 *                              terminated with a 0-length area.
 *
 * @return                  0 on success;
 *                          FFS_ECORRUPT if no valid file system was detected;
 *                          other nonzero on error.
 */
int
ffs_restore_full(const struct ffs_area_desc *area_descs)
{
    struct ffs_disk_area disk_area;
    int cur_area_idx;
    int use_area;
    int rc;
    int i;

    /* Start from a clean state. */
    ffs_misc_reset();

    /* Read each area from flash. */
    for (i = 0; area_descs[i].fad_length != 0; i++) {
        rc = ffs_restore_detect_one_area(&disk_area, area_descs[i].fad_offset);
        switch (rc) {
        case 0:
            use_area = 1;
            break;

        case FFS_ECORRUPT:
            use_area = 0;
            break;

        default:
            goto err;
        }

        if (use_area) {
            if (disk_area.fda_id == FFS_AREA_ID_NONE &&
                ffs_scratch_area_idx != FFS_AREA_ID_NONE) {

                /* Don't allow more than one scratch area. */
                use_area = 0;
            }
        }

        if (use_area) {
            /* Populate RAM with a representation of this area. */
            cur_area_idx = ffs_num_areas;

            rc = ffs_misc_set_num_areas(ffs_num_areas + 1);
            if (rc != 0) {
                goto err;
            }

            ffs_areas[cur_area_idx].fa_offset = area_descs[i].fad_offset;
            ffs_areas[cur_area_idx].fa_length = area_descs[i].fad_length;
            ffs_areas[cur_area_idx].fa_cur = sizeof (struct ffs_disk_area);
            ffs_areas[cur_area_idx].fa_gc_seq = disk_area.fda_gc_seq;
            ffs_areas[cur_area_idx].fa_id = disk_area.fda_id;

            if (disk_area.fda_id == FFS_AREA_ID_NONE) {
                ffs_scratch_area_idx = cur_area_idx;
            } else {
                ffs_restore_area_contents(cur_area_idx);
            }
        }
    }

    /* All areas have been restored from flash. */

    if (ffs_scratch_area_idx == FFS_AREA_ID_NONE) {
        /* No scratch area.  The system may have been rebooted in the middle of
         * a garbage collection cycle.  Look for a candidate scratch area.
         */
        rc = ffs_restore_corrupt_flash();
        if (rc != 0) {
            goto err;
        }
    }

    /* Ensure this file system contains a valid scratch area. */
    rc = ffs_misc_validate_scratch();
    if (rc != 0) {
        goto err;
    }

    /* Delete from RAM any objects that were invalidated when subsequent areas
     * were restored.
     */
    ffs_restore_sweep();

    /* Make sure the file system contains a valid root directory. */
    rc = ffs_misc_validate_root();
    if (rc != 0) {
        goto err;
    }

    /* Set the maximum data block size according to the size of the smallest
     * area.
     */
    ffs_misc_set_max_block_data_size();

    return 0;

err:
    ffs_misc_reset();
    return rc;
}

