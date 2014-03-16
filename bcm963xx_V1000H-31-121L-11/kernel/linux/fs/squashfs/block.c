/*
 * Squashfs - a compressed read only filesystem for Linux
 *
 * Copyright (c) 2002, 2003, 2004, 2005, 2006, 2007, 2008
 * Phillip Lougher <phillip@lougher.demon.co.uk>
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
 * Foundation, 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * block.c
 */

/*
 * This file implements the low-level routines to read and decompress
 * datablocks and metadata blocks.
 */

#include <linux/fs.h>
#include <linux/vfs.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/buffer_head.h>
#include <linux/zlib.h>

#include "squashfs_fs.h"
#include "squashfs_fs_sb.h"
#include "squashfs_fs_i.h"
#include "squashfs.h"

#ifdef SQUASHFS_LZMA_ENABLE
extern DEFINE_PER_CPU(struct sqlzma *, sqlzma);
#endif

/*
 * Read the metadata block length, this is stored in the first two
 * bytes of the metadata block.
 */
static struct buffer_head *get_block_length(struct super_block *sb,
			u64 *cur_index, int *offset, int *length)
{
	struct squashfs_sb_info *msblk = sb->s_fs_info;
	struct buffer_head *bh;

	bh = sb_bread(sb, *cur_index);
	if (bh == NULL)
		return NULL;

	if (msblk->devblksize - *offset == 1) {
		*length = (unsigned char) bh->b_data[*offset];
		put_bh(bh);
		bh = sb_bread(sb, ++(*cur_index));
		if (bh == NULL)
			return NULL;
		*length |= (unsigned char) bh->b_data[0] << 8;
		*offset = 1;
	} else {
		*length = (unsigned char) bh->b_data[*offset] |
			(unsigned char) bh->b_data[*offset + 1] << 8;
		*offset += 2;
	}

	return bh;
}


/*
 * Read and decompress a metadata block or datablock.  Length is non-zero
 * if a datablock is being read (the size is stored elsewhere in the
 * filesystem), otherwise the length is obtained from the first two bytes of
 * the metadata block.  A bit in the length field indicates if the block
 * is stored uncompressed in the filesystem (usually because compression
 * generated a larger block - this does occasionally happen with zlib).
 */
int squashfs_read_data(struct super_block *sb, void **buffer, u64 index,
			int length, u64 *next_index, int srclength, int pages)
{
	struct squashfs_sb_info *msblk = sb->s_fs_info;
	struct buffer_head **bh;
	int offset = index & ((1 << msblk->devblksize_log2) - 1);
	u64 cur_index = index >> msblk->devblksize_log2;
	int bytes, compressed, b = 0, k = 0, page = 0, avail;


	bh = kcalloc((msblk->block_size >> msblk->devblksize_log2) + 1,
				sizeof(*bh), GFP_KERNEL);
	if (bh == NULL)
		return -ENOMEM;

	if (length) {
		/*
		 * Datablock.
		 */
		bytes = -offset;
		compressed = SQUASHFS_COMPRESSED_BLOCK(length);
		length = SQUASHFS_COMPRESSED_SIZE_BLOCK(length);
		if (next_index)
			*next_index = index + length;

		TRACE("Block @ 0x%llx, %scompressed size %d, src size %d\n",
			index, compressed ? "" : "un", length, srclength);

		if (length < 0 || length > srclength ||
				(index + length) > msblk->bytes_used)
			goto read_failure;

		for (b = 0; bytes < length; b++, cur_index++) {
			bh[b] = sb_getblk(sb, cur_index);
			if (bh[b] == NULL)
				goto block_release;
			bytes += msblk->devblksize;
		}
		ll_rw_block(READ, b, bh);
	} else {
		/*
		 * Metadata block.
		 */
		if ((index + 2) > msblk->bytes_used)
			goto read_failure;

		bh[0] = get_block_length(sb, &cur_index, &offset, &length);
		if (bh[0] == NULL)
			goto read_failure;
		b = 1;

		bytes = msblk->devblksize - offset;
		compressed = SQUASHFS_COMPRESSED(length);
		length = SQUASHFS_COMPRESSED_SIZE(length);
		if (next_index)
			*next_index = index + length + 2;

		TRACE("Block @ 0x%llx, %scompressed size %d\n", index,
				compressed ? "" : "un", length);

		if (length < 0 || length > srclength ||
					(index + length) > msblk->bytes_used)
			goto block_release;

		for (; bytes < length; b++) {
			bh[b] = sb_getblk(sb, ++cur_index);
			if (bh[b] == NULL)
				goto block_release;
			bytes += msblk->devblksize;
		}
		ll_rw_block(READ, b - 1, bh + 1);
	}

	if (compressed) {
		
#ifdef SQUASHFS_LZMA_ENABLE
		int zlib_err = Z_STREAM_END;
		int start, rest;
		enum {Src, Dst};
		struct sized_buf sbuf[2];
		struct sqlzma *percpu;
#ifdef AEI_VDSL_CUSTOMER_NCS
		int offset_save, avail_save, bytes_save;
		#define MAX_RETRY 10
		int retry = MAX_RETRY;
#endif

		start = k;
		for (k = 0; k < b; k++) {
			wait_on_buffer(bh[k]);
			if (!buffer_uptodate(bh[k]))
				goto release_mutex;
		}

		avail = 0;
		for (k = 0; !avail && k < b; k++) {
			avail = msblk->devblksize - offset;
			if (length < avail)
				avail = length;
			if (avail)
				break;
			offset = 0;
			brelse(bh[k]);
		}
		bytes = 0;
		if (!avail)
			goto block_release; // nothing to be process

		start = k;
		percpu = get_cpu_var(sqlzma);
#ifdef KeepPreemptive
		put_cpu_var(sqlzma);
		mutex_lock(&percpu->mtx);
#endif


#ifdef AEI_VDSL_CUSTOMER_NCS
		offset_save = offset;
		avail_save = avail;
		bytes_save = bytes;
try_again:
#endif
		for (; k < b; k++) {
			memcpy(percpu->read_data + bytes, bh[k]->b_data + offset,
			       avail);
			bytes += avail;
			offset = 0;
#ifndef AEI_VDSL_CUSTOMER_NCS
			brelse(bh[k]);
#endif
			avail = msblk->devblksize - offset;
			rest = length - bytes;
			if (rest < avail)
				avail = rest;
		}

		//Now we start to decompress
		sbuf[Src].buf = percpu->read_data;
		sbuf[Src].sz = bytes;
		sbuf[Dst].buf = buffer[0];
		sbuf[Dst].sz = srclength;
		dpri_un(&percpu->un);
		dpri("src %d %p, dst %d %p\n", sbuf[Src].sz, sbuf[Src].buf,
		     sbuf[Dst].sz, sbuf[Dst].buf);
#ifndef AEI_VDSL_CUSTOMER_NCS
		zlib_err = sqlzma_un(&percpu->un, sbuf + Src, sbuf + Dst);
#else
		zlib_err = sqlzma_un(&percpu->un, sbuf + Src, sbuf + Dst, !retry);
		k = start;
		if (unlikely(zlib_err) && retry--) {
			extern void brcm_mtd_read(void *to, unsigned long from, ssize_t len);

			printk("DEBUG: squashfs error, try to re-read, retry=%d\n", MAX_RETRY - retry);
			for (; k < b; k++)
				brcm_mtd_read(bh[k]->b_data, bh[k]->b_blocknr * bh[k]->b_size, bh[k]->b_size);
			offset = offset_save;
			avail = avail_save;
			bytes = bytes_save;
			k = start;
			goto try_again;
		}
		for (; k < b; k++)
			brelse(bh[k]);
#endif
		bytes = percpu->un.un_reslen;

#ifdef KeepPreemptive
		mutex_unlock(&percpu->mtx);
#else
		put_cpu_var(sqlzma);
#endif

		if (unlikely(zlib_err)) {
			dpri("zlib_err %d\n", zlib_err);
 			goto release_mutex;
 		}

		length = bytes;

#else //No LZMA
		int zlib_err = 0, zlib_init = 0;

		/*
		 * Uncompress block.
		 */

		mutex_lock(&msblk->read_data_mutex);

		msblk->stream.avail_out = 0;
		msblk->stream.avail_in = 0;

		bytes = length;
		do {
			if (msblk->stream.avail_in == 0 && k < b) {
				avail = min(bytes, msblk->devblksize - offset);
				bytes -= avail;
				wait_on_buffer(bh[k]);
				if (!buffer_uptodate(bh[k]))
					goto release_mutex;

				if (avail == 0) {
					offset = 0;
					put_bh(bh[k++]);
					continue;
				}

				msblk->stream.next_in = bh[k]->b_data + offset;
				msblk->stream.avail_in = avail;
				offset = 0;
			}

			if (msblk->stream.avail_out == 0 && page < pages) {
				msblk->stream.next_out = buffer[page++];
				msblk->stream.avail_out = PAGE_CACHE_SIZE;
			}

			if (!zlib_init) {
				zlib_err = zlib_inflateInit(&msblk->stream);
				if (zlib_err != Z_OK) {
					ERROR("zlib_inflateInit returned"
						" unexpected result 0x%x,"
						" srclength %d\n", zlib_err,
						srclength);
					goto release_mutex;
				}
				zlib_init = 1;
			}

			zlib_err = zlib_inflate(&msblk->stream, Z_SYNC_FLUSH);

			if (msblk->stream.avail_in == 0 && k < b)
				put_bh(bh[k++]);
		} while (zlib_err == Z_OK);

		if (zlib_err != Z_STREAM_END) {
			ERROR("zlib_inflate error, data probably corrupt\n");
			goto release_mutex;
		}

		zlib_err = zlib_inflateEnd(&msblk->stream);
		if (zlib_err != Z_OK) {
			ERROR("zlib_inflate error, data probably corrupt\n");
			goto release_mutex;
		}
		length = msblk->stream.total_out;
		mutex_unlock(&msblk->read_data_mutex);
#endif		
	} else {
		/*
		 * Block is uncompressed.
		 */
		int i, in, pg_offset = 0;

		for (i = 0; i < b; i++) {
			wait_on_buffer(bh[i]);
			if (!buffer_uptodate(bh[i]))
				goto block_release;
		}

		for (bytes = length; k < b; k++) {
			in = min(bytes, msblk->devblksize - offset);
			bytes -= in;
			while (in) {
				if (pg_offset == PAGE_CACHE_SIZE) {
					page++;
					pg_offset = 0;
				}
				avail = min_t(int, in, PAGE_CACHE_SIZE -
						pg_offset);
				memcpy(buffer[page] + pg_offset,
						bh[k]->b_data + offset, avail);
				in -= avail;
				pg_offset += avail;
				offset += avail;
			}
			offset = 0;
			put_bh(bh[k]);
		}
	}

	kfree(bh);
	return length;

release_mutex:
#ifndef SQUASHFS_LZMA_ENABLE
	mutex_unlock(&msblk->read_data_mutex);
#endif

block_release:
	for (; k < b; k++)
		put_bh(bh[k]);

read_failure:
	ERROR("squashfs_read_data failed to read block 0x%llx\n",
					(unsigned long long) index);
	kfree(bh);
	return -EIO;
}
