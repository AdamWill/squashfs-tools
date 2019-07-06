/*
 * Unsquash a squashfs filesystem.  This is a highly compressed read only
 * filesystem.
 *
 * Copyright (c) 2009, 2010, 2011, 2012, 2013, 2019
 * Phillip Lougher <phillip@squashfs.org.uk>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2,
 * or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * unsquash-4.c
 */

#include "unsquashfs.h"
#include "squashfs_swap.h"
#include "xattr.h"

static struct squashfs_fragment_entry *fragment_table;
static unsigned int *id_table;

static int read_fragment_table(long long *directory_table_end)
{
	int res, i;
	int bytes = SQUASHFS_FRAGMENT_BYTES(sBlk.s.fragments);
	int  indexes = SQUASHFS_FRAGMENT_INDEXES(sBlk.s.fragments);
	long long fragment_table_index[indexes];

	TRACE("read_fragment_table: %d fragments, reading %d fragment indexes "
		"from 0x%llx\n", sBlk.s.fragments, indexes,
		sBlk.s.fragment_table_start);

	if(sBlk.s.fragments == 0) {
		*directory_table_end = sBlk.s.fragment_table_start;
		return TRUE;
	}

	fragment_table = malloc(bytes);
	if(fragment_table == NULL)
		EXIT_UNSQUASH("read_fragment_table: failed to allocate "
			"fragment table\n");

	res = read_fs_bytes(fd, sBlk.s.fragment_table_start,
		SQUASHFS_FRAGMENT_INDEX_BYTES(sBlk.s.fragments),
		fragment_table_index);
	if(res == FALSE) {
		ERROR("read_fragment_table: failed to read fragment table "
			"index\n");
		return FALSE;
	}
	SQUASHFS_INSWAP_FRAGMENT_INDEXES(fragment_table_index, indexes);

	for(i = 0; i < indexes; i++) {
		int expected = (i + 1) != indexes ? SQUASHFS_METADATA_SIZE :
					bytes & (SQUASHFS_METADATA_SIZE - 1);
		int length = read_block(fd, fragment_table_index[i], NULL,
			expected, ((char *) fragment_table) + (i *
			SQUASHFS_METADATA_SIZE));
		TRACE("Read fragment table block %d, from 0x%llx, length %d\n",
			i, fragment_table_index[i], length);
		if(length == FALSE) {
			ERROR("read_fragment_table: failed to read fragment "
				"table index\n");
			return FALSE;
		}
	}

	for(i = 0; i < sBlk.s.fragments; i++) 
		SQUASHFS_INSWAP_FRAGMENT_ENTRY(&fragment_table[i]);

	*directory_table_end = fragment_table_index[0];
	return TRUE;
}


void read_fragment_4(unsigned int fragment, long long *start_block, int *size)
{
	TRACE("read_fragment: reading fragment %d\n", fragment);

	struct squashfs_fragment_entry *fragment_entry;

	fragment_entry = &fragment_table[fragment];
	*start_block = fragment_entry->start_block;
	*size = fragment_entry->size;
}


struct inode *read_inode_4(unsigned int start_block, unsigned int offset)
{
	static union squashfs_inode_header header;
	long long start = sBlk.s.inode_table_start + start_block;
	int bytes = lookup_entry(inode_table_hash, start);
	char *block_ptr = inode_table + bytes + offset;
	static struct inode i;

	TRACE("read_inode: reading inode [%d:%d]\n", start_block,  offset);

	if(bytes == -1)
		EXIT_UNSQUASH("read_inode: inode table block %lld not found\n",
			start); 		

	SQUASHFS_SWAP_BASE_INODE_HEADER(block_ptr, &header.base);

	i.uid = (uid_t) id_table[header.base.uid];
	i.gid = (uid_t) id_table[header.base.guid];
	i.mode = lookup_type[header.base.inode_type] | header.base.mode;
	i.type = header.base.inode_type;
	i.time = header.base.mtime;
	i.inode_number = header.base.inode_number;

	switch(header.base.inode_type) {
		case SQUASHFS_DIR_TYPE: {
			struct squashfs_dir_inode_header *inode = &header.dir;

			SQUASHFS_SWAP_DIR_INODE_HEADER(block_ptr, inode);

			i.data = inode->file_size;
			i.offset = inode->offset;
			i.start = inode->start_block;
			i.xattr = SQUASHFS_INVALID_XATTR;
			break;
		}
		case SQUASHFS_LDIR_TYPE: {
			struct squashfs_ldir_inode_header *inode = &header.ldir;

			SQUASHFS_SWAP_LDIR_INODE_HEADER(block_ptr, inode);

			i.data = inode->file_size;
			i.offset = inode->offset;
			i.start = inode->start_block;
			i.xattr = inode->xattr;
			break;
		}
		case SQUASHFS_FILE_TYPE: {
			struct squashfs_reg_inode_header *inode = &header.reg;

			SQUASHFS_SWAP_REG_INODE_HEADER(block_ptr, inode);

			i.data = inode->file_size;
			i.frag_bytes = inode->fragment == SQUASHFS_INVALID_FRAG
				?  0 : inode->file_size % sBlk.s.block_size;
			i.fragment = inode->fragment;
			i.offset = inode->offset;
			i.blocks = inode->fragment == SQUASHFS_INVALID_FRAG ?
				(i.data + sBlk.s.block_size - 1) >>
				sBlk.s.block_log :
				i.data >> sBlk.s.block_log;
			i.start = inode->start_block;
			i.sparse = 0;
			i.block_ptr = block_ptr + sizeof(*inode);
			i.xattr = SQUASHFS_INVALID_XATTR;
			break;
		}	
		case SQUASHFS_LREG_TYPE: {
			struct squashfs_lreg_inode_header *inode = &header.lreg;

			SQUASHFS_SWAP_LREG_INODE_HEADER(block_ptr, inode);

			i.data = inode->file_size;
			i.frag_bytes = inode->fragment == SQUASHFS_INVALID_FRAG
				?  0 : inode->file_size % sBlk.s.block_size;
			i.fragment = inode->fragment;
			i.offset = inode->offset;
			i.blocks = inode->fragment == SQUASHFS_INVALID_FRAG ?
				(inode->file_size + sBlk.s.block_size - 1) >>
				sBlk.s.block_log :
				inode->file_size >> sBlk.s.block_log;
			i.start = inode->start_block;
			i.sparse = inode->sparse != 0;
			i.block_ptr = block_ptr + sizeof(*inode);
			i.xattr = inode->xattr;
			break;
		}	
		case SQUASHFS_SYMLINK_TYPE:
		case SQUASHFS_LSYMLINK_TYPE: {
			struct squashfs_symlink_inode_header *inode = &header.symlink;

			SQUASHFS_SWAP_SYMLINK_INODE_HEADER(block_ptr, inode);

			i.symlink = malloc(inode->symlink_size + 1);
			if(i.symlink == NULL)
				EXIT_UNSQUASH("read_inode: failed to malloc "
					"symlink data\n");
			strncpy(i.symlink, block_ptr +
				sizeof(struct squashfs_symlink_inode_header),
				inode->symlink_size);
			i.symlink[inode->symlink_size] = '\0';
			i.data = inode->symlink_size;

			if(header.base.inode_type == SQUASHFS_LSYMLINK_TYPE)
				SQUASHFS_SWAP_INTS(block_ptr +
					sizeof(struct squashfs_symlink_inode_header) +
					inode->symlink_size, &i.xattr, 1);
			else
				i.xattr = SQUASHFS_INVALID_XATTR;
			break;
		}
 		case SQUASHFS_BLKDEV_TYPE:
	 	case SQUASHFS_CHRDEV_TYPE: {
			struct squashfs_dev_inode_header *inode = &header.dev;

			SQUASHFS_SWAP_DEV_INODE_HEADER(block_ptr, inode);

			i.data = inode->rdev;
			i.xattr = SQUASHFS_INVALID_XATTR;
			break;
		}
 		case SQUASHFS_LBLKDEV_TYPE:
	 	case SQUASHFS_LCHRDEV_TYPE: {
			struct squashfs_ldev_inode_header *inode = &header.ldev;

			SQUASHFS_SWAP_LDEV_INODE_HEADER(block_ptr, inode);

			i.data = inode->rdev;
			i.xattr = inode->xattr;
			break;
		}
		case SQUASHFS_FIFO_TYPE:
		case SQUASHFS_SOCKET_TYPE:
			i.data = 0;
			i.xattr = SQUASHFS_INVALID_XATTR;
			break;
		case SQUASHFS_LFIFO_TYPE:
		case SQUASHFS_LSOCKET_TYPE: {
			struct squashfs_lipc_inode_header *inode = &header.lipc;

			SQUASHFS_SWAP_LIPC_INODE_HEADER(block_ptr, inode);

			i.data = 0;
			i.xattr = inode->xattr;
			break;
		}
		default:
			EXIT_UNSQUASH("Unknown inode type %d in read_inode!\n",
				header.base.inode_type);
	}
	return &i;
}


struct dir *squashfs_opendir_4(unsigned int block_start, unsigned int offset,
	struct inode **i)
{
	struct squashfs_dir_header dirh;
	char buffer[sizeof(struct squashfs_dir_entry) + SQUASHFS_NAME_LEN + 1]
		__attribute__((aligned));
	struct squashfs_dir_entry *dire = (struct squashfs_dir_entry *) buffer;
	long long start;
	int bytes;
	int dir_count, size;
	struct dir_ent *new_dir;
	struct dir *dir;

	TRACE("squashfs_opendir: inode start block %d, offset %d\n",
		block_start, offset);

	*i = s_ops.read_inode(block_start, offset);

	dir = malloc(sizeof(struct dir));
	if(dir == NULL)
		EXIT_UNSQUASH("squashfs_opendir: malloc failed!\n");

	dir->dir_count = 0;
	dir->cur_entry = 0;
	dir->mode = (*i)->mode;
	dir->uid = (*i)->uid;
	dir->guid = (*i)->gid;
	dir->mtime = (*i)->time;
	dir->xattr = (*i)->xattr;
	dir->dirs = NULL;

	if ((*i)->data == 3)
		/*
		 * if the directory is empty, skip the unnecessary
		 * lookup_entry, this fixes the corner case with
		 * completely empty filesystems where lookup_entry correctly
		 * returning -1 is incorrectly treated as an error
		 */
		return dir;

	start = sBlk.s.directory_table_start + (*i)->start;
	bytes = lookup_entry(directory_table_hash, start);

	if(bytes == -1)
		EXIT_UNSQUASH("squashfs_opendir: directory block %d not "
			"found!\n", block_start);

	bytes += (*i)->offset;
	size = (*i)->data + bytes - 3;

	while(bytes < size) {			
		SQUASHFS_SWAP_DIR_HEADER(directory_table + bytes, &dirh);
	
		dir_count = dirh.count + 1;
		TRACE("squashfs_opendir: Read directory header @ byte position "
			"%d, %d directory entries\n", bytes, dir_count);
		bytes += sizeof(dirh);

		/* dir_count should never be larger than SQUASHFS_DIR_COUNT */
		if(dir_count > SQUASHFS_DIR_COUNT) {
			ERROR("File system corrupted: too many entries in directory\n");
			goto corrupted;
		}

		while(dir_count--) {
			SQUASHFS_SWAP_DIR_ENTRY(directory_table + bytes, dire);

			bytes += sizeof(*dire);

			/* size should never be SQUASHFS_NAME_LEN or larger */
			if(dire->size >= SQUASHFS_NAME_LEN) {
				ERROR("File system corrupted: filename too long\n");
				goto corrupted;
			}

			memcpy(dire->name, directory_table + bytes,
				dire->size + 1);
			dire->name[dire->size + 1] = '\0';
			TRACE("squashfs_opendir: directory entry %s, inode "
				"%d:%d, type %d\n", dire->name,
				dirh.start_block, dire->offset, dire->type);
			if((dir->dir_count % DIR_ENT_SIZE) == 0) {
				new_dir = realloc(dir->dirs, (dir->dir_count +
					DIR_ENT_SIZE) * sizeof(struct dir_ent));
				if(new_dir == NULL)
					EXIT_UNSQUASH("squashfs_opendir: "
						"realloc failed!\n");
				dir->dirs = new_dir;
			}
			strcpy(dir->dirs[dir->dir_count].name, dire->name);
			dir->dirs[dir->dir_count].start_block =
				dirh.start_block;
			dir->dirs[dir->dir_count].offset = dire->offset;
			dir->dirs[dir->dir_count].type = dire->type;
			dir->dir_count ++;
			bytes += dire->size + 1;
		}
	}

	return dir;

corrupted:
	free(dir->dirs);
	free(dir);
	return NULL;
}


static int read_uids_guids(long long *table_start)
{
	int res, i;
	int bytes = SQUASHFS_ID_BYTES(sBlk.s.no_ids);
	int indexes = SQUASHFS_ID_BLOCKS(sBlk.s.no_ids);
	long long id_index_table[indexes];

	TRACE("read_uids_guids: no_ids %d\n", sBlk.s.no_ids);

	id_table = malloc(bytes);
	if(id_table == NULL) {
		ERROR("read_uids_guids: failed to allocate id table\n");
		return FALSE;
	}

	res = read_fs_bytes(fd, sBlk.s.id_table_start,
		SQUASHFS_ID_BLOCK_BYTES(sBlk.s.no_ids), id_index_table);
	if(res == FALSE) {
		ERROR("read_uids_guids: failed to read id index table\n");
		return FALSE;
	}
	SQUASHFS_INSWAP_ID_BLOCKS(id_index_table, indexes);

	/*
	 * id_index_table[0] stores the start of the compressed id blocks.
	 * This by definition is also the end of the previous filesystem
	 * table - this may be the exports table if it is present, or the
	 * fragments table if it isn't.
	 */
	*table_start = id_index_table[0];

	for(i = 0; i < indexes; i++) {
		int expected = (i + 1) != indexes ? SQUASHFS_METADATA_SIZE :
					bytes & (SQUASHFS_METADATA_SIZE - 1);
		res = read_block(fd, id_index_table[i], NULL, expected,
			((char *) id_table) + i * SQUASHFS_METADATA_SIZE);
		if(res == FALSE) {
			ERROR("read_uids_guids: failed to read id table block"
				"\n");
			return FALSE;
		}
	}

	SQUASHFS_INSWAP_INTS(id_table, sBlk.s.no_ids);

	return TRUE;
}


static int parse_exports_table(long long *table_start)
{
	int res;
	int indexes = SQUASHFS_LOOKUP_BLOCKS(sBlk.s.inodes);
	long long export_index_table[indexes];

	res = read_fs_bytes(fd, sBlk.s.lookup_table_start,
		SQUASHFS_LOOKUP_BLOCK_BYTES(sBlk.s.inodes), export_index_table);
	if(res == FALSE) {
		ERROR("parse_exports_table: failed to read export index table\n");
		return FALSE;
	}
	SQUASHFS_INSWAP_LOOKUP_BLOCKS(export_index_table, indexes);

	/*
	 * export_index_table[0] stores the start of the compressed export blocks.
	 * This by definition is also the end of the previous filesystem
	 * table - the fragment table.
	 */
	*table_start = export_index_table[0];

	return TRUE;
}


int read_filesystem_tables_4()
{
	long long directory_table_end, table_start;

	if(read_xattrs_from_disk(fd, &sBlk.s, no_xattrs, &table_start) == 0)
		return FALSE;

	if(read_uids_guids(&table_start) == FALSE)
		return FALSE;

	if(parse_exports_table(&table_start) == FALSE)
		return FALSE;

	if(read_fragment_table(&directory_table_end) == FALSE)
		return FALSE;

	if(read_inode_table(sBlk.s.inode_table_start,
				sBlk.s.directory_table_start) == FALSE)
		return FALSE;

	if(read_directory_table(sBlk.s.directory_table_start,
				directory_table_end) == FALSE)
		return FALSE;

	if(no_xattrs)
		sBlk.s.xattr_id_table_start = SQUASHFS_INVALID_BLK;

	return TRUE;
}
