/*
 *	Generic address resolution entity
 *
 *	Authors:
 *	Pedro Roque		<roque@di.fc.ul.pt>
 *	Alexey Kuznetsov	<kuznet@ms2.inr.ac.ru>
 *
 *	This program is free software; you can redistribute it and/or
 *      modify it under the terms of the GNU General Public License
 *      as published by the Free Software Foundation; either version
 *      2 of the License, or (at your option) any later version.
 *
 *	Fixes:
 *	Vitaly E. Lavrov	releasing NULL neighbor in neigh_add.
 *	Harald Welte		Add neighbour cache statistics like rtstat
 */

#include <linux/slab.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/socket.h>
#include <linux/netdevice.h>
#include <linux/proc_fs.h>
#ifdef CONFIG_SYSCTL
#include <linux/sysctl.h>
#endif
#include <linux/times.h>
#include <net/net_namespace.h>
#include <net/neighbour.h>
#include <net/dst.h>
#include <net/sock.h>
#include <net/netevent.h>
#include <net/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/random.h>
#include <linux/string.h>
#include <linux/log2.h>

#define NEIGH_DEBUG 1

#define NEIGH_PRINTK(x...) printk(x)
#define NEIGH_NOPRINTK(x...) do { ; } while(0)
#define NEIGH_PRINTK1 NEIGH_NOPRINTK
#define NEIGH_PRINTK2 NEIGH_NOPRINTK

#if NEIGH_DEBUG >= 1
#undef NEIGH_PRINTK1
#define NEIGH_PRINTK1 NEIGH_PRINTK
#endif
#if NEIGH_DEBUG >= 2
#undef NEIGH_PRINTK2
#define NEIGH_PRINTK2 NEIGH_PRINTK
#endif

#define PNEIGH_HASHMASK		0xF

static void neigh_timer_handler(unsigned long arg);
static void __neigh_notify(struct neighbour *n, int type, int flags);
static void neigh_update_notify(struct neighbour *neigh);
static int pneigh_ifdown(struct neigh_table *tbl, struct net_device *dev);

static struct neigh_table *neigh_tables;
#ifdef CONFIG_PROC_FS
static const struct file_operations neigh_stat_seq_fops;
#endif


static DEFINE_RWLOCK(neigh_tbl_lock);

static int neigh_blackhole(struct neighbour *neigh, struct sk_buff *skb)
{
	kfree_skb(skb);
	return -ENETDOWN;
}

static void neigh_cleanup_and_release(struct neighbour *neigh)
{
	if (neigh->parms->neigh_cleanup)
		neigh->parms->neigh_cleanup(neigh);

	__neigh_notify(neigh, RTM_DELNEIGH, 0);
	neigh_release(neigh);
}


unsigned long neigh_rand_reach_time(unsigned long base)
{
	return base ? (net_random() % base) + (base >> 1) : 0;
}
EXPORT_SYMBOL(neigh_rand_reach_time);


static int neigh_forced_gc(struct neigh_table *tbl)
{
	int shrunk = 0;
	int i;
	struct neigh_hash_table *nht;

	NEIGH_CACHE_STAT_INC(tbl, forced_gc_runs);

	write_lock_bh(&tbl->lock);
	nht = rcu_dereference_protected(tbl->nht,
					lockdep_is_held(&tbl->lock));
	for (i = 0; i < (1 << nht->hash_shift); i++) {
		struct neighbour *n;
		struct neighbour __rcu **np;

		np = &nht->hash_buckets[i];
		while ((n = rcu_dereference_protected(*np,
					lockdep_is_held(&tbl->lock))) != NULL) {
			write_lock(&n->lock);
			if (atomic_read(&n->refcnt) == 1 &&
			    !(n->nud_state & NUD_PERMANENT)) {
				rcu_assign_pointer(*np,
					rcu_dereference_protected(n->next,
						  lockdep_is_held(&tbl->lock)));
				n->dead = 1;
				shrunk	= 1;
				write_unlock(&n->lock);
				neigh_cleanup_and_release(n);
				continue;
			}
			write_unlock(&n->lock);
			np = &n->next;
		}
	}

	tbl->last_flush = jiffies;

	write_unlock_bh(&tbl->lock);

	return shrunk;
}

static void neigh_add_timer(struct neighbour *n, unsigned long when)
{
	neigh_hold(n);
	if (unlikely(mod_timer(&n->timer, when))) {
		printk("NEIGH: BUG, double timer add, state is %x\n",
		       n->nud_state);
		dump_stack();
	}
}

static int neigh_del_timer(struct neighbour *n)
{
	if ((n->nud_state & NUD_IN_TIMER) &&
	    del_timer(&n->timer)) {
		neigh_release(n);
		return 1;
	}
	return 0;
}

static void pneigh_queue_purge(struct sk_buff_head *list)
{
	struct sk_buff *skb;

	while ((skb = skb_dequeue(list)) != NULL) {
		dev_put(skb->dev);
		kfree_skb(skb);
	}
}

static void neigh_flush_dev(struct neigh_table *tbl, struct net_device *dev)
{
	int i;
	struct neigh_hash_table *nht;

	nht = rcu_dereference_protected(tbl->nht,
					lockdep_is_held(&tbl->lock));

	for (i = 0; i < (1 << nht->hash_shift); i++) {
		struct neighbour *n;
		struct neighbour __rcu **np = &nht->hash_buckets[i];

		while ((n = rcu_dereference_protected(*np,
					lockdep_is_held(&tbl->lock))) != NULL) {
			if (dev && n->dev != dev) {
				np = &n->next;
				continue;
			}
			rcu_assign_pointer(*np,
				   rcu_dereference_protected(n->next,
						lockdep_is_held(&tbl->lock)));
			write_lock(&n->lock);
			neigh_del_timer(n);
			n->dead = 1;

			if (atomic_read(&n->refcnt) != 1) {
				skb_queue_purge(&n->arp_queue);
				n->arp_queue_len_bytes = 0;
				n->output = neigh_blackhole;
				if (n->nud_state & NUD_VALID)
					n->nud_state = NUD_NOARP;
				else
					n->nud_state = NUD_NONE;
				NEIGH_PRINTK2("neigh %p is stray.\n", n);
			}
			write_unlock(&n->lock);
			neigh_cleanup_and_release(n);
		}
	}
}

void neigh_changeaddr(struct neigh_table *tbl, struct net_device *dev)
{
	write_lock_bh(&tbl->lock);
	neigh_flush_dev(tbl, dev);
	write_unlock_bh(&tbl->lock);
}
EXPORT_SYMBOL(neigh_changeaddr);

int neigh_ifdown(struct neigh_table *tbl, struct net_device *dev)
{
	write_lock_bh(&tbl->lock);
	neigh_flush_dev(tbl, dev);
	pneigh_ifdown(tbl, dev);
	write_unlock_bh(&tbl->lock);

	del_timer_sync(&tbl->proxy_timer);
	pneigh_queue_purge(&tbl->proxy_queue);
	return 0;
}
EXPORT_SYMBOL(neigh_ifdown);

static struct neighbour *neigh_alloc(struct neigh_table *tbl, struct net_device *dev)
{
	struct neighbour *n = NULL;
	unsigned long now = jiffies;
	int entries;

	entries = atomic_inc_return(&tbl->entries) - 1;
	if (entries >= tbl->gc_thresh3 ||
	    (entries >= tbl->gc_thresh2 &&
	     time_after(now, tbl->last_flush + 5 * HZ))) {
		if (!neigh_forced_gc(tbl) &&
		    entries >= tbl->gc_thresh3)
			goto out_entries;
	}

	if (tbl->entry_size)
		n = kzalloc(tbl->entry_size, GFP_ATOMIC);
	else {
		int sz = sizeof(*n) + tbl->key_len;

		sz = ALIGN(sz, NEIGH_PRIV_ALIGN);
		sz += dev->neigh_priv_len;
		n = kzalloc(sz, GFP_ATOMIC);
	}
	if (!n)
		goto out_entries;

	skb_queue_head_init(&n->arp_queue);
	rwlock_init(&n->lock);
	seqlock_init(&n->ha_lock);
	n->updated	  = n->used = now;
	n->nud_state	  = NUD_NONE;
	n->output	  = neigh_blackhole;
	seqlock_init(&n->hh.hh_lock);
	n->parms	  = neigh_parms_clone(&tbl->parms);
	setup_timer(&n->timer, neigh_timer_handler, (unsigned long)n);

	NEIGH_CACHE_STAT_INC(tbl, allocs);
	n->tbl		  = tbl;
	atomic_set(&n->refcnt, 1);
	n->dead		  = 1;
out:
	return n;

out_entries:
	atomic_dec(&tbl->entries);
	goto out;
}

static void neigh_get_hash_rnd(u32 *x)
{
	get_random_bytes(x, sizeof(*x));
	*x |= 1;
}

static struct neigh_hash_table *neigh_hash_alloc(unsigned int shift)
{
	size_t size = (1 << shift) * sizeof(struct neighbour *);
	struct neigh_hash_table *ret;
	struct neighbour __rcu **buckets;
	int i;

	ret = kmalloc(sizeof(*ret), GFP_ATOMIC);
	if (!ret)
		return NULL;
	if (size <= PAGE_SIZE)
		buckets = kzalloc(size, GFP_ATOMIC);
	else
		buckets = (struct neighbour __rcu **)
			  __get_free_pages(GFP_ATOMIC | __GFP_ZERO,
					   get_order(size));
	if (!buckets) {
		kfree(ret);
		return NULL;
	}
	ret->hash_buckets = buckets;
	ret->hash_shift = shift;
	for (i = 0; i < NEIGH_NUM_HASH_RND; i++)
		neigh_get_hash_rnd(&ret->hash_rnd[i]);
	return ret;
}

static void neigh_hash_free_rcu(struct rcu_head *head)
{
	struct neigh_hash_table *nht = container_of(head,
						    struct neigh_hash_table,
						    rcu);
	size_t size = (1 << nht->hash_shift) * sizeof(struct neighbour *);
	struct neighbour __rcu **buckets = nht->hash_buckets;

	if (size <= PAGE_SIZE)
		kfree(buckets);
	else
		free_pages((unsigned long)buckets, get_order(size));
	kfree(nht);
}

static struct neigh_hash_table *neigh_hash_grow(struct neigh_table *tbl,
						unsigned long new_shift)
{
	unsigned int i, hash;
	struct neigh_hash_table *new_nht, *old_nht;

	NEIGH_CACHE_STAT_INC(tbl, hash_grows);

	old_nht = rcu_dereference_protected(tbl->nht,
					    lockdep_is_held(&tbl->lock));
	new_nht = neigh_hash_alloc(new_shift);
	if (!new_nht)
		return old_nht;

	for (i = 0; i < (1 << old_nht->hash_shift); i++) {
		struct neighbour *n, *next;

		for (n = rcu_dereference_protected(old_nht->hash_buckets[i],
						   lockdep_is_held(&tbl->lock));
		     n != NULL;
		     n = next) {
			hash = tbl->hash(n->primary_key, n->dev,
					 new_nht->hash_rnd);

			hash >>= (32 - new_nht->hash_shift);
			next = rcu_dereference_protected(n->next,
						lockdep_is_held(&tbl->lock));

			rcu_assign_pointer(n->next,
					   rcu_dereference_protected(
						new_nht->hash_buckets[hash],
						lockdep_is_held(&tbl->lock)));
			rcu_assign_pointer(new_nht->hash_buckets[hash], n);
		}
	}

	rcu_assign_pointer(tbl->nht, new_nht);
	call_rcu(&old_nht->rcu, neigh_hash_free_rcu);
	return new_nht;
}

struct neighbour *neigh_lookup(struct neigh_table *tbl, const void *pkey,
			       struct net_device *dev)
{
	struct neighbour *n;
	int key_len = tbl->key_len;
	u32 hash_val;
	struct neigh_hash_table *nht;

	NEIGH_CACHE_STAT_INC(tbl, lookups);

	rcu_read_lock_bh();
	nht = rcu_dereference_bh(tbl->nht);
	
	if ((!dev) || (!nht)) {
		rcu_read_unlock_bh();
		return NULL;
	}
	
	hash_val = tbl->hash(pkey, dev, nht->hash_rnd) >> (32 - nht->hash_shift);

	for (n = rcu_dereference_bh(nht->hash_buckets[hash_val]);
	     n != NULL;
	     n = rcu_dereference_bh(n->next)) {
		if (dev == n->dev && !memcmp(n->primary_key, pkey, key_len)) {
			if (!atomic_inc_not_zero(&n->refcnt))
				n = NULL;
			NEIGH_CACHE_STAT_INC(tbl, hits);
			break;
		}
	}

	rcu_read_unlock_bh();
	return n;
}
EXPORT_SYMBOL(neigh_lookup);

struct neighbour *neigh_lookup_nodev(struct neigh_table *tbl, struct net *net,
				     const void *pkey)
{
	struct neighbour *n;
	int key_len = tbl->key_len;
	u32 hash_val;
	struct neigh_hash_table *nht;

	NEIGH_CACHE_STAT_INC(tbl, lookups);

	rcu_read_lock_bh();
	nht = rcu_dereference_bh(tbl->nht);
	hash_val = tbl->hash(pkey, NULL, nht->hash_rnd) >> (32 - nht->hash_shift);

	for (n = rcu_dereference_bh(nht->hash_buckets[hash_val]);
	     n != NULL;
	     n = rcu_dereference_bh(n->next)) {
		if (!memcmp(n->primary_key, pkey, key_len) &&
		    net_eq(dev_net(n->dev), net)) {
			if (!atomic_inc_not_zero(&n->refcnt))
				n = NULL;
			NEIGH_CACHE_STAT_INC(tbl, hits);
			break;
		}
	}

	rcu_read_unlock_bh();
	return n;
}
EXPORT_SYMBOL(neigh_lookup_nodev);

struct neighbour *neigh_create(struct neigh_table *tbl, const void *pkey,
			       struct net_device *dev)
{
	u32 hash_val;
	int key_len = tbl->key_len;
	int error;
	struct neighbour *n1, *rc, *n = neigh_alloc(tbl, dev);
	struct neigh_hash_table *nht;

	if (!n) {
		rc = ERR_PTR(-ENOBUFS);
		goto out;
	}

	memcpy(n->primary_key, pkey, key_len);
	n->dev = dev;
	dev_hold(dev);

	
	if (tbl->constructor &&	(error = tbl->constructor(n)) < 0) {
		rc = ERR_PTR(error);
		goto out_neigh_release;
	}

	if (dev->netdev_ops->ndo_neigh_construct) {
		error = dev->netdev_ops->ndo_neigh_construct(n);
		if (error < 0) {
			rc = ERR_PTR(error);
			goto out_neigh_release;
		}
	}

	
	if (n->parms->neigh_setup &&
	    (error = n->parms->neigh_setup(n)) < 0) {
		rc = ERR_PTR(error);
		goto out_neigh_release;
	}

	n->confirmed = jiffies - (n->parms->base_reachable_time << 1);

	write_lock_bh(&tbl->lock);
	nht = rcu_dereference_protected(tbl->nht,
					lockdep_is_held(&tbl->lock));

	if (atomic_read(&tbl->entries) > (1 << nht->hash_shift))
		nht = neigh_hash_grow(tbl, nht->hash_shift + 1);

	hash_val = tbl->hash(pkey, dev, nht->hash_rnd) >> (32 - nht->hash_shift);

	if (n->parms->dead) {
		rc = ERR_PTR(-EINVAL);
		goto out_tbl_unlock;
	}

	for (n1 = rcu_dereference_protected(nht->hash_buckets[hash_val],
					    lockdep_is_held(&tbl->lock));
	     n1 != NULL;
	     n1 = rcu_dereference_protected(n1->next,
			lockdep_is_held(&tbl->lock))) {
		if (dev == n1->dev && !memcmp(n1->primary_key, pkey, key_len)) {
			neigh_hold(n1);
			rc = n1;
			goto out_tbl_unlock;
		}
	}

	n->dead = 0;
	neigh_hold(n);
	rcu_assign_pointer(n->next,
			   rcu_dereference_protected(nht->hash_buckets[hash_val],
						     lockdep_is_held(&tbl->lock)));
	rcu_assign_pointer(nht->hash_buckets[hash_val], n);
	write_unlock_bh(&tbl->lock);
	NEIGH_PRINTK2("neigh %p is created.\n", n);
	rc = n;
out:
	return rc;
out_tbl_unlock:
	write_unlock_bh(&tbl->lock);
out_neigh_release:
	neigh_release(n);
	goto out;
}
EXPORT_SYMBOL(neigh_create);

static u32 pneigh_hash(const void *pkey, int key_len)
{
	u32 hash_val = *(u32 *)(pkey + key_len - 4);
	hash_val ^= (hash_val >> 16);
	hash_val ^= hash_val >> 8;
	hash_val ^= hash_val >> 4;
	hash_val &= PNEIGH_HASHMASK;
	return hash_val;
}

static struct pneigh_entry *__pneigh_lookup_1(struct pneigh_entry *n,
					      struct net *net,
					      const void *pkey,
					      int key_len,
					      struct net_device *dev)
{
	while (n) {
		if (!memcmp(n->key, pkey, key_len) &&
		    net_eq(pneigh_net(n), net) &&
		    (n->dev == dev || !n->dev))
			return n;
		n = n->next;
	}
	return NULL;
}

struct pneigh_entry *__pneigh_lookup(struct neigh_table *tbl,
		struct net *net, const void *pkey, struct net_device *dev)
{
	int key_len = tbl->key_len;
	u32 hash_val = pneigh_hash(pkey, key_len);

	return __pneigh_lookup_1(tbl->phash_buckets[hash_val],
				 net, pkey, key_len, dev);
}
EXPORT_SYMBOL_GPL(__pneigh_lookup);

struct pneigh_entry * pneigh_lookup(struct neigh_table *tbl,
				    struct net *net, const void *pkey,
				    struct net_device *dev, int creat)
{
	struct pneigh_entry *n;
	int key_len = tbl->key_len;
	u32 hash_val = pneigh_hash(pkey, key_len);

	read_lock_bh(&tbl->lock);
	n = __pneigh_lookup_1(tbl->phash_buckets[hash_val],
			      net, pkey, key_len, dev);
	read_unlock_bh(&tbl->lock);

	if (n || !creat)
		goto out;

	ASSERT_RTNL();

	n = kmalloc(sizeof(*n) + key_len, GFP_KERNEL);
	if (!n)
		goto out;

	write_pnet(&n->net, hold_net(net));
	memcpy(n->key, pkey, key_len);
	n->dev = dev;
	if (dev)
		dev_hold(dev);

	if (tbl->pconstructor && tbl->pconstructor(n)) {
		if (dev)
			dev_put(dev);
		release_net(net);
		kfree(n);
		n = NULL;
		goto out;
	}

	write_lock_bh(&tbl->lock);
	n->next = tbl->phash_buckets[hash_val];
	tbl->phash_buckets[hash_val] = n;
	write_unlock_bh(&tbl->lock);
out:
	return n;
}
EXPORT_SYMBOL(pneigh_lookup);


int pneigh_delete(struct neigh_table *tbl, struct net *net, const void *pkey,
		  struct net_device *dev)
{
	struct pneigh_entry *n, **np;
	int key_len = tbl->key_len;
	u32 hash_val = pneigh_hash(pkey, key_len);

	write_lock_bh(&tbl->lock);
	for (np = &tbl->phash_buckets[hash_val]; (n = *np) != NULL;
	     np = &n->next) {
		if (!memcmp(n->key, pkey, key_len) && n->dev == dev &&
		    net_eq(pneigh_net(n), net)) {
			*np = n->next;
			write_unlock_bh(&tbl->lock);
			if (tbl->pdestructor)
				tbl->pdestructor(n);
			if (n->dev)
				dev_put(n->dev);
			release_net(pneigh_net(n));
			kfree(n);
			return 0;
		}
	}
	write_unlock_bh(&tbl->lock);
	return -ENOENT;
}

static int pneigh_ifdown(struct neigh_table *tbl, struct net_device *dev)
{
	struct pneigh_entry *n, **np;
	u32 h;

	for (h = 0; h <= PNEIGH_HASHMASK; h++) {
		np = &tbl->phash_buckets[h];
		while ((n = *np) != NULL) {
			if (!dev || n->dev == dev) {
				*np = n->next;
				if (tbl->pdestructor)
					tbl->pdestructor(n);
				if (n->dev)
					dev_put(n->dev);
				release_net(pneigh_net(n));
				kfree(n);
				continue;
			}
			np = &n->next;
		}
	}
	return -ENOENT;
}

static void neigh_parms_destroy(struct neigh_parms *parms);

static inline void neigh_parms_put(struct neigh_parms *parms)
{
	if (atomic_dec_and_test(&parms->refcnt))
		neigh_parms_destroy(parms);
}

void neigh_destroy(struct neighbour *neigh)
{
	struct net_device *dev = neigh->dev;

	NEIGH_CACHE_STAT_INC(neigh->tbl, destroys);

	if (!neigh->dead) {
		printk(KERN_WARNING
		       "Destroying alive neighbour %p\n", neigh);
		dump_stack();
		return;
	}

	if (neigh_del_timer(neigh))
		printk(KERN_WARNING "Impossible event.\n");

	skb_queue_purge(&neigh->arp_queue);
	neigh->arp_queue_len_bytes = 0;

	if (dev->netdev_ops->ndo_neigh_destroy)
		dev->netdev_ops->ndo_neigh_destroy(neigh);

	dev_put(dev);
	neigh_parms_put(neigh->parms);

	NEIGH_PRINTK2("neigh %p is destroyed.\n", neigh);

	atomic_dec(&neigh->tbl->entries);
	kfree_rcu(neigh, rcu);
}
EXPORT_SYMBOL(neigh_destroy);

static void neigh_suspect(struct neighbour *neigh)
{
	NEIGH_PRINTK2("neigh %p is suspected.\n", neigh);

	neigh->output = neigh->ops->output;
}

static void neigh_connect(struct neighbour *neigh)
{
	NEIGH_PRINTK2("neigh %p is connected.\n", neigh);

	neigh->output = neigh->ops->connected_output;
}

static void neigh_periodic_work(struct work_struct *work)
{
	struct neigh_table *tbl = container_of(work, struct neigh_table, gc_work.work);
	struct neighbour *n;
	struct neighbour __rcu **np;
	unsigned int i;
	struct neigh_hash_table *nht;

	NEIGH_CACHE_STAT_INC(tbl, periodic_gc_runs);

	write_lock_bh(&tbl->lock);
	nht = rcu_dereference_protected(tbl->nht,
					lockdep_is_held(&tbl->lock));


	if (time_after(jiffies, tbl->last_rand + 300 * HZ)) {
		struct neigh_parms *p;
		tbl->last_rand = jiffies;
		for (p = &tbl->parms; p; p = p->next)
			p->reachable_time =
				neigh_rand_reach_time(p->base_reachable_time);
	}

	for (i = 0 ; i < (1 << nht->hash_shift); i++) {
		np = &nht->hash_buckets[i];

		while ((n = rcu_dereference_protected(*np,
				lockdep_is_held(&tbl->lock))) != NULL) {
			unsigned int state;

			write_lock(&n->lock);

			state = n->nud_state;
			if (state & (NUD_PERMANENT | NUD_IN_TIMER)) {
				write_unlock(&n->lock);
				goto next_elt;
			}

			if (time_before(n->used, n->confirmed))
				n->used = n->confirmed;

			if (atomic_read(&n->refcnt) == 1 &&
			    (state == NUD_FAILED ||
			     time_after(jiffies, n->used + n->parms->gc_staletime))) {
				*np = n->next;
				n->dead = 1;
				write_unlock(&n->lock);
				neigh_cleanup_and_release(n);
				continue;
			}
			write_unlock(&n->lock);

next_elt:
			np = &n->next;
		}
		write_unlock_bh(&tbl->lock);
		cond_resched();
		write_lock_bh(&tbl->lock);
		nht = rcu_dereference_protected(tbl->nht,
						lockdep_is_held(&tbl->lock));
	}
	schedule_delayed_work(&tbl->gc_work,
			      tbl->parms.base_reachable_time >> 1);
	write_unlock_bh(&tbl->lock);
}

static __inline__ int neigh_max_probes(struct neighbour *n)
{
	struct neigh_parms *p = n->parms;
	return (n->nud_state & NUD_PROBE) ?
		p->ucast_probes :
		p->ucast_probes + p->app_probes + p->mcast_probes;
}

static void neigh_invalidate(struct neighbour *neigh)
	__releases(neigh->lock)
	__acquires(neigh->lock)
{
	struct sk_buff *skb;

	NEIGH_CACHE_STAT_INC(neigh->tbl, res_failed);
	NEIGH_PRINTK2("neigh %p is failed.\n", neigh);
	neigh->updated = jiffies;

	while (neigh->nud_state == NUD_FAILED &&
	       (skb = __skb_dequeue(&neigh->arp_queue)) != NULL) {
		write_unlock(&neigh->lock);
		neigh->ops->error_report(neigh, skb);
		write_lock(&neigh->lock);
	}
	skb_queue_purge(&neigh->arp_queue);
	neigh->arp_queue_len_bytes = 0;
}

static void neigh_probe(struct neighbour *neigh)
	__releases(neigh->lock)
{
	struct sk_buff *skb = skb_peek(&neigh->arp_queue);
	
	if (skb)
		skb = skb_copy(skb, GFP_ATOMIC);
	write_unlock(&neigh->lock);
	neigh->ops->solicit(neigh, skb);
	atomic_inc(&neigh->probes);
	kfree_skb(skb);
}


static void neigh_timer_handler(unsigned long arg)
{
	unsigned long now, next;
	struct neighbour *neigh = (struct neighbour *)arg;
	unsigned state;
	int notify = 0;

	write_lock(&neigh->lock);

	state = neigh->nud_state;
	now = jiffies;
	next = now + HZ;

	if (!(state & NUD_IN_TIMER))
		goto out;

	if (state & NUD_REACHABLE) {
		if (time_before_eq(now,
				   neigh->confirmed + neigh->parms->reachable_time)) {
			NEIGH_PRINTK2("neigh %p is still alive.\n", neigh);
			next = neigh->confirmed + neigh->parms->reachable_time;
		} else if (time_before_eq(now,
					  neigh->used + neigh->parms->delay_probe_time)) {
			NEIGH_PRINTK2("neigh %p is delayed.\n", neigh);
			neigh->nud_state = NUD_DELAY;
			neigh->updated = jiffies;
			neigh_suspect(neigh);
			next = now + neigh->parms->delay_probe_time;
		} else {
			NEIGH_PRINTK2("neigh %p is suspected.\n", neigh);
			neigh->nud_state = NUD_STALE;
			neigh->updated = jiffies;
			neigh_suspect(neigh);
			notify = 1;
		}
	} else if (state & NUD_DELAY) {
		if (time_before_eq(now,
				   neigh->confirmed + neigh->parms->delay_probe_time)) {
			NEIGH_PRINTK2("neigh %p is now reachable.\n", neigh);
			neigh->nud_state = NUD_REACHABLE;
			neigh->updated = jiffies;
			neigh_connect(neigh);
			notify = 1;
			next = neigh->confirmed + neigh->parms->reachable_time;
		} else {
			NEIGH_PRINTK2("neigh %p is probed.\n", neigh);
			neigh->nud_state = NUD_PROBE;
			neigh->updated = jiffies;
			atomic_set(&neigh->probes, 0);
			next = now + neigh->parms->retrans_time;
		}
	} else {
		
		next = now + neigh->parms->retrans_time;
	}

	if ((neigh->nud_state & (NUD_INCOMPLETE | NUD_PROBE)) &&
	    atomic_read(&neigh->probes) >= neigh_max_probes(neigh)) {
		neigh->nud_state = NUD_FAILED;
		notify = 1;
		neigh_invalidate(neigh);
	}

	if (neigh->nud_state & NUD_IN_TIMER) {
		if (time_before(next, jiffies + HZ/2))
			next = jiffies + HZ/2;
		if (!mod_timer(&neigh->timer, next))
			neigh_hold(neigh);
	}
	if (neigh->nud_state & (NUD_INCOMPLETE | NUD_PROBE)) {
		neigh_probe(neigh);
	} else {
out:
		write_unlock(&neigh->lock);
	}

	if (notify)
		neigh_update_notify(neigh);

	neigh_release(neigh);
}

int __neigh_event_send(struct neighbour *neigh, struct sk_buff *skb)
{
	int rc;
	bool immediate_probe = false;

	write_lock_bh(&neigh->lock);

	rc = 0;
	if (neigh->nud_state & (NUD_CONNECTED | NUD_DELAY | NUD_PROBE))
		goto out_unlock_bh;

	if (!(neigh->nud_state & (NUD_STALE | NUD_INCOMPLETE))) {
		if (neigh->parms->mcast_probes + neigh->parms->app_probes) {
			unsigned long next, now = jiffies;

			atomic_set(&neigh->probes, neigh->parms->ucast_probes);
			neigh->nud_state     = NUD_INCOMPLETE;
			neigh->updated = now;
			next = now + max(neigh->parms->retrans_time, HZ/2);
			neigh_add_timer(neigh, next);
			immediate_probe = true;
		} else {
			neigh->nud_state = NUD_FAILED;
			neigh->updated = jiffies;
			write_unlock_bh(&neigh->lock);

			kfree_skb(skb);
			return 1;
		}
	} else if (neigh->nud_state & NUD_STALE) {
		NEIGH_PRINTK2("neigh %p is delayed.\n", neigh);
		neigh->nud_state = NUD_DELAY;
		neigh->updated = jiffies;
		neigh_add_timer(neigh,
				jiffies + neigh->parms->delay_probe_time);
	}

	if (neigh->nud_state == NUD_INCOMPLETE) {
		if (skb) {
			while (neigh->arp_queue_len_bytes + skb->truesize >
			       neigh->parms->queue_len_bytes) {
				struct sk_buff *buff;

				buff = __skb_dequeue(&neigh->arp_queue);
				if (!buff)
					break;
				neigh->arp_queue_len_bytes -= buff->truesize;
				kfree_skb(buff);
				NEIGH_CACHE_STAT_INC(neigh->tbl, unres_discards);
			}
			skb_dst_force(skb);
			__skb_queue_tail(&neigh->arp_queue, skb);
			neigh->arp_queue_len_bytes += skb->truesize;
		}
		rc = 1;
	}
out_unlock_bh:
	if (immediate_probe)
		neigh_probe(neigh);
	else
		write_unlock(&neigh->lock);
	local_bh_enable();
	return rc;
}
EXPORT_SYMBOL(__neigh_event_send);

static void neigh_update_hhs(struct neighbour *neigh)
{
	struct hh_cache *hh;
	void (*update)(struct hh_cache*, const struct net_device*, const unsigned char *)
		= NULL;

	if (neigh->dev->header_ops)
		update = neigh->dev->header_ops->cache_update;

	if (update) {
		hh = &neigh->hh;
		if (hh->hh_len) {
			write_seqlock_bh(&hh->hh_lock);
			update(hh, neigh->dev, neigh->ha);
			write_sequnlock_bh(&hh->hh_lock);
		}
	}
}




int neigh_update(struct neighbour *neigh, const u8 *lladdr, u8 new,
		 u32 flags)
{
	u8 old;
	int err;
	int notify = 0;
	struct net_device *dev;
	int update_isrouter = 0;

	write_lock_bh(&neigh->lock);

	dev    = neigh->dev;
	old    = neigh->nud_state;
	err    = -EPERM;

	if (!(flags & NEIGH_UPDATE_F_ADMIN) &&
	    (old & (NUD_NOARP | NUD_PERMANENT)))
		goto out;

	if (!(new & NUD_VALID)) {
		neigh_del_timer(neigh);
		if (old & NUD_CONNECTED)
			neigh_suspect(neigh);
		neigh->nud_state = new;
		err = 0;
		notify = old & NUD_VALID;
		if ((old & (NUD_INCOMPLETE | NUD_PROBE)) &&
		    (new & NUD_FAILED)) {
			neigh_invalidate(neigh);
			notify = 1;
		}
		goto out;
	}

	
	if (!dev->addr_len) {
		
		lladdr = neigh->ha;
	} else if (lladdr) {
		if ((old & NUD_VALID) &&
		    !memcmp(lladdr, neigh->ha, dev->addr_len))
			lladdr = neigh->ha;
	} else {
		err = -EINVAL;
		if (!(old & NUD_VALID))
			goto out;
		lladdr = neigh->ha;
	}

	if (new & NUD_CONNECTED)
		neigh->confirmed = jiffies;
	neigh->updated = jiffies;

	err = 0;
	update_isrouter = flags & NEIGH_UPDATE_F_OVERRIDE_ISROUTER;
	if (old & NUD_VALID) {
		if (lladdr != neigh->ha && !(flags & NEIGH_UPDATE_F_OVERRIDE)) {
			update_isrouter = 0;
			if ((flags & NEIGH_UPDATE_F_WEAK_OVERRIDE) &&
			    (old & NUD_CONNECTED)) {
				lladdr = neigh->ha;
				new = NUD_STALE;
			} else
				goto out;
		} else {
			if (lladdr == neigh->ha && new == NUD_STALE &&
			    ((flags & NEIGH_UPDATE_F_WEAK_OVERRIDE) ||
			     (old & NUD_CONNECTED))
			    )
				new = old;
		}
	}

	if (new != old) {
		neigh_del_timer(neigh);
		if (new & NUD_IN_TIMER)
			neigh_add_timer(neigh, (jiffies +
						((new & NUD_REACHABLE) ?
						 neigh->parms->reachable_time :
						 0)));
		neigh->nud_state = new;
	}

	if (lladdr != neigh->ha) {
		write_seqlock(&neigh->ha_lock);
		memcpy(&neigh->ha, lladdr, dev->addr_len);
		write_sequnlock(&neigh->ha_lock);
		neigh_update_hhs(neigh);
		if (!(new & NUD_CONNECTED))
			neigh->confirmed = jiffies -
				      (neigh->parms->base_reachable_time << 1);
		notify = 1;
	}
	if (new == old)
		goto out;
	if (new & NUD_CONNECTED)
		neigh_connect(neigh);
	else
		neigh_suspect(neigh);
	if (!(old & NUD_VALID)) {
		struct sk_buff *skb;

		

		while (neigh->nud_state & NUD_VALID &&
		       (skb = __skb_dequeue(&neigh->arp_queue)) != NULL) {
			struct dst_entry *dst = skb_dst(skb);
			struct neighbour *n2, *n1 = neigh;
			write_unlock_bh(&neigh->lock);

			rcu_read_lock();
			
			if (dst && (n2 = dst_get_neighbour_noref(dst)) != NULL)
				n1 = n2;
			n1->output(n1, skb);
			rcu_read_unlock();

			write_lock_bh(&neigh->lock);
		}
		skb_queue_purge(&neigh->arp_queue);
		neigh->arp_queue_len_bytes = 0;
	}
out:
	if (update_isrouter) {
		neigh->flags = (flags & NEIGH_UPDATE_F_ISROUTER) ?
			(neigh->flags | NTF_ROUTER) :
			(neigh->flags & ~NTF_ROUTER);
	}
	write_unlock_bh(&neigh->lock);

	if (notify)
		neigh_update_notify(neigh);

	return err;
}
EXPORT_SYMBOL(neigh_update);

struct neighbour *neigh_event_ns(struct neigh_table *tbl,
				 u8 *lladdr, void *saddr,
				 struct net_device *dev)
{
	struct neighbour *neigh = __neigh_lookup(tbl, saddr, dev,
						 lladdr || !dev->addr_len);
	if (neigh)
		neigh_update(neigh, lladdr, NUD_STALE,
			     NEIGH_UPDATE_F_OVERRIDE);
	return neigh;
}
EXPORT_SYMBOL(neigh_event_ns);

static void neigh_hh_init(struct neighbour *n, struct dst_entry *dst)
{
	struct net_device *dev = dst->dev;
	__be16 prot = dst->ops->protocol;
	struct hh_cache	*hh = &n->hh;

	write_lock_bh(&n->lock);

	if (!hh->hh_len)
		dev->header_ops->cache(n, hh, prot);

	write_unlock_bh(&n->lock);
}


int neigh_compat_output(struct neighbour *neigh, struct sk_buff *skb)
{
	struct net_device *dev = skb->dev;

	__skb_pull(skb, skb_network_offset(skb));

	if (dev_hard_header(skb, dev, ntohs(skb->protocol), NULL, NULL,
			    skb->len) < 0 &&
	    dev->header_ops->rebuild(skb))
		return 0;

	return dev_queue_xmit(skb);
}
EXPORT_SYMBOL(neigh_compat_output);


int neigh_resolve_output(struct neighbour *neigh, struct sk_buff *skb)
{
	struct dst_entry *dst = skb_dst(skb);
	int rc = 0;

	if (!dst)
		goto discard;

	__skb_pull(skb, skb_network_offset(skb));

	if (!neigh_event_send(neigh, skb)) {
		int err;
		struct net_device *dev = neigh->dev;
		unsigned int seq;

		if (dev->header_ops->cache && !neigh->hh.hh_len)
			neigh_hh_init(neigh, dst);

		do {
			seq = read_seqbegin(&neigh->ha_lock);
			err = dev_hard_header(skb, dev, ntohs(skb->protocol),
					      neigh->ha, NULL, skb->len);
		} while (read_seqretry(&neigh->ha_lock, seq));

		if (err >= 0)
			rc = dev_queue_xmit(skb);
		else
			goto out_kfree_skb;
	}
out:
	return rc;
discard:
	NEIGH_PRINTK1("neigh_resolve_output: dst=%p neigh=%p\n",
		      dst, neigh);
out_kfree_skb:
	rc = -EINVAL;
	kfree_skb(skb);
	goto out;
}
EXPORT_SYMBOL(neigh_resolve_output);


int neigh_connected_output(struct neighbour *neigh, struct sk_buff *skb)
{
	struct net_device *dev = neigh->dev;
	unsigned int seq;
	int err;

	__skb_pull(skb, skb_network_offset(skb));

	do {
		seq = read_seqbegin(&neigh->ha_lock);
		err = dev_hard_header(skb, dev, ntohs(skb->protocol),
				      neigh->ha, NULL, skb->len);
	} while (read_seqretry(&neigh->ha_lock, seq));

	if (err >= 0)
		err = dev_queue_xmit(skb);
	else {
		err = -EINVAL;
		kfree_skb(skb);
	}
	return err;
}
EXPORT_SYMBOL(neigh_connected_output);

int neigh_direct_output(struct neighbour *neigh, struct sk_buff *skb)
{
	return dev_queue_xmit(skb);
}
EXPORT_SYMBOL(neigh_direct_output);

static void neigh_proxy_process(unsigned long arg)
{
	struct neigh_table *tbl = (struct neigh_table *)arg;
	long sched_next = 0;
	unsigned long now = jiffies;
	struct sk_buff *skb, *n;

	spin_lock(&tbl->proxy_queue.lock);

	skb_queue_walk_safe(&tbl->proxy_queue, skb, n) {
		long tdif = NEIGH_CB(skb)->sched_next - now;

		if (tdif <= 0) {
			struct net_device *dev = skb->dev;

			__skb_unlink(skb, &tbl->proxy_queue);
			if (tbl->proxy_redo && netif_running(dev)) {
				rcu_read_lock();
				tbl->proxy_redo(skb);
				rcu_read_unlock();
			} else {
				kfree_skb(skb);
			}

			dev_put(dev);
		} else if (!sched_next || tdif < sched_next)
			sched_next = tdif;
	}
	del_timer(&tbl->proxy_timer);
	if (sched_next)
		mod_timer(&tbl->proxy_timer, jiffies + sched_next);
	spin_unlock(&tbl->proxy_queue.lock);
}

void pneigh_enqueue(struct neigh_table *tbl, struct neigh_parms *p,
		    struct sk_buff *skb)
{
	unsigned long now = jiffies;
	unsigned long sched_next = now + (net_random() % p->proxy_delay);

	if (tbl->proxy_queue.qlen > p->proxy_qlen) {
		kfree_skb(skb);
		return;
	}

	NEIGH_CB(skb)->sched_next = sched_next;
	NEIGH_CB(skb)->flags |= LOCALLY_ENQUEUED;

	spin_lock(&tbl->proxy_queue.lock);
	if (del_timer(&tbl->proxy_timer)) {
		if (time_before(tbl->proxy_timer.expires, sched_next))
			sched_next = tbl->proxy_timer.expires;
	}
	skb_dst_drop(skb);
	dev_hold(skb->dev);
	__skb_queue_tail(&tbl->proxy_queue, skb);
	mod_timer(&tbl->proxy_timer, sched_next);
	spin_unlock(&tbl->proxy_queue.lock);
}
EXPORT_SYMBOL(pneigh_enqueue);

static inline struct neigh_parms *lookup_neigh_parms(struct neigh_table *tbl,
						      struct net *net, int ifindex)
{
	struct neigh_parms *p;

	for (p = &tbl->parms; p; p = p->next) {
		if ((p->dev && p->dev->ifindex == ifindex && net_eq(neigh_parms_net(p), net)) ||
		    (!p->dev && !ifindex))
			return p;
	}

	return NULL;
}

struct neigh_parms *neigh_parms_alloc(struct net_device *dev,
				      struct neigh_table *tbl)
{
	struct neigh_parms *p, *ref;
	struct net *net = dev_net(dev);
	const struct net_device_ops *ops = dev->netdev_ops;

	ref = lookup_neigh_parms(tbl, net, 0);
	if (!ref)
		return NULL;

	p = kmemdup(ref, sizeof(*p), GFP_KERNEL);
	if (p) {
		p->tbl		  = tbl;
		atomic_set(&p->refcnt, 1);
		p->reachable_time =
				neigh_rand_reach_time(p->base_reachable_time);

		if (ops->ndo_neigh_setup && ops->ndo_neigh_setup(dev, p)) {
			kfree(p);
			return NULL;
		}

		dev_hold(dev);
		p->dev = dev;
		write_pnet(&p->net, hold_net(net));
		p->sysctl_table = NULL;
		write_lock_bh(&tbl->lock);
		p->next		= tbl->parms.next;
		tbl->parms.next = p;
		write_unlock_bh(&tbl->lock);
	}
	return p;
}
EXPORT_SYMBOL(neigh_parms_alloc);

static void neigh_rcu_free_parms(struct rcu_head *head)
{
	struct neigh_parms *parms =
		container_of(head, struct neigh_parms, rcu_head);

	neigh_parms_put(parms);
}

void neigh_parms_release(struct neigh_table *tbl, struct neigh_parms *parms)
{
	struct neigh_parms **p;

	if (!parms || parms == &tbl->parms)
		return;
	write_lock_bh(&tbl->lock);
	for (p = &tbl->parms.next; *p; p = &(*p)->next) {
		if (*p == parms) {
			*p = parms->next;
			parms->dead = 1;
			write_unlock_bh(&tbl->lock);
			if (parms->dev)
				dev_put(parms->dev);
			call_rcu(&parms->rcu_head, neigh_rcu_free_parms);
			return;
		}
	}
	write_unlock_bh(&tbl->lock);
	NEIGH_PRINTK1("neigh_parms_release: not found\n");
}
EXPORT_SYMBOL(neigh_parms_release);

static void neigh_parms_destroy(struct neigh_parms *parms)
{
	release_net(neigh_parms_net(parms));
	kfree(parms);
}

static struct lock_class_key neigh_table_proxy_queue_class;

void neigh_table_init_no_netlink(struct neigh_table *tbl)
{
	unsigned long now = jiffies;
	unsigned long phsize;

	write_pnet(&tbl->parms.net, &init_net);
	atomic_set(&tbl->parms.refcnt, 1);
	tbl->parms.reachable_time =
			  neigh_rand_reach_time(tbl->parms.base_reachable_time);

	tbl->stats = alloc_percpu(struct neigh_statistics);
	if (!tbl->stats)
		panic("cannot create neighbour cache statistics");

#ifdef CONFIG_PROC_FS
	if (!proc_create_data(tbl->id, 0, init_net.proc_net_stat,
			      &neigh_stat_seq_fops, tbl))
		panic("cannot create neighbour proc dir entry");
#endif

	RCU_INIT_POINTER(tbl->nht, neigh_hash_alloc(3));

	phsize = (PNEIGH_HASHMASK + 1) * sizeof(struct pneigh_entry *);
	tbl->phash_buckets = kzalloc(phsize, GFP_KERNEL);

	if (!tbl->nht || !tbl->phash_buckets)
		panic("cannot allocate neighbour cache hashes");

	rwlock_init(&tbl->lock);
	INIT_DELAYED_WORK_DEFERRABLE(&tbl->gc_work, neigh_periodic_work);
	schedule_delayed_work(&tbl->gc_work, tbl->parms.reachable_time);
	setup_timer(&tbl->proxy_timer, neigh_proxy_process, (unsigned long)tbl);
	skb_queue_head_init_class(&tbl->proxy_queue,
			&neigh_table_proxy_queue_class);

	tbl->last_flush = now;
	tbl->last_rand	= now + tbl->parms.reachable_time * 20;
}
EXPORT_SYMBOL(neigh_table_init_no_netlink);

void neigh_table_init(struct neigh_table *tbl)
{
	struct neigh_table *tmp;

	neigh_table_init_no_netlink(tbl);
	write_lock(&neigh_tbl_lock);
	for (tmp = neigh_tables; tmp; tmp = tmp->next) {
		if (tmp->family == tbl->family)
			break;
	}
	tbl->next	= neigh_tables;
	neigh_tables	= tbl;
	write_unlock(&neigh_tbl_lock);

	if (unlikely(tmp)) {
		printk(KERN_ERR "NEIGH: Registering multiple tables for "
		       "family %d\n", tbl->family);
		dump_stack();
	}
}
EXPORT_SYMBOL(neigh_table_init);

int neigh_table_clear(struct neigh_table *tbl)
{
	struct neigh_table **tp;

	
	cancel_delayed_work_sync(&tbl->gc_work);
	del_timer_sync(&tbl->proxy_timer);
	pneigh_queue_purge(&tbl->proxy_queue);
	neigh_ifdown(tbl, NULL);
	if (atomic_read(&tbl->entries))
		printk(KERN_CRIT "neighbour leakage\n");
	write_lock(&neigh_tbl_lock);
	for (tp = &neigh_tables; *tp; tp = &(*tp)->next) {
		if (*tp == tbl) {
			*tp = tbl->next;
			break;
		}
	}
	write_unlock(&neigh_tbl_lock);

	call_rcu(&rcu_dereference_protected(tbl->nht, 1)->rcu,
		 neigh_hash_free_rcu);
	tbl->nht = NULL;

	kfree(tbl->phash_buckets);
	tbl->phash_buckets = NULL;

	remove_proc_entry(tbl->id, init_net.proc_net_stat);

	free_percpu(tbl->stats);
	tbl->stats = NULL;

	return 0;
}
EXPORT_SYMBOL(neigh_table_clear);

static int neigh_delete(struct sk_buff *skb, struct nlmsghdr *nlh, void *arg)
{
	struct net *net = sock_net(skb->sk);
	struct ndmsg *ndm;
	struct nlattr *dst_attr;
	struct neigh_table *tbl;
	struct net_device *dev = NULL;
	int err = -EINVAL;

	ASSERT_RTNL();
	if (nlmsg_len(nlh) < sizeof(*ndm))
		goto out;

	dst_attr = nlmsg_find_attr(nlh, sizeof(*ndm), NDA_DST);
	if (dst_attr == NULL)
		goto out;

	ndm = nlmsg_data(nlh);
	if (ndm->ndm_ifindex) {
		dev = __dev_get_by_index(net, ndm->ndm_ifindex);
		if (dev == NULL) {
			err = -ENODEV;
			goto out;
		}
	}

	read_lock(&neigh_tbl_lock);
	for (tbl = neigh_tables; tbl; tbl = tbl->next) {
		struct neighbour *neigh;

		if (tbl->family != ndm->ndm_family)
			continue;
		read_unlock(&neigh_tbl_lock);

		if (nla_len(dst_attr) < tbl->key_len)
			goto out;

		if (ndm->ndm_flags & NTF_PROXY) {
			err = pneigh_delete(tbl, net, nla_data(dst_attr), dev);
			goto out;
		}

		if (dev == NULL)
			goto out;

		neigh = neigh_lookup(tbl, nla_data(dst_attr), dev);
		if (neigh == NULL) {
			err = -ENOENT;
			goto out;
		}

		err = neigh_update(neigh, NULL, NUD_FAILED,
				   NEIGH_UPDATE_F_OVERRIDE |
				   NEIGH_UPDATE_F_ADMIN);
		neigh_release(neigh);
		goto out;
	}
	read_unlock(&neigh_tbl_lock);
	err = -EAFNOSUPPORT;

out:
	return err;
}

static int neigh_add(struct sk_buff *skb, struct nlmsghdr *nlh, void *arg)
{
	struct net *net = sock_net(skb->sk);
	struct ndmsg *ndm;
	struct nlattr *tb[NDA_MAX+1];
	struct neigh_table *tbl;
	struct net_device *dev = NULL;
	int err;

	ASSERT_RTNL();
	err = nlmsg_parse(nlh, sizeof(*ndm), tb, NDA_MAX, NULL);
	if (err < 0)
		goto out;

	err = -EINVAL;
	if (tb[NDA_DST] == NULL)
		goto out;

	ndm = nlmsg_data(nlh);
	if (ndm->ndm_ifindex) {
		dev = __dev_get_by_index(net, ndm->ndm_ifindex);
		if (dev == NULL) {
			err = -ENODEV;
			goto out;
		}

		if (tb[NDA_LLADDR] && nla_len(tb[NDA_LLADDR]) < dev->addr_len)
			goto out;
	}

	read_lock(&neigh_tbl_lock);
	for (tbl = neigh_tables; tbl; tbl = tbl->next) {
		int flags = NEIGH_UPDATE_F_ADMIN | NEIGH_UPDATE_F_OVERRIDE;
		struct neighbour *neigh;
		void *dst, *lladdr;

		if (tbl->family != ndm->ndm_family)
			continue;
		read_unlock(&neigh_tbl_lock);

		if (nla_len(tb[NDA_DST]) < tbl->key_len)
			goto out;
		dst = nla_data(tb[NDA_DST]);
		lladdr = tb[NDA_LLADDR] ? nla_data(tb[NDA_LLADDR]) : NULL;

		if (ndm->ndm_flags & NTF_PROXY) {
			struct pneigh_entry *pn;

			err = -ENOBUFS;
			pn = pneigh_lookup(tbl, net, dst, dev, 1);
			if (pn) {
				pn->flags = ndm->ndm_flags;
				err = 0;
			}
			goto out;
		}

		if (dev == NULL)
			goto out;

		neigh = neigh_lookup(tbl, dst, dev);
		if (neigh == NULL) {
			if (!(nlh->nlmsg_flags & NLM_F_CREATE)) {
				err = -ENOENT;
				goto out;
			}

			neigh = __neigh_lookup_errno(tbl, dst, dev);
			if (IS_ERR(neigh)) {
				err = PTR_ERR(neigh);
				goto out;
			}
		} else {
			if (nlh->nlmsg_flags & NLM_F_EXCL) {
				err = -EEXIST;
				neigh_release(neigh);
				goto out;
			}

			if (!(nlh->nlmsg_flags & NLM_F_REPLACE))
				flags &= ~NEIGH_UPDATE_F_OVERRIDE;
		}

		if (ndm->ndm_flags & NTF_USE) {
			neigh_event_send(neigh, NULL);
			err = 0;
		} else
			err = neigh_update(neigh, lladdr, ndm->ndm_state, flags);
		neigh_release(neigh);
		goto out;
	}

	read_unlock(&neigh_tbl_lock);
	err = -EAFNOSUPPORT;
out:
	return err;
}

static int neightbl_fill_parms(struct sk_buff *skb, struct neigh_parms *parms)
{
	struct nlattr *nest;

	nest = nla_nest_start(skb, NDTA_PARMS);
	if (nest == NULL)
		return -ENOBUFS;

	if (parms->dev)
		NLA_PUT_U32(skb, NDTPA_IFINDEX, parms->dev->ifindex);

	NLA_PUT_U32(skb, NDTPA_REFCNT, atomic_read(&parms->refcnt));
	NLA_PUT_U32(skb, NDTPA_QUEUE_LENBYTES, parms->queue_len_bytes);
	
	NLA_PUT_U32(skb, NDTPA_QUEUE_LEN,
		    DIV_ROUND_UP(parms->queue_len_bytes,
				 SKB_TRUESIZE(ETH_FRAME_LEN)));
	NLA_PUT_U32(skb, NDTPA_PROXY_QLEN, parms->proxy_qlen);
	NLA_PUT_U32(skb, NDTPA_APP_PROBES, parms->app_probes);
	NLA_PUT_U32(skb, NDTPA_UCAST_PROBES, parms->ucast_probes);
	NLA_PUT_U32(skb, NDTPA_MCAST_PROBES, parms->mcast_probes);
	NLA_PUT_MSECS(skb, NDTPA_REACHABLE_TIME, parms->reachable_time);
	NLA_PUT_MSECS(skb, NDTPA_BASE_REACHABLE_TIME,
		      parms->base_reachable_time);
	NLA_PUT_MSECS(skb, NDTPA_GC_STALETIME, parms->gc_staletime);
	NLA_PUT_MSECS(skb, NDTPA_DELAY_PROBE_TIME, parms->delay_probe_time);
	NLA_PUT_MSECS(skb, NDTPA_RETRANS_TIME, parms->retrans_time);
	NLA_PUT_MSECS(skb, NDTPA_ANYCAST_DELAY, parms->anycast_delay);
	NLA_PUT_MSECS(skb, NDTPA_PROXY_DELAY, parms->proxy_delay);
	NLA_PUT_MSECS(skb, NDTPA_LOCKTIME, parms->locktime);

	return nla_nest_end(skb, nest);

nla_put_failure:
	nla_nest_cancel(skb, nest);
	return -EMSGSIZE;
}

static int neightbl_fill_info(struct sk_buff *skb, struct neigh_table *tbl,
			      u32 pid, u32 seq, int type, int flags)
{
	struct nlmsghdr *nlh;
	struct ndtmsg *ndtmsg;

	nlh = nlmsg_put(skb, pid, seq, type, sizeof(*ndtmsg), flags);
	if (nlh == NULL)
		return -EMSGSIZE;

	ndtmsg = nlmsg_data(nlh);

	read_lock_bh(&tbl->lock);
	ndtmsg->ndtm_family = tbl->family;
	ndtmsg->ndtm_pad1   = 0;
	ndtmsg->ndtm_pad2   = 0;

	NLA_PUT_STRING(skb, NDTA_NAME, tbl->id);
	NLA_PUT_MSECS(skb, NDTA_GC_INTERVAL, tbl->gc_interval);
	NLA_PUT_U32(skb, NDTA_THRESH1, tbl->gc_thresh1);
	NLA_PUT_U32(skb, NDTA_THRESH2, tbl->gc_thresh2);
	NLA_PUT_U32(skb, NDTA_THRESH3, tbl->gc_thresh3);

	{
		unsigned long now = jiffies;
		unsigned int flush_delta = now - tbl->last_flush;
		unsigned int rand_delta = now - tbl->last_rand;
		struct neigh_hash_table *nht;
		struct ndt_config ndc = {
			.ndtc_key_len		= tbl->key_len,
			.ndtc_entry_size	= tbl->entry_size,
			.ndtc_entries		= atomic_read(&tbl->entries),
			.ndtc_last_flush	= jiffies_to_msecs(flush_delta),
			.ndtc_last_rand		= jiffies_to_msecs(rand_delta),
			.ndtc_proxy_qlen	= tbl->proxy_queue.qlen,
		};

		rcu_read_lock_bh();
		nht = rcu_dereference_bh(tbl->nht);
		ndc.ndtc_hash_rnd = nht->hash_rnd[0];
		ndc.ndtc_hash_mask = ((1 << nht->hash_shift) - 1);
		rcu_read_unlock_bh();

		NLA_PUT(skb, NDTA_CONFIG, sizeof(ndc), &ndc);
	}

	{
		int cpu;
		struct ndt_stats ndst;

		memset(&ndst, 0, sizeof(ndst));

		for_each_possible_cpu(cpu) {
			struct neigh_statistics	*st;

			st = per_cpu_ptr(tbl->stats, cpu);
			ndst.ndts_allocs		+= st->allocs;
			ndst.ndts_destroys		+= st->destroys;
			ndst.ndts_hash_grows		+= st->hash_grows;
			ndst.ndts_res_failed		+= st->res_failed;
			ndst.ndts_lookups		+= st->lookups;
			ndst.ndts_hits			+= st->hits;
			ndst.ndts_rcv_probes_mcast	+= st->rcv_probes_mcast;
			ndst.ndts_rcv_probes_ucast	+= st->rcv_probes_ucast;
			ndst.ndts_periodic_gc_runs	+= st->periodic_gc_runs;
			ndst.ndts_forced_gc_runs	+= st->forced_gc_runs;
		}

		NLA_PUT(skb, NDTA_STATS, sizeof(ndst), &ndst);
	}

	BUG_ON(tbl->parms.dev);
	if (neightbl_fill_parms(skb, &tbl->parms) < 0)
		goto nla_put_failure;

	read_unlock_bh(&tbl->lock);
	return nlmsg_end(skb, nlh);

nla_put_failure:
	read_unlock_bh(&tbl->lock);
	nlmsg_cancel(skb, nlh);
	return -EMSGSIZE;
}

static int neightbl_fill_param_info(struct sk_buff *skb,
				    struct neigh_table *tbl,
				    struct neigh_parms *parms,
				    u32 pid, u32 seq, int type,
				    unsigned int flags)
{
	struct ndtmsg *ndtmsg;
	struct nlmsghdr *nlh;

	nlh = nlmsg_put(skb, pid, seq, type, sizeof(*ndtmsg), flags);
	if (nlh == NULL)
		return -EMSGSIZE;

	ndtmsg = nlmsg_data(nlh);

	read_lock_bh(&tbl->lock);
	ndtmsg->ndtm_family = tbl->family;
	ndtmsg->ndtm_pad1   = 0;
	ndtmsg->ndtm_pad2   = 0;

	if (nla_put_string(skb, NDTA_NAME, tbl->id) < 0 ||
	    neightbl_fill_parms(skb, parms) < 0)
		goto errout;

	read_unlock_bh(&tbl->lock);
	return nlmsg_end(skb, nlh);
errout:
	read_unlock_bh(&tbl->lock);
	nlmsg_cancel(skb, nlh);
	return -EMSGSIZE;
}

static const struct nla_policy nl_neightbl_policy[NDTA_MAX+1] = {
	[NDTA_NAME]		= { .type = NLA_STRING },
	[NDTA_THRESH1]		= { .type = NLA_U32 },
	[NDTA_THRESH2]		= { .type = NLA_U32 },
	[NDTA_THRESH3]		= { .type = NLA_U32 },
	[NDTA_GC_INTERVAL]	= { .type = NLA_U64 },
	[NDTA_PARMS]		= { .type = NLA_NESTED },
};

static const struct nla_policy nl_ntbl_parm_policy[NDTPA_MAX+1] = {
	[NDTPA_IFINDEX]			= { .type = NLA_U32 },
	[NDTPA_QUEUE_LEN]		= { .type = NLA_U32 },
	[NDTPA_PROXY_QLEN]		= { .type = NLA_U32 },
	[NDTPA_APP_PROBES]		= { .type = NLA_U32 },
	[NDTPA_UCAST_PROBES]		= { .type = NLA_U32 },
	[NDTPA_MCAST_PROBES]		= { .type = NLA_U32 },
	[NDTPA_BASE_REACHABLE_TIME]	= { .type = NLA_U64 },
	[NDTPA_GC_STALETIME]		= { .type = NLA_U64 },
	[NDTPA_DELAY_PROBE_TIME]	= { .type = NLA_U64 },
	[NDTPA_RETRANS_TIME]		= { .type = NLA_U64 },
	[NDTPA_ANYCAST_DELAY]		= { .type = NLA_U64 },
	[NDTPA_PROXY_DELAY]		= { .type = NLA_U64 },
	[NDTPA_LOCKTIME]		= { .type = NLA_U64 },
};

static int neightbl_set(struct sk_buff *skb, struct nlmsghdr *nlh, void *arg)
{
	struct net *net = sock_net(skb->sk);
	struct neigh_table *tbl;
	struct ndtmsg *ndtmsg;
	struct nlattr *tb[NDTA_MAX+1];
	int err;

	err = nlmsg_parse(nlh, sizeof(*ndtmsg), tb, NDTA_MAX,
			  nl_neightbl_policy);
	if (err < 0)
		goto errout;

	if (tb[NDTA_NAME] == NULL) {
		err = -EINVAL;
		goto errout;
	}

	ndtmsg = nlmsg_data(nlh);
	read_lock(&neigh_tbl_lock);
	for (tbl = neigh_tables; tbl; tbl = tbl->next) {
		if (ndtmsg->ndtm_family && tbl->family != ndtmsg->ndtm_family)
			continue;

		if (nla_strcmp(tb[NDTA_NAME], tbl->id) == 0)
			break;
	}

	if (tbl == NULL) {
		err = -ENOENT;
		goto errout_locked;
	}

	write_lock_bh(&tbl->lock);

	if (tb[NDTA_PARMS]) {
		struct nlattr *tbp[NDTPA_MAX+1];
		struct neigh_parms *p;
		int i, ifindex = 0;

		err = nla_parse_nested(tbp, NDTPA_MAX, tb[NDTA_PARMS],
				       nl_ntbl_parm_policy);
		if (err < 0)
			goto errout_tbl_lock;

		if (tbp[NDTPA_IFINDEX])
			ifindex = nla_get_u32(tbp[NDTPA_IFINDEX]);

		p = lookup_neigh_parms(tbl, net, ifindex);
		if (p == NULL) {
			err = -ENOENT;
			goto errout_tbl_lock;
		}

		for (i = 1; i <= NDTPA_MAX; i++) {
			if (tbp[i] == NULL)
				continue;

			switch (i) {
			case NDTPA_QUEUE_LEN:
				p->queue_len_bytes = nla_get_u32(tbp[i]) *
						     SKB_TRUESIZE(ETH_FRAME_LEN);
				break;
			case NDTPA_QUEUE_LENBYTES:
				p->queue_len_bytes = nla_get_u32(tbp[i]);
				break;
			case NDTPA_PROXY_QLEN:
				p->proxy_qlen = nla_get_u32(tbp[i]);
				break;
			case NDTPA_APP_PROBES:
				p->app_probes = nla_get_u32(tbp[i]);
				break;
			case NDTPA_UCAST_PROBES:
				p->ucast_probes = nla_get_u32(tbp[i]);
				break;
			case NDTPA_MCAST_PROBES:
				p->mcast_probes = nla_get_u32(tbp[i]);
				break;
			case NDTPA_BASE_REACHABLE_TIME:
				p->base_reachable_time = nla_get_msecs(tbp[i]);
				break;
			case NDTPA_GC_STALETIME:
				p->gc_staletime = nla_get_msecs(tbp[i]);
				break;
			case NDTPA_DELAY_PROBE_TIME:
				p->delay_probe_time = nla_get_msecs(tbp[i]);
				break;
			case NDTPA_RETRANS_TIME:
				p->retrans_time = nla_get_msecs(tbp[i]);
				break;
			case NDTPA_ANYCAST_DELAY:
				p->anycast_delay = nla_get_msecs(tbp[i]);
				break;
			case NDTPA_PROXY_DELAY:
				p->proxy_delay = nla_get_msecs(tbp[i]);
				break;
			case NDTPA_LOCKTIME:
				p->locktime = nla_get_msecs(tbp[i]);
				break;
			}
		}
	}

	if (tb[NDTA_THRESH1])
		tbl->gc_thresh1 = nla_get_u32(tb[NDTA_THRESH1]);

	if (tb[NDTA_THRESH2])
		tbl->gc_thresh2 = nla_get_u32(tb[NDTA_THRESH2]);

	if (tb[NDTA_THRESH3])
		tbl->gc_thresh3 = nla_get_u32(tb[NDTA_THRESH3]);

	if (tb[NDTA_GC_INTERVAL])
		tbl->gc_interval = nla_get_msecs(tb[NDTA_GC_INTERVAL]);

	err = 0;

errout_tbl_lock:
	write_unlock_bh(&tbl->lock);
errout_locked:
	read_unlock(&neigh_tbl_lock);
errout:
	return err;
}

static int neightbl_dump_info(struct sk_buff *skb, struct netlink_callback *cb)
{
	struct net *net = sock_net(skb->sk);
	int family, tidx, nidx = 0;
	int tbl_skip = cb->args[0];
	int neigh_skip = cb->args[1];
	struct neigh_table *tbl;

	family = ((struct rtgenmsg *) nlmsg_data(cb->nlh))->rtgen_family;

	read_lock(&neigh_tbl_lock);
	for (tbl = neigh_tables, tidx = 0; tbl; tbl = tbl->next, tidx++) {
		struct neigh_parms *p;

		if (tidx < tbl_skip || (family && tbl->family != family))
			continue;

		if (neightbl_fill_info(skb, tbl, NETLINK_CB(cb->skb).pid,
				       cb->nlh->nlmsg_seq, RTM_NEWNEIGHTBL,
				       NLM_F_MULTI) <= 0)
			break;

		for (nidx = 0, p = tbl->parms.next; p; p = p->next) {
			if (!net_eq(neigh_parms_net(p), net))
				continue;

			if (nidx < neigh_skip)
				goto next;

			if (neightbl_fill_param_info(skb, tbl, p,
						     NETLINK_CB(cb->skb).pid,
						     cb->nlh->nlmsg_seq,
						     RTM_NEWNEIGHTBL,
						     NLM_F_MULTI) <= 0)
				goto out;
		next:
			nidx++;
		}

		neigh_skip = 0;
	}
out:
	read_unlock(&neigh_tbl_lock);
	cb->args[0] = tidx;
	cb->args[1] = nidx;

	return skb->len;
}

static int neigh_fill_info(struct sk_buff *skb, struct neighbour *neigh,
			   u32 pid, u32 seq, int type, unsigned int flags)
{
	unsigned long now = jiffies;
	struct nda_cacheinfo ci;
	struct nlmsghdr *nlh;
	struct ndmsg *ndm;

	nlh = nlmsg_put(skb, pid, seq, type, sizeof(*ndm), flags);
	if (nlh == NULL)
		return -EMSGSIZE;

	ndm = nlmsg_data(nlh);
	ndm->ndm_family	 = neigh->ops->family;
	ndm->ndm_pad1    = 0;
	ndm->ndm_pad2    = 0;
	ndm->ndm_flags	 = neigh->flags;
	ndm->ndm_type	 = neigh->type;
	ndm->ndm_ifindex = neigh->dev->ifindex;

	NLA_PUT(skb, NDA_DST, neigh->tbl->key_len, neigh->primary_key);

	read_lock_bh(&neigh->lock);
	ndm->ndm_state	 = neigh->nud_state;
	if (neigh->nud_state & NUD_VALID) {
		char haddr[MAX_ADDR_LEN];

		neigh_ha_snapshot(haddr, neigh, neigh->dev);
		if (nla_put(skb, NDA_LLADDR, neigh->dev->addr_len, haddr) < 0) {
			read_unlock_bh(&neigh->lock);
			goto nla_put_failure;
		}
	}

	ci.ndm_used	 = jiffies_to_clock_t(now - neigh->used);
	ci.ndm_confirmed = jiffies_to_clock_t(now - neigh->confirmed);
	ci.ndm_updated	 = jiffies_to_clock_t(now - neigh->updated);
	ci.ndm_refcnt	 = atomic_read(&neigh->refcnt) - 1;
	read_unlock_bh(&neigh->lock);

	NLA_PUT_U32(skb, NDA_PROBES, atomic_read(&neigh->probes));
	NLA_PUT(skb, NDA_CACHEINFO, sizeof(ci), &ci);

	return nlmsg_end(skb, nlh);

nla_put_failure:
	nlmsg_cancel(skb, nlh);
	return -EMSGSIZE;
}

static int pneigh_fill_info(struct sk_buff *skb, struct pneigh_entry *pn,
			    u32 pid, u32 seq, int type, unsigned int flags,
			    struct neigh_table *tbl)
{
	struct nlmsghdr *nlh;
	struct ndmsg *ndm;

	nlh = nlmsg_put(skb, pid, seq, type, sizeof(*ndm), flags);
	if (nlh == NULL)
		return -EMSGSIZE;

	ndm = nlmsg_data(nlh);
	ndm->ndm_family	 = tbl->family;
	ndm->ndm_pad1    = 0;
	ndm->ndm_pad2    = 0;
	ndm->ndm_flags	 = pn->flags | NTF_PROXY;
	ndm->ndm_type	 = NDA_DST;
	ndm->ndm_ifindex = pn->dev->ifindex;
	ndm->ndm_state	 = NUD_NONE;

	NLA_PUT(skb, NDA_DST, tbl->key_len, pn->key);

	return nlmsg_end(skb, nlh);

nla_put_failure:
	nlmsg_cancel(skb, nlh);
	return -EMSGSIZE;
}

static void neigh_update_notify(struct neighbour *neigh)
{
	call_netevent_notifiers(NETEVENT_NEIGH_UPDATE, neigh);
	__neigh_notify(neigh, RTM_NEWNEIGH, 0);
}

static int neigh_dump_table(struct neigh_table *tbl, struct sk_buff *skb,
			    struct netlink_callback *cb)
{
	struct net *net = sock_net(skb->sk);
	struct neighbour *n;
	int rc, h, s_h = cb->args[1];
	int idx, s_idx = idx = cb->args[2];
	struct neigh_hash_table *nht;

	rcu_read_lock_bh();
	nht = rcu_dereference_bh(tbl->nht);

	for (h = 0; h < (1 << nht->hash_shift); h++) {
		if (h < s_h)
			continue;
		if (h > s_h)
			s_idx = 0;
		for (n = rcu_dereference_bh(nht->hash_buckets[h]), idx = 0;
		     n != NULL;
		     n = rcu_dereference_bh(n->next)) {
			if (!net_eq(dev_net(n->dev), net))
				continue;
			if (idx < s_idx)
				goto next;
			if (neigh_fill_info(skb, n, NETLINK_CB(cb->skb).pid,
					    cb->nlh->nlmsg_seq,
					    RTM_NEWNEIGH,
					    NLM_F_MULTI) <= 0) {
				rc = -1;
				goto out;
			}
next:
			idx++;
		}
	}
	rc = skb->len;
out:
	rcu_read_unlock_bh();
	cb->args[1] = h;
	cb->args[2] = idx;
	return rc;
}

static int pneigh_dump_table(struct neigh_table *tbl, struct sk_buff *skb,
			     struct netlink_callback *cb)
{
	struct pneigh_entry *n;
	struct net *net = sock_net(skb->sk);
	int rc, h, s_h = cb->args[3];
	int idx, s_idx = idx = cb->args[4];

	read_lock_bh(&tbl->lock);

	for (h = 0; h <= PNEIGH_HASHMASK; h++) {
		if (h < s_h)
			continue;
		if (h > s_h)
			s_idx = 0;
		for (n = tbl->phash_buckets[h], idx = 0; n; n = n->next) {
			if (dev_net(n->dev) != net)
				continue;
			if (idx < s_idx)
				goto next;
			if (pneigh_fill_info(skb, n, NETLINK_CB(cb->skb).pid,
					    cb->nlh->nlmsg_seq,
					    RTM_NEWNEIGH,
					    NLM_F_MULTI, tbl) <= 0) {
				read_unlock_bh(&tbl->lock);
				rc = -1;
				goto out;
			}
		next:
			idx++;
		}
	}

	read_unlock_bh(&tbl->lock);
	rc = skb->len;
out:
	cb->args[3] = h;
	cb->args[4] = idx;
	return rc;

}

static int neigh_dump_info(struct sk_buff *skb, struct netlink_callback *cb)
{
	struct neigh_table *tbl;
	int t, family, s_t;
	int proxy = 0;
	int err = 0;

	read_lock(&neigh_tbl_lock);
	family = ((struct rtgenmsg *) nlmsg_data(cb->nlh))->rtgen_family;

	if (nlmsg_len(cb->nlh) >= sizeof(struct ndmsg) &&
	    ((struct ndmsg *) nlmsg_data(cb->nlh))->ndm_flags == NTF_PROXY)
		proxy = 1;

	s_t = cb->args[0];

	for (tbl = neigh_tables, t = 0; tbl && (err >= 0);
	     tbl = tbl->next, t++) {
		if (t < s_t || (family && tbl->family != family))
			continue;
		if (t > s_t)
			memset(&cb->args[1], 0, sizeof(cb->args) -
						sizeof(cb->args[0]));
		if (proxy)
			err = pneigh_dump_table(tbl, skb, cb);
		else
			err = neigh_dump_table(tbl, skb, cb);
	}
	read_unlock(&neigh_tbl_lock);

	cb->args[0] = t;
	return skb->len;
}

void neigh_for_each(struct neigh_table *tbl, void (*cb)(struct neighbour *, void *), void *cookie)
{
	int chain;
	struct neigh_hash_table *nht;

	rcu_read_lock_bh();
	nht = rcu_dereference_bh(tbl->nht);

	read_lock(&tbl->lock); 
	for (chain = 0; chain < (1 << nht->hash_shift); chain++) {
		struct neighbour *n;

		for (n = rcu_dereference_bh(nht->hash_buckets[chain]);
		     n != NULL;
		     n = rcu_dereference_bh(n->next))
			cb(n, cookie);
	}
	read_unlock(&tbl->lock);
	rcu_read_unlock_bh();
}
EXPORT_SYMBOL(neigh_for_each);

void __neigh_for_each_release(struct neigh_table *tbl,
			      int (*cb)(struct neighbour *))
{
	int chain;
	struct neigh_hash_table *nht;

	nht = rcu_dereference_protected(tbl->nht,
					lockdep_is_held(&tbl->lock));
	for (chain = 0; chain < (1 << nht->hash_shift); chain++) {
		struct neighbour *n;
		struct neighbour __rcu **np;

		np = &nht->hash_buckets[chain];
		while ((n = rcu_dereference_protected(*np,
					lockdep_is_held(&tbl->lock))) != NULL) {
			int release;

			write_lock(&n->lock);
			release = cb(n);
			if (release) {
				rcu_assign_pointer(*np,
					rcu_dereference_protected(n->next,
						lockdep_is_held(&tbl->lock)));
				n->dead = 1;
			} else
				np = &n->next;
			write_unlock(&n->lock);
			if (release)
				neigh_cleanup_and_release(n);
		}
	}
}
EXPORT_SYMBOL(__neigh_for_each_release);

#ifdef CONFIG_PROC_FS

static struct neighbour *neigh_get_first(struct seq_file *seq)
{
	struct neigh_seq_state *state = seq->private;
	struct net *net = seq_file_net(seq);
	struct neigh_hash_table *nht = state->nht;
	struct neighbour *n = NULL;
	int bucket = state->bucket;

	state->flags &= ~NEIGH_SEQ_IS_PNEIGH;
	for (bucket = 0; bucket < (1 << nht->hash_shift); bucket++) {
		n = rcu_dereference_bh(nht->hash_buckets[bucket]);

		while (n) {
			if (!net_eq(dev_net(n->dev), net))
				goto next;
			if (state->neigh_sub_iter) {
				loff_t fakep = 0;
				void *v;

				v = state->neigh_sub_iter(state, n, &fakep);
				if (!v)
					goto next;
			}
			if (!(state->flags & NEIGH_SEQ_SKIP_NOARP))
				break;
			if (n->nud_state & ~NUD_NOARP)
				break;
next:
			n = rcu_dereference_bh(n->next);
		}

		if (n)
			break;
	}
	state->bucket = bucket;

	return n;
}

static struct neighbour *neigh_get_next(struct seq_file *seq,
					struct neighbour *n,
					loff_t *pos)
{
	struct neigh_seq_state *state = seq->private;
	struct net *net = seq_file_net(seq);
	struct neigh_hash_table *nht = state->nht;

	if (state->neigh_sub_iter) {
		void *v = state->neigh_sub_iter(state, n, pos);
		if (v)
			return n;
	}
	n = rcu_dereference_bh(n->next);

	while (1) {
		while (n) {
			if (!net_eq(dev_net(n->dev), net))
				goto next;
			if (state->neigh_sub_iter) {
				void *v = state->neigh_sub_iter(state, n, pos);
				if (v)
					return n;
				goto next;
			}
			if (!(state->flags & NEIGH_SEQ_SKIP_NOARP))
				break;

			if (n->nud_state & ~NUD_NOARP)
				break;
next:
			n = rcu_dereference_bh(n->next);
		}

		if (n)
			break;

		if (++state->bucket >= (1 << nht->hash_shift))
			break;

		n = rcu_dereference_bh(nht->hash_buckets[state->bucket]);
	}

	if (n && pos)
		--(*pos);
	return n;
}

static struct neighbour *neigh_get_idx(struct seq_file *seq, loff_t *pos)
{
	struct neighbour *n = neigh_get_first(seq);

	if (n) {
		--(*pos);
		while (*pos) {
			n = neigh_get_next(seq, n, pos);
			if (!n)
				break;
		}
	}
	return *pos ? NULL : n;
}

static struct pneigh_entry *pneigh_get_first(struct seq_file *seq)
{
	struct neigh_seq_state *state = seq->private;
	struct net *net = seq_file_net(seq);
	struct neigh_table *tbl = state->tbl;
	struct pneigh_entry *pn = NULL;
	int bucket = state->bucket;

	state->flags |= NEIGH_SEQ_IS_PNEIGH;
	for (bucket = 0; bucket <= PNEIGH_HASHMASK; bucket++) {
		pn = tbl->phash_buckets[bucket];
		while (pn && !net_eq(pneigh_net(pn), net))
			pn = pn->next;
		if (pn)
			break;
	}
	state->bucket = bucket;

	return pn;
}

static struct pneigh_entry *pneigh_get_next(struct seq_file *seq,
					    struct pneigh_entry *pn,
					    loff_t *pos)
{
	struct neigh_seq_state *state = seq->private;
	struct net *net = seq_file_net(seq);
	struct neigh_table *tbl = state->tbl;

	do {
		pn = pn->next;
	} while (pn && !net_eq(pneigh_net(pn), net));

	while (!pn) {
		if (++state->bucket > PNEIGH_HASHMASK)
			break;
		pn = tbl->phash_buckets[state->bucket];
		while (pn && !net_eq(pneigh_net(pn), net))
			pn = pn->next;
		if (pn)
			break;
	}

	if (pn && pos)
		--(*pos);

	return pn;
}

static struct pneigh_entry *pneigh_get_idx(struct seq_file *seq, loff_t *pos)
{
	struct pneigh_entry *pn = pneigh_get_first(seq);

	if (pn) {
		--(*pos);
		while (*pos) {
			pn = pneigh_get_next(seq, pn, pos);
			if (!pn)
				break;
		}
	}
	return *pos ? NULL : pn;
}

static void *neigh_get_idx_any(struct seq_file *seq, loff_t *pos)
{
	struct neigh_seq_state *state = seq->private;
	void *rc;
	loff_t idxpos = *pos;

	rc = neigh_get_idx(seq, &idxpos);
	if (!rc && !(state->flags & NEIGH_SEQ_NEIGH_ONLY))
		rc = pneigh_get_idx(seq, &idxpos);

	return rc;
}

void *neigh_seq_start(struct seq_file *seq, loff_t *pos, struct neigh_table *tbl, unsigned int neigh_seq_flags)
	__acquires(rcu_bh)
{
	struct neigh_seq_state *state = seq->private;

	state->tbl = tbl;
	state->bucket = 0;
	state->flags = (neigh_seq_flags & ~NEIGH_SEQ_IS_PNEIGH);

	rcu_read_lock_bh();
	state->nht = rcu_dereference_bh(tbl->nht);

	return *pos ? neigh_get_idx_any(seq, pos) : SEQ_START_TOKEN;
}
EXPORT_SYMBOL(neigh_seq_start);

void *neigh_seq_next(struct seq_file *seq, void *v, loff_t *pos)
{
	struct neigh_seq_state *state;
	void *rc;

	if (v == SEQ_START_TOKEN) {
		rc = neigh_get_first(seq);
		goto out;
	}

	state = seq->private;
	if (!(state->flags & NEIGH_SEQ_IS_PNEIGH)) {
		rc = neigh_get_next(seq, v, NULL);
		if (rc)
			goto out;
		if (!(state->flags & NEIGH_SEQ_NEIGH_ONLY))
			rc = pneigh_get_first(seq);
	} else {
		BUG_ON(state->flags & NEIGH_SEQ_NEIGH_ONLY);
		rc = pneigh_get_next(seq, v, NULL);
	}
out:
	++(*pos);
	return rc;
}
EXPORT_SYMBOL(neigh_seq_next);

void neigh_seq_stop(struct seq_file *seq, void *v)
	__releases(rcu_bh)
{
	rcu_read_unlock_bh();
}
EXPORT_SYMBOL(neigh_seq_stop);


static void *neigh_stat_seq_start(struct seq_file *seq, loff_t *pos)
{
	struct neigh_table *tbl = seq->private;
	int cpu;

	if (*pos == 0)
		return SEQ_START_TOKEN;

	for (cpu = *pos-1; cpu < nr_cpu_ids; ++cpu) {
		if (!cpu_possible(cpu))
			continue;
		*pos = cpu+1;
		return per_cpu_ptr(tbl->stats, cpu);
	}
	return NULL;
}

static void *neigh_stat_seq_next(struct seq_file *seq, void *v, loff_t *pos)
{
	struct neigh_table *tbl = seq->private;
	int cpu;

	for (cpu = *pos; cpu < nr_cpu_ids; ++cpu) {
		if (!cpu_possible(cpu))
			continue;
		*pos = cpu+1;
		return per_cpu_ptr(tbl->stats, cpu);
	}
	return NULL;
}

static void neigh_stat_seq_stop(struct seq_file *seq, void *v)
{

}

static int neigh_stat_seq_show(struct seq_file *seq, void *v)
{
	struct neigh_table *tbl = seq->private;
	struct neigh_statistics *st = v;

	if (v == SEQ_START_TOKEN) {
		seq_printf(seq, "entries  allocs destroys hash_grows  lookups hits  res_failed  rcv_probes_mcast rcv_probes_ucast  periodic_gc_runs forced_gc_runs unresolved_discards\n");
		return 0;
	}

	seq_printf(seq, "%08x  %08lx %08lx %08lx  %08lx %08lx  %08lx  "
			"%08lx %08lx  %08lx %08lx %08lx\n",
		   atomic_read(&tbl->entries),

		   st->allocs,
		   st->destroys,
		   st->hash_grows,

		   st->lookups,
		   st->hits,

		   st->res_failed,

		   st->rcv_probes_mcast,
		   st->rcv_probes_ucast,

		   st->periodic_gc_runs,
		   st->forced_gc_runs,
		   st->unres_discards
		   );

	return 0;
}

static const struct seq_operations neigh_stat_seq_ops = {
	.start	= neigh_stat_seq_start,
	.next	= neigh_stat_seq_next,
	.stop	= neigh_stat_seq_stop,
	.show	= neigh_stat_seq_show,
};

static int neigh_stat_seq_open(struct inode *inode, struct file *file)
{
	int ret = seq_open(file, &neigh_stat_seq_ops);

	if (!ret) {
		struct seq_file *sf = file->private_data;
		sf->private = PDE(inode)->data;
	}
	return ret;
};

static const struct file_operations neigh_stat_seq_fops = {
	.owner	 = THIS_MODULE,
	.open 	 = neigh_stat_seq_open,
	.read	 = seq_read,
	.llseek	 = seq_lseek,
	.release = seq_release,
};

#endif 

static inline size_t neigh_nlmsg_size(void)
{
	return NLMSG_ALIGN(sizeof(struct ndmsg))
	       + nla_total_size(MAX_ADDR_LEN) 
	       + nla_total_size(MAX_ADDR_LEN) 
	       + nla_total_size(sizeof(struct nda_cacheinfo))
	       + nla_total_size(4); 
}

static void __neigh_notify(struct neighbour *n, int type, int flags)
{
	struct net *net = dev_net(n->dev);
	struct sk_buff *skb;
	int err = -ENOBUFS;

	skb = nlmsg_new(neigh_nlmsg_size(), GFP_ATOMIC);
	if (skb == NULL)
		goto errout;

	err = neigh_fill_info(skb, n, 0, 0, type, flags);
	if (err < 0) {
		
		WARN_ON(err == -EMSGSIZE);
		kfree_skb(skb);
		goto errout;
	}
	rtnl_notify(skb, net, 0, RTNLGRP_NEIGH, NULL, GFP_ATOMIC);
	return;
errout:
	if (err < 0)
		rtnl_set_sk_err(net, RTNLGRP_NEIGH, err);
}

#ifdef CONFIG_ARPD
void neigh_app_ns(struct neighbour *n)
{
	__neigh_notify(n, RTM_GETNEIGH, NLM_F_REQUEST);
}
EXPORT_SYMBOL(neigh_app_ns);
#endif 

#ifdef CONFIG_SYSCTL

static int proc_unres_qlen(ctl_table *ctl, int write, void __user *buffer,
			   size_t *lenp, loff_t *ppos)
{
	int size, ret;
	ctl_table tmp = *ctl;

	tmp.data = &size;
	size = DIV_ROUND_UP(*(int *)ctl->data, SKB_TRUESIZE(ETH_FRAME_LEN));
	ret = proc_dointvec(&tmp, write, buffer, lenp, ppos);
	if (write && !ret)
		*(int *)ctl->data = size * SKB_TRUESIZE(ETH_FRAME_LEN);
	return ret;
}

enum {
	NEIGH_VAR_MCAST_PROBE,
	NEIGH_VAR_UCAST_PROBE,
	NEIGH_VAR_APP_PROBE,
	NEIGH_VAR_RETRANS_TIME,
	NEIGH_VAR_BASE_REACHABLE_TIME,
	NEIGH_VAR_DELAY_PROBE_TIME,
	NEIGH_VAR_GC_STALETIME,
	NEIGH_VAR_QUEUE_LEN,
	NEIGH_VAR_QUEUE_LEN_BYTES,
	NEIGH_VAR_PROXY_QLEN,
	NEIGH_VAR_ANYCAST_DELAY,
	NEIGH_VAR_PROXY_DELAY,
	NEIGH_VAR_LOCKTIME,
	NEIGH_VAR_RETRANS_TIME_MS,
	NEIGH_VAR_BASE_REACHABLE_TIME_MS,
	NEIGH_VAR_GC_INTERVAL,
	NEIGH_VAR_GC_THRESH1,
	NEIGH_VAR_GC_THRESH2,
	NEIGH_VAR_GC_THRESH3,
	NEIGH_VAR_MAX
};

static struct neigh_sysctl_table {
	struct ctl_table_header *sysctl_header;
	struct ctl_table neigh_vars[NEIGH_VAR_MAX + 1];
	char *dev_name;
} neigh_sysctl_template __read_mostly = {
	.neigh_vars = {
		[NEIGH_VAR_MCAST_PROBE] = {
			.procname	= "mcast_solicit",
			.maxlen		= sizeof(int),
			.mode		= 0644,
			.proc_handler	= proc_dointvec,
		},
		[NEIGH_VAR_UCAST_PROBE] = {
			.procname	= "ucast_solicit",
			.maxlen		= sizeof(int),
			.mode		= 0644,
			.proc_handler	= proc_dointvec,
		},
		[NEIGH_VAR_APP_PROBE] = {
			.procname	= "app_solicit",
			.maxlen		= sizeof(int),
			.mode		= 0644,
			.proc_handler	= proc_dointvec,
		},
		[NEIGH_VAR_RETRANS_TIME] = {
			.procname	= "retrans_time",
			.maxlen		= sizeof(int),
			.mode		= 0644,
			.proc_handler	= proc_dointvec_userhz_jiffies,
		},
		[NEIGH_VAR_BASE_REACHABLE_TIME] = {
			.procname	= "base_reachable_time",
			.maxlen		= sizeof(int),
			.mode		= 0644,
			.proc_handler	= proc_dointvec_jiffies,
		},
		[NEIGH_VAR_DELAY_PROBE_TIME] = {
			.procname	= "delay_first_probe_time",
			.maxlen		= sizeof(int),
			.mode		= 0644,
			.proc_handler	= proc_dointvec_jiffies,
		},
		[NEIGH_VAR_GC_STALETIME] = {
			.procname	= "gc_stale_time",
			.maxlen		= sizeof(int),
			.mode		= 0644,
			.proc_handler	= proc_dointvec_jiffies,
		},
		[NEIGH_VAR_QUEUE_LEN] = {
			.procname	= "unres_qlen",
			.maxlen		= sizeof(int),
			.mode		= 0644,
			.proc_handler	= proc_unres_qlen,
		},
		[NEIGH_VAR_QUEUE_LEN_BYTES] = {
			.procname	= "unres_qlen_bytes",
			.maxlen		= sizeof(int),
			.mode		= 0644,
			.proc_handler	= proc_dointvec,
		},
		[NEIGH_VAR_PROXY_QLEN] = {
			.procname	= "proxy_qlen",
			.maxlen		= sizeof(int),
			.mode		= 0644,
			.proc_handler	= proc_dointvec,
		},
		[NEIGH_VAR_ANYCAST_DELAY] = {
			.procname	= "anycast_delay",
			.maxlen		= sizeof(int),
			.mode		= 0644,
			.proc_handler	= proc_dointvec_userhz_jiffies,
		},
		[NEIGH_VAR_PROXY_DELAY] = {
			.procname	= "proxy_delay",
			.maxlen		= sizeof(int),
			.mode		= 0644,
			.proc_handler	= proc_dointvec_userhz_jiffies,
		},
		[NEIGH_VAR_LOCKTIME] = {
			.procname	= "locktime",
			.maxlen		= sizeof(int),
			.mode		= 0644,
			.proc_handler	= proc_dointvec_userhz_jiffies,
		},
		[NEIGH_VAR_RETRANS_TIME_MS] = {
			.procname	= "retrans_time_ms",
			.maxlen		= sizeof(int),
			.mode		= 0644,
			.proc_handler	= proc_dointvec_ms_jiffies,
		},
		[NEIGH_VAR_BASE_REACHABLE_TIME_MS] = {
			.procname	= "base_reachable_time_ms",
			.maxlen		= sizeof(int),
			.mode		= 0644,
			.proc_handler	= proc_dointvec_ms_jiffies,
		},
		[NEIGH_VAR_GC_INTERVAL] = {
			.procname	= "gc_interval",
			.maxlen		= sizeof(int),
			.mode		= 0644,
			.proc_handler	= proc_dointvec_jiffies,
		},
		[NEIGH_VAR_GC_THRESH1] = {
			.procname	= "gc_thresh1",
			.maxlen		= sizeof(int),
			.mode		= 0644,
			.proc_handler	= proc_dointvec,
		},
		[NEIGH_VAR_GC_THRESH2] = {
			.procname	= "gc_thresh2",
			.maxlen		= sizeof(int),
			.mode		= 0644,
			.proc_handler	= proc_dointvec,
		},
		[NEIGH_VAR_GC_THRESH3] = {
			.procname	= "gc_thresh3",
			.maxlen		= sizeof(int),
			.mode		= 0644,
			.proc_handler	= proc_dointvec,
		},
		{},
	},
};

int neigh_sysctl_register(struct net_device *dev, struct neigh_parms *p,
			  char *p_name, proc_handler *handler)
{
	struct neigh_sysctl_table *t;
	const char *dev_name_source = NULL;

#define NEIGH_CTL_PATH_ROOT	0
#define NEIGH_CTL_PATH_PROTO	1
#define NEIGH_CTL_PATH_NEIGH	2
#define NEIGH_CTL_PATH_DEV	3

	struct ctl_path neigh_path[] = {
		{ .procname = "net",	 },
		{ .procname = "proto",	 },
		{ .procname = "neigh",	 },
		{ .procname = "default", },
		{ },
	};

	t = kmemdup(&neigh_sysctl_template, sizeof(*t), GFP_KERNEL);
	if (!t)
		goto err;

	t->neigh_vars[NEIGH_VAR_MCAST_PROBE].data  = &p->mcast_probes;
	t->neigh_vars[NEIGH_VAR_UCAST_PROBE].data  = &p->ucast_probes;
	t->neigh_vars[NEIGH_VAR_APP_PROBE].data  = &p->app_probes;
	t->neigh_vars[NEIGH_VAR_RETRANS_TIME].data  = &p->retrans_time;
	t->neigh_vars[NEIGH_VAR_BASE_REACHABLE_TIME].data  = &p->base_reachable_time;
	t->neigh_vars[NEIGH_VAR_DELAY_PROBE_TIME].data  = &p->delay_probe_time;
	t->neigh_vars[NEIGH_VAR_GC_STALETIME].data  = &p->gc_staletime;
	t->neigh_vars[NEIGH_VAR_QUEUE_LEN].data  = &p->queue_len_bytes;
	t->neigh_vars[NEIGH_VAR_QUEUE_LEN_BYTES].data  = &p->queue_len_bytes;
	t->neigh_vars[NEIGH_VAR_PROXY_QLEN].data  = &p->proxy_qlen;
	t->neigh_vars[NEIGH_VAR_ANYCAST_DELAY].data  = &p->anycast_delay;
	t->neigh_vars[NEIGH_VAR_PROXY_DELAY].data = &p->proxy_delay;
	t->neigh_vars[NEIGH_VAR_LOCKTIME].data = &p->locktime;
	t->neigh_vars[NEIGH_VAR_RETRANS_TIME_MS].data  = &p->retrans_time;
	t->neigh_vars[NEIGH_VAR_BASE_REACHABLE_TIME_MS].data  = &p->base_reachable_time;

	if (dev) {
		dev_name_source = dev->name;
		
		memset(&t->neigh_vars[NEIGH_VAR_GC_INTERVAL], 0,
		       sizeof(t->neigh_vars[NEIGH_VAR_GC_INTERVAL]));
	} else {
		dev_name_source = neigh_path[NEIGH_CTL_PATH_DEV].procname;
		t->neigh_vars[NEIGH_VAR_GC_INTERVAL].data = (int *)(p + 1);
		t->neigh_vars[NEIGH_VAR_GC_THRESH1].data = (int *)(p + 1) + 1;
		t->neigh_vars[NEIGH_VAR_GC_THRESH2].data = (int *)(p + 1) + 2;
		t->neigh_vars[NEIGH_VAR_GC_THRESH3].data = (int *)(p + 1) + 3;
	}


	if (handler) {
		
		t->neigh_vars[NEIGH_VAR_RETRANS_TIME].proc_handler = handler;
		t->neigh_vars[NEIGH_VAR_RETRANS_TIME].extra1 = dev;
		
		t->neigh_vars[NEIGH_VAR_BASE_REACHABLE_TIME].proc_handler = handler;
		t->neigh_vars[NEIGH_VAR_BASE_REACHABLE_TIME].extra1 = dev;
		
		t->neigh_vars[NEIGH_VAR_RETRANS_TIME_MS].proc_handler = handler;
		t->neigh_vars[NEIGH_VAR_RETRANS_TIME_MS].extra1 = dev;
		
		t->neigh_vars[NEIGH_VAR_BASE_REACHABLE_TIME_MS].proc_handler = handler;
		t->neigh_vars[NEIGH_VAR_BASE_REACHABLE_TIME_MS].extra1 = dev;
	}

	t->dev_name = kstrdup(dev_name_source, GFP_KERNEL);
	if (!t->dev_name)
		goto free;

	neigh_path[NEIGH_CTL_PATH_DEV].procname = t->dev_name;
	neigh_path[NEIGH_CTL_PATH_PROTO].procname = p_name;

	t->sysctl_header =
		register_net_sysctl_table(neigh_parms_net(p), neigh_path, t->neigh_vars);
	if (!t->sysctl_header)
		goto free_procname;

	p->sysctl_table = t;
	return 0;

free_procname:
	kfree(t->dev_name);
free:
	kfree(t);
err:
	return -ENOBUFS;
}
EXPORT_SYMBOL(neigh_sysctl_register);

void neigh_sysctl_unregister(struct neigh_parms *p)
{
	if (p->sysctl_table) {
		struct neigh_sysctl_table *t = p->sysctl_table;
		p->sysctl_table = NULL;
		unregister_sysctl_table(t->sysctl_header);
		kfree(t->dev_name);
		kfree(t);
	}
}
EXPORT_SYMBOL(neigh_sysctl_unregister);

#endif	

static int __init neigh_init(void)
{
	rtnl_register(PF_UNSPEC, RTM_NEWNEIGH, neigh_add, NULL, NULL);
	rtnl_register(PF_UNSPEC, RTM_DELNEIGH, neigh_delete, NULL, NULL);
	rtnl_register(PF_UNSPEC, RTM_GETNEIGH, NULL, neigh_dump_info, NULL);

	rtnl_register(PF_UNSPEC, RTM_GETNEIGHTBL, NULL, neightbl_dump_info,
		      NULL);
	rtnl_register(PF_UNSPEC, RTM_SETNEIGHTBL, neightbl_set, NULL, NULL);

	return 0;
}

subsys_initcall(neigh_init);

