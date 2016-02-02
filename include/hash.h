#ifndef HASH_H
#define HASH_H

#include <stdbool.h>
#include <list.h>
#include "internal.h"

/* 2^31 + 2^29 - 2^25 + 2^22 - 2^19 - 2^16 + 1 */
#define GOLDEN_RATIO_PRIME_32 0x9e370001UL

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

static inline u32 hash_32(u32 val, unsigned int bits)
{
	u32 hash = val * GOLDEN_RATIO_PRIME_32;

	/* High bits are more random, so use them. */
	return hash >> (32 - bits);
}

static inline int ilog2(unsigned long v)
{
	int l = 0;
	while ((1UL << l) != v)
		l++;
	return l;
}

#define hash_min(val, bits) (hash_32(val, bits))

#define DEFINE_HASHTABLE(name, bits)                                            \
	struct hlist_head name[1 << (bits)] =                                   \
			{ [0 ... ((1 << (bits)) - 1)] = HLIST_HEAD_INIT }

#define DECLARE_HASHTABLE(name, bits)                                           \
        struct hlist_head name[1 << (bits)]

#define HASH_SIZE(name) (ARRAY_SIZE(name))
#define HASH_BITS(name) (ilog2(HASH_SIZE(name)))

static inline void __hash_init(struct hlist_head *ht, unsigned int sz)
{
	unsigned int i;

	for (i = 0; i < sz; i++)
		INIT_HLIST_HEAD(&ht[i]);
}

#define hash_init(hashtable) __hash_init(hashtable, HASH_SIZE(hashtable))

#define hash_add(hashtable, node, key)                                          \
	hlist_add_head(node, &hashtable[hash_min(key, HASH_BITS(hashtable))])

static inline bool hash_hashed(struct hlist_node *node)
{
	return !hlist_unhashed(node);
}

static inline bool __hash_empty(struct hlist_head *ht, unsigned int sz)
{
	unsigned int i;

	for (i = 0; i < sz; i++)
		if (!hlist_empty(&ht[i]))
			return false;

	return true;
}

#define hash_empty(hashtable) __hash_empty(hashtable, HASH_SIZE(hashtable))

static inline void hash_del(struct hlist_node *node)
{
	hlist_del_init(node);
}

#define hash_for_each(name, bkt, obj, member)                           \
	for ((bkt) = 0, obj = NULL; obj == NULL && (bkt) < HASH_SIZE(name);\
			(bkt)++)\
		hlist_for_each_entry(obj, &name[bkt], member)

#define hash_for_each_safe(name, bkt, tmp, obj, member)                 \
	for ((bkt) = 0, obj = NULL; obj == NULL && (bkt) < HASH_SIZE(name);\
			(bkt)++)\
		hlist_for_each_entry_safe(obj, tmp, &name[bkt], member)

#define hash_for_each_possible(name, obj, member, key)                  \
	hlist_for_each_entry(obj, &name[hash_min(key, HASH_BITS(name))], member)

#define hash_for_each_possible_safe(name, obj, tmp, member, key)        \
	hlist_for_each_entry_safe(obj, tmp,\
		&name[hash_min(key, HASH_BITS(name))], member)

#endif
