#ifndef _LIST_H
#define _LIST_H

#include <hardware.h>

/* Simple linked list inspired from the Linux kernel list implementation */
typedef struct list_head {
	struct list_head *prev, *next;
} list_t;

#define LIST_NULL -1
#define LIST_SUCCESS 1

#define DECLARE_LIST_HEAD(name) struct list_head name
#define LIST(name) struct list_head name = { &(name), &(name) }

#define INIT_LIST_HEAD(h) do { \
	(h)->next = (h); (h)->prev = (h); \
} while (0)

#define INIT_LIST_ELM(le) do { \
	(le)->next = NULL; (le)->prev = NULL; \
} while (0)

#define TO_LIST(container) ((struct list_head *)(container))

#define list_entry(ptr, type, member) ({			\
		const typeof( ((type *)0)->member ) *__mptr = (ptr);	\
		(type *)( (char *)__mptr - offsetof(type,member) );})
#define offsetof(TYPE, MEMBER) ((unsigned long) &((TYPE *)0)->MEMBER)

int list_del(list_t * le);
int list_del_init(list_t * le);
int list_add_tail(list_t * head, list_t * le);
int list_add(list_t * head, list_t * le);

#define list_for_each(curr, head) \
	for (curr = (head)->next; curr != (head); curr = curr->next)

#define list_for_each_safe(pos, tmp, head) \
	for (pos = (head)->next, tmp = pos->next; pos != (head); \
			pos = tmp, tmp = pos->next)

#define list_empty(head) ((head) == (head)->next)

#define list_first(head) ((head)->next)

#define list_unattached(le) ((le)->next == NULL && (le)->prev == NULL)

/* Simple hash list inspired from the Linux kernel hlist implementation */
struct hlist_head {
	struct hlist_node *first;
};

struct hlist_node {
	struct hlist_node *next, **pprev;
};

#define HLIST_HEAD_INIT { .first = NULL }
#define HLIST_HEAD(name) struct hlist_head name = { .first = NULL }
#define INIT_HLIST_HEAD(ptr) ((ptr)->first = NULL)

static inline void INIT_HLIST_NODE(struct hlist_node *h)
{
	h->next = NULL;
	h->pprev = NULL;
}

static inline void hlist_add_head(struct hlist_node *n, struct hlist_head *h)
{
	struct hlist_node *first = h->first;
	n->next = first;
	if (first)
		first->pprev = &n->next;
	h->first = n;
	n->pprev = &h->first;
}

static inline int hlist_unhashed(const struct hlist_node *h)
{
	return !h->pprev;
}

static inline int hlist_empty(const struct hlist_head *h)
{
	return !h->first;
}

static inline void __hlist_del(struct hlist_node *n)
{
	struct hlist_node *next = n->next;
	struct hlist_node **pprev = n->pprev;
	*pprev = next;
	if (next)
		next->pprev = pprev;
}

static inline void hlist_del_init(struct hlist_node *n)
{
	if (!hlist_unhashed(n)) {
		__hlist_del(n);
		INIT_HLIST_NODE(n);
	}
}

#define hlist_entry(ptr, type, member) list_entry(ptr,type,member)

#define hlist_entry_safe(ptr, type, member) \
	({ typeof(ptr) ____ptr = (ptr); \
	   ____ptr ? hlist_entry(____ptr, type, member) : NULL; \
	})

#define hlist_for_each_entry(pos, head, member)                         \
	for (pos = hlist_entry_safe((head)->first, typeof(*(pos)), member);\
	     pos;                                                       \
	     pos = hlist_entry_safe((pos)->member.next, typeof(*(pos)), member))

#define hlist_for_each_entry_safe(pos, n, head, member)                 \
	for (pos = hlist_entry_safe((head)->first, typeof(*pos), member);\
	     pos && ({ n = pos->member.next; 1; });                     \
	     pos = hlist_entry_safe(n, typeof(*pos), member))

#endif
