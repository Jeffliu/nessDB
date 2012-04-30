/* 
 * This is a simple 'sstable' implemention, merge all mtable into on-disk indices 
 * BLOCK's LAYOUT:
 * +--------+--------+--------+--------+
 * |             sst block 1           |
 * +--------+--------+--------+--------+
 * |             sst block 2           |
 * +--------+--------+--------+--------+
 * |      ... all the other blocks ..  |
 * +--------+--------+--------+--------+
 * |             sst block N           |
 * +--------+--------+--------+--------+
 * |             footer                |
 * +--------+--------+--------+--------+
 *
 * FOOTER's LAYOUT:
 * +--------+--------+--------+--------+
 * |               last key            |
 * +--------+--------+--------+--------+
 * |             block count           |
 * +--------+--------+--------+--------+
 * |                 crc               |
 * +--------+--------+--------+--------+

 * nessDB storage engine
 * Copyright (c) 2011-2012, BohuTANG <overred.shuttler at gmail dot com>
 * All rights reserved.
 * Code is licensed with BSD. See COPYING.BSD file.
 * 
 */
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/mman.h>

#include "buffer.h"
#include "sst.h"
#include "debug.h"

#define BLK_MAGIC (20111225)
#define F_CRC (2011)

struct footer{
	char key[NESSDB_MAX_KEY_SIZE];
	__be32 count;
	__be32 crc;
	__be32 size;
	__be32 max_len;
	__be32 max_lcp;
	__be64 offset_delta;
};

struct stats {
	int mmap_size;
	int max_len;
	int max_lcp;
};

void _prepare_stats(struct skipnode *x, size_t count, struct stats *stats)
{
	size_t i;
	int real_count = 0;
	int goon = 1;
	int max_len = 0;
	int max_lcp = 0;
	char *pre_key = NULL;

	struct skipnode *node = x;

	memset(stats, 0, sizeof(struct stats));
	for (i = 0; i < count; i++) {
		if (node->opt == ADD) {
			real_count++;
			max_len = node->klen > max_len ? node->klen : max_len;
			if (goon && pre_key) {
				int k = 0;

				while (pre_key[k] == node->key[k] && pre_key[k] != '\0')
					k++;

				if (k == 0) {
					goon = 0;
					continue;
				}

				max_lcp = max_lcp > k ? k : max_lcp;
			}

			pre_key = node->key;
		}

		node = node->forward[0];
	}

	stats->max_len = max_len;
	stats->max_lcp = max_lcp;
	stats->mmap_size = (max_len + sizeof(uint32_t)) * real_count ;

	if (max_lcp > 0)
		__DEBUG(LEVEL_INFO, "max_len:%d, max_lcp:%d", max_len, max_lcp);
}

void _sst_load(struct sst *sst)
{
	int fd, result, all_count = 0;
	DIR *dd;
	struct dirent *de;

	dd = opendir(sst->basedir);
	while ((de = readdir(dd))) {
		if (strstr(de->d_name, ".sst")) {
			int fcount = 0, fcrc = 0;
			struct meta_node mn;
			struct footer footer;
			char sst_file[FILE_PATH_SIZE];
			int fsize = sizeof(struct footer);

			memset(sst_file, 0, FILE_PATH_SIZE);
			snprintf(sst_file, FILE_PATH_SIZE, "%s/%s", sst->basedir, de->d_name);
			
			fd = open(sst_file, O_RDWR, 0644);
			lseek(fd, -fsize, SEEK_END);
			result = read(fd, &footer, sizeof(struct footer));
			if (result != sizeof(struct footer))
				__PANIC("read footer error");

			fcount = from_be32(footer.count);
			fcrc = from_be32(footer.crc);
			if (fcrc != F_CRC) {
				__PANIC("Crc wrong, sst file maybe broken, crc:<%d>,index<%s>", fcrc, sst_file);
				close(fd);
				continue;
			}

			if (fcount == 0) {
				close(fd);
				continue;
			}

			all_count += fcount;
						
			/* Set meta */
			mn.count = fcount;
			memset(mn.end, 0, NESSDB_MAX_KEY_SIZE);
			memcpy(mn.end, footer.key, NESSDB_MAX_KEY_SIZE);

			memset(mn.index_name, 0, FILE_NAME_SIZE);
			memcpy(mn.index_name, de->d_name, FILE_NAME_SIZE);
			meta_set(sst->meta, &mn);
		
			close(fd);
		}
	}

	closedir(dd);
	__DEBUG(LEVEL_DEBUG, "Load sst,all entries count:<%d>", all_count);
}

struct sst *sst_new(const char *basedir)
{
	struct sst *s;

	s = calloc(1, sizeof(struct sst));

	s->meta = meta_new();
	memcpy(s->basedir, basedir, FILE_PATH_SIZE);

	s->bloom = bloom_new();
	s->mutexer.lsn = -1;
	pthread_mutex_init(&s->mutexer.mutex, NULL);

	/* SST files load */
	_sst_load(s);
	
	return s;
}

void *_write_mmap(struct sst *sst, struct skipnode *x, size_t count, int need_new)
{
	int i, j, c_clone;
	int fd;
	int result;
	char file[FILE_PATH_SIZE];
	struct skipnode *last;
	struct footer footer;
	struct stats stats;

	int fsize = sizeof(struct footer);

	_prepare_stats(x, count, &stats);

	struct inner_block {
		char key[stats.max_len];
		__be32 offset;
	};

	struct inner_block *blks;

	memset(file, 0, FILE_PATH_SIZE);
	snprintf(file, FILE_PATH_SIZE, "%s/%s", sst->basedir, sst->name);
	fd = open(file, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd == -1)
		__PANIC("create sst file error");

	if (lseek(fd, stats.mmap_size - 1, SEEK_SET) == -1)
		__PANIC("lseek sst error");

	result = write(fd, "", 1);
	if (result == -1)
		__PANIC("write empty error");

	blks = mmap(0, stats.mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (blks == MAP_FAILED) {
		__PANIC("map error when write");
	}

	last = x;
	c_clone = count;

	for (i = 0, j= 0; i < c_clone; i++) {
		if (x->opt == ADD) {
			memset(blks[j].key, 0, stats.max_len);
			memcpy(blks[j].key, x->key, x->klen);
			blks[j].offset = to_be32(x->val);

			j++;
		} else
			count--;

		last = x;
		x = x->forward[0];
	}

#ifdef MSYNC
	if (msync(blks , stats.mmap_size, MS_SYNC) == -1) {
		__DEBUG(LEVEL_ERROR, "Msync error");
	}
#endif

	if (munmap(blks , stats.mmap_size) == -1) {
		__DEBUG(LEVEL_ERROR, "Un-mmapping the file");
	}
	
	footer.count = to_be32(count);
	footer.crc = to_be32(F_CRC);
	footer.size = to_be32(stats.mmap_size);
	footer.max_len = to_be32(stats.max_len);
	footer.max_lcp = to_be32(stats.max_lcp);


	memset(footer.key, 0, NESSDB_MAX_KEY_SIZE);
	memcpy(footer.key, last->key, last->klen);

	result = write(fd, &footer, fsize);
	if (result == -1)
		__PANIC("write footer");

	/* Set meta */
	struct meta_node mn;

	mn.count = count;
	memset(mn.end, 0, NESSDB_MAX_KEY_SIZE);
	memcpy(mn.end, last->key, NESSDB_MAX_KEY_SIZE);

	memset(mn.index_name, 0, FILE_NAME_SIZE);
	memcpy(mn.index_name, sst->name, FILE_NAME_SIZE);
	
	if (need_new) 
		meta_set(sst->meta, &mn);
	else 
		meta_set_byname(sst->meta, &mn);

	close(fd);

	return x;
}

struct skiplist *_read_mmap(struct sst *sst, size_t count)
{
	int i;
	int fd;
	int result;
	int fcount;
	int blk_sizes;
	char file[FILE_PATH_SIZE];
	struct skiplist *merge = NULL;
	struct footer footer;
	int fsize = sizeof(struct footer);

	memset(file, 0, FILE_PATH_SIZE);
	snprintf(file, FILE_PATH_SIZE, "%s/%s", sst->basedir, sst->name);

	fd = open(file, O_RDWR, 0644);
	if (fd == -1)
		__PANIC("open sst error when read map");

	result = lseek(fd, -fsize, SEEK_END);
	if (result == -1)
		__PANIC("lseek footer  error");

	result = read(fd, &footer, fsize);
	if (result != fsize) {
		__PANIC("read error when read footer");
	}

	fcount = from_be32(footer.count);

	blk_sizes = from_be32(footer.size);

	struct inner_block {
		char key[from_be32(footer.max_len)];
		uint32_t offset;
	};

	struct inner_block *blks;

	/* Blocks read */
	blks = mmap(0, blk_sizes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (blks == MAP_FAILED) {
		__PANIC("map error when read");
		goto out;
	}

	/* Merge */

	struct slice sk;
	uint64_t offset;
	int max_len = from_be32(footer.max_len);

	merge = skiplist_new(fcount + count + 1);
	for (i = 0; i < fcount; i++) {
		char key[max_len + 1];

		memcpy(key, blks[i].key, max_len);
		key[max_len] = 0;

		sk.len = max_len;
		sk.data = key;

		offset = from_be32(blks[i].offset);
		skiplist_insert(merge, &sk, offset, ADD);
	}
	
	if (munmap(blks , blk_sizes) == -1)
		__DEBUG(LEVEL_ERROR, "Un-mmapping the file");

out:
	close(fd);

	return merge;
}

uint64_t _read_offset(struct sst *sst, struct slice *sk)
{
	int fd;
	int fcount;
	int blk_sizes;
	int result;
	uint64_t off = 0UL;
	char file[FILE_PATH_SIZE];
	char *mmaps;
	struct footer footer;
	int fsize = sizeof(struct footer);

	memset(file, 0, FILE_PATH_SIZE);
	snprintf(file, FILE_PATH_SIZE, "%s/%s", sst->basedir, sst->name);

	fd = open(file, O_RDWR, 0644);
	if (fd == -1) {
		__DEBUG(LEVEL_ERROR, "open sst error when read offset");
		return 0UL;
	}
	
	result = lseek(fd, -fsize, SEEK_END);
	if (result == -1) {
		__DEBUG(LEVEL_ERROR, "lseek error when read offset");
		goto out;
	}

	result = read(fd, &footer, fsize);
	if (result == -1) {
		__DEBUG(LEVEL_ERROR, "read footer error when read offset");
		goto out;
	}


	fcount = from_be32(footer.count);
	blk_sizes = from_be32(footer.size); 

	/* Blocks read */
	mmaps = mmap(0, blk_sizes, PROT_READ, MAP_SHARED, fd, 0);
	if (mmaps == MAP_FAILED) {
		__DEBUG(LEVEL_ERROR, "Map_failed when read");
		goto out;
	}

	int i;
	int cur = 0;

	for (i = 0; i < fcount; i++) {
		uint16_t klen = u16_from_big((unsigned char*)(mmaps + cur));
		cur += sizeof(klen);

		int cmp = memcmp(sk->data, mmaps + cur , klen);
		cur += klen;
		if (cmp == 0) {
			off = u64_from_big((unsigned char*)(mmaps + cur));
			break;
		}

		cur += sizeof(off);
	}


	if (munmap(mmaps, blk_sizes) == -1)
		__DEBUG(LEVEL_ERROR, "un-mmapping the file");

out:
	close(fd);

	return off;
}

void _flush_merge_list(struct sst *sst, struct skipnode *x, size_t count, struct meta_node *meta)
{
	int mul;
	int rem;
	int lsn;
	int i;

	/* Less than 2x SST_MAX_COUNT,compact one index file */
	if (count <= SST_MAX_COUNT * 2) {
		if (meta) {
			lsn = meta->lsn;
			sst->mutexer.lsn = lsn;
			pthread_mutex_lock(&sst->mutexer.mutex);
			x = _write_mmap(sst, x, count, 0);
			pthread_mutex_unlock(&sst->mutexer.mutex);
			sst->mutexer.lsn = -1;
		} else 
			x = _write_mmap(sst, x, count, 0);
	} else {
		if (meta) {
			lsn = meta->lsn;
			sst->mutexer.lsn = lsn;
			pthread_mutex_lock(&sst->mutexer.mutex);
			x = _write_mmap(sst, x, SST_MAX_COUNT, 0);
			pthread_mutex_unlock(&sst->mutexer.mutex);
			sst->mutexer.lsn = -1;
		} else
			x = _write_mmap(sst, x, SST_MAX_COUNT, 0);

		/* first+last */
		mul = (count - SST_MAX_COUNT * 2) / SST_MAX_COUNT;
		rem = count % SST_MAX_COUNT;

		for (i = 0; i < mul; i++) {
			memset(sst->name, 0, FILE_NAME_SIZE);
			snprintf(sst->name, FILE_NAME_SIZE, "%d.sst", sst->meta->size); 
			x = _write_mmap(sst, x, SST_MAX_COUNT, 1);
		}

		/* The remain part,will be larger than SST_MAX_COUNT */
		memset(sst->name, 0, FILE_NAME_SIZE);
		snprintf(sst->name, FILE_NAME_SIZE, "%d.sst", sst->meta->size); 

		x = _write_mmap(sst, x, rem + SST_MAX_COUNT, 1);
	}	
}

void _flush_new_list(struct sst *sst, struct skipnode *x, size_t count)
{
	int mul ;
	int rem;
	int i;

	if (count <= SST_MAX_COUNT * 2) {
		memset(sst->name, 0, FILE_NAME_SIZE);
		snprintf(sst->name, FILE_NAME_SIZE, "%d.sst", sst->meta->size); 
		x = _write_mmap(sst, x, count, 1);
	} else {
		mul = count / SST_MAX_COUNT;
		rem = count % SST_MAX_COUNT;

		for (i = 0; i < (mul - 1); i++) {
			memset(sst->name, 0, FILE_NAME_SIZE);
			snprintf(sst->name, FILE_NAME_SIZE, "%d.sst", sst->meta->size); 
			x = _write_mmap(sst, x, SST_MAX_COUNT, 1);
		}

		memset(sst->name, 0, FILE_NAME_SIZE);
		snprintf(sst->name, FILE_NAME_SIZE, "%d.sst", sst->meta->size); 
		x = _write_mmap(sst, x, SST_MAX_COUNT + rem, 1);
	}
}

void _flush_list(struct sst *sst, struct skipnode *x,struct skipnode *hdr,int flush_count)
{
	int pos = 0;
	int count = flush_count;
	struct skipnode *cur = x;
	struct skipnode *first = hdr;
	struct skiplist *merge = NULL;
	struct meta_node *meta_info = NULL;

	while(cur != first) {
		meta_info = meta_get(sst->meta, cur->key);

		/* If m is NULL, cur->key more larger than meta's largest area
		 * need to create new index-file
		 */
		if(!meta_info){

			/* If merge is NULL,it has no merge*/
			if(merge) {
				struct skipnode *h = merge->hdr->forward[0];
				_flush_merge_list(sst, h, merge->count, NULL);
				skiplist_free(merge);
				merge = NULL;
			}

			/* Flush the last nodes to disk */
			_flush_new_list(sst, x, count - pos);

			return;
		} else {

			/* If m is not NULL,means found the index of the cur
			 * We need:
			 * 1) compare the sst->name with meta index name
			 *		a)If 0: add the cur to merge,and continue
			 *		b)others:
			 *			b1)Flush the merge list to disk
			 *			b2)Open the meta's mmap,and load all blocks to new merge,add cur to merge
			 */
			int cmp = strcmp(sst->name, meta_info->index_name);
			if(cmp == 0) {
				if (!merge)
					merge = _read_mmap(sst,count);	

				skiplist_insert_node(merge, cur);
			} else {
				if (merge) {
					struct skipnode *h = merge->hdr->forward[0];

					_flush_merge_list(sst, h, merge->count, meta_info);
					skiplist_free(merge);
					merge = NULL;
				}

				memset(sst->name, 0, FILE_NAME_SIZE);
				memcpy(sst->name, meta_info->index_name, FILE_NAME_SIZE);
				merge = _read_mmap(sst, count);

				/* Add to merge list */
				skiplist_insert_node(merge, cur);
			}

		}

		pos++;
		cur = cur->forward[0];
	}

	if (merge) {
		struct skipnode *h = merge->hdr->forward[0];
		_flush_merge_list(sst, h, merge->count, meta_info);
		skiplist_free(merge);
	}
}

void sst_merge(struct sst *sst, struct skiplist *list, int fromlog)
{
	struct skipnode *x= list->hdr->forward[0];

	if (fromlog == 1) {
		struct skipnode *cur = x;
		struct skipnode *first = list->hdr;

		__DEBUG(LEVEL_DEBUG, "adding log items to bloomfilter");
		while (cur != first) {
			if (cur->opt == ADD)
				bloom_add(sst->bloom, cur->key);
			cur = cur->forward[0];
		}
	}

	/* First time,index is NULL,need to be created */
	if (sst->meta->size == 0)
		 _flush_new_list(sst, x, list->count);
	else
		_flush_list(sst, x, list->hdr, list->count);
	skiplist_free(list);
}

uint64_t sst_getoff(struct sst *sst, struct slice *sk)
{
	int lsn;
	uint64_t off = 0UL;
	struct meta_node *meta_info;

	meta_info = meta_get(sst->meta, sk->data);
	if(!meta_info)
		return 0UL;

	memcpy(sst->name, meta_info->index_name, FILE_NAME_SIZE);

	/* If get one record from on-disk sst file, 
	 * this file must not be operated by bg-merge thread
	 */
	lsn = meta_info->lsn;
	if (sst->mutexer.lsn == lsn) {
		pthread_mutex_lock(&sst->mutexer.mutex);
		off = _read_offset(sst, sk);
		pthread_mutex_unlock(&sst->mutexer.mutex);
	} else {
		off = _read_offset(sst, sk);
	}

	return off;
}

void sst_free(struct sst *sst)
{
	if (sst) {
		meta_free(sst->meta);
		bloom_free(sst->bloom);
		free(sst);
	}
}
