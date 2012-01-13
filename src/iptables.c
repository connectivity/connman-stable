/*
 *
 *  Connection Manager
 *
 *  Copyright (C) 2007-2011  Intel Corporation. All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <getopt.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/errno.h>
#include <sys/socket.h>
#include <xtables.h>

#include <linux/netfilter_ipv4/ip_tables.h>

#include "connman.h"


static const char *hooknames[] = {
	[NF_IP_PRE_ROUTING]	= "PREROUTING",
	[NF_IP_LOCAL_IN]	= "INPUT",
	[NF_IP_FORWARD]		= "FORWARD",
	[NF_IP_LOCAL_OUT]	= "OUTPUT",
	[NF_IP_POST_ROUTING]	= "POSTROUTING",
};

#define LABEL_ACCEPT  "ACCEPT"
#define LABEL_DROP    "DROP"
#define LABEL_QUEUE   "QUEUE"
#define LABEL_RETURN  "RETURN"

#define XT_OPTION_OFFSET_SCALE 256

/* fn returns 0 to continue iteration */
#define _XT_ENTRY_ITERATE_CONTINUE(type, entries, size, n, fn, args...) \
({								\
	unsigned int __i;					\
	int __n;						\
	int __ret = 0;						\
	type *__entry;						\
								\
	for (__i = 0, __n = 0; __i < (size);			\
	     __i += __entry->next_offset, __n++) {		\
		__entry = (void *)(entries) + __i;		\
		if (__n < n)					\
			continue;				\
								\
		__ret = fn(__entry,  ## args);			\
		if (__ret != 0)					\
			break;					\
	}							\
	__ret;							\
})

/* fn returns 0 to continue iteration */
#define _XT_ENTRY_ITERATE(type, entries, size, fn, args...) \
	_XT_ENTRY_ITERATE_CONTINUE(type, entries, size, 0, fn, args)

#define ENTRY_ITERATE(entries, size, fn, args...) \
	_XT_ENTRY_ITERATE(struct ipt_entry, entries, size, fn, ## args)

#define MIN_ALIGN (__alignof__(struct ipt_entry))

#define ALIGN(s) (((s) + ((MIN_ALIGN)-1)) & ~((MIN_ALIGN)-1))

struct error_target {
	struct xt_entry_target t;
	char error[IPT_TABLE_MAXNAMELEN];
};

struct connman_iptables_entry {
	int offset;
	int builtin;

	struct ipt_entry *entry;
};

struct connman_iptables {
	int ipt_sock;

	struct ipt_getinfo *info;
	struct ipt_get_entries *blob_entries;

	unsigned int num_entries;
	unsigned int old_entries;
	unsigned int size;

	unsigned int underflow[NF_INET_NUMHOOKS];
	unsigned int hook_entry[NF_INET_NUMHOOKS];

	GList *entries;
};

static GHashTable *table_hash = NULL;

static struct ipt_entry *get_entry(struct connman_iptables *table,
					unsigned int offset)
{
	return (struct ipt_entry *)((char *)table->blob_entries->entrytable +
									offset);
}

static int is_hook_entry(struct connman_iptables *table,
				struct ipt_entry *entry)
{
	unsigned int i;

	for (i = 0; i < NF_INET_NUMHOOKS; i++) {
		if ((table->info->valid_hooks & (1 << i))
		&& get_entry(table, table->info->hook_entry[i]) == entry)
			return i;
	}

	return -1;
}

static unsigned long entry_to_offset(struct connman_iptables *table,
					struct ipt_entry *entry)
{
	return (void *)entry - (void *)table->blob_entries->entrytable;
}

static int target_to_verdict(char *target_name)
{
	if (!strcmp(target_name, LABEL_ACCEPT))
		return -NF_ACCEPT - 1;

	if (!strcmp(target_name, LABEL_DROP))
		return -NF_DROP - 1;

	if (!strcmp(target_name, LABEL_QUEUE))
		return -NF_QUEUE - 1;

	if (!strcmp(target_name, LABEL_RETURN))
		return XT_RETURN;

	return 0;
}

static gboolean is_builtin_target(char *target_name)
{
	if (!strcmp(target_name, LABEL_ACCEPT) ||
		!strcmp(target_name, LABEL_DROP) ||
		!strcmp(target_name, LABEL_QUEUE) ||
		!strcmp(target_name, LABEL_RETURN))
		return TRUE;

	return FALSE;
}

static gboolean is_jump(struct connman_iptables_entry *e)
{
	struct xt_entry_target *target;

	target = ipt_get_target(e->entry);

	if (!strcmp(target->u.user.name, IPT_STANDARD_TARGET)) {
		struct xt_standard_target *t;

		t = (struct xt_standard_target *)target;

		switch (t->verdict) {
		case XT_RETURN:
		case -NF_ACCEPT - 1:
		case -NF_DROP - 1:
		case -NF_QUEUE - 1:
		case -NF_STOP - 1:
			return false;

		default:
			return true;
		}
	}

	return false;
}

static gboolean is_chain(struct connman_iptables *table,
				struct connman_iptables_entry *e)
{
	struct ipt_entry *entry;
	struct xt_entry_target *target;

	entry = e->entry;
	if (e->builtin >= 0)
		return TRUE;

	target = ipt_get_target(entry);
	if (!strcmp(target->u.user.name, IPT_ERROR_TARGET))
		return TRUE;

	return FALSE;
}

static GList *find_chain_head(struct connman_iptables *table,
				char *chain_name)
{
	GList *list;
	struct connman_iptables_entry *head;
	struct ipt_entry *entry;
	struct xt_entry_target *target;
	int builtin;

	for (list = table->entries; list; list = list->next) {
		head = list->data;
		entry = head->entry;

		/* Buit-in chain */
		builtin = head->builtin;
		if (builtin >= 0 && !strcmp(hooknames[builtin], chain_name))
			break;

		/* User defined chain */
		target = ipt_get_target(entry);
		if (!strcmp(target->u.user.name, IPT_ERROR_TARGET) &&
		    !strcmp((char *)target->data, chain_name))
			break;
	}

	return list;
}

static GList *find_chain_tail(struct connman_iptables *table,
				char *chain_name)
{
	struct connman_iptables_entry *tail;
	GList *chain_head, *list;

	chain_head = find_chain_head(table, chain_name);
	if (chain_head == NULL)
		return NULL;

	/* Then we look for the next chain */
	for (list = chain_head->next; list; list = list->next) {
		tail = list->data;

		if (is_chain(table, tail))
			return list;
	}

	/* Nothing found, we return the table end */
	return g_list_last(table->entries);
}


static void update_offsets(struct connman_iptables *table)
{
	GList *list, *prev;
	struct connman_iptables_entry *entry, *prev_entry;

	for (list = table->entries; list; list = list->next) {
		entry = list->data;

		if (list == table->entries) {
			entry->offset = 0;

			continue;
		}

		prev = list->prev;
		prev_entry = prev->data;

		entry->offset = prev_entry->offset +
					prev_entry->entry->next_offset;
	}
}

static void update_targets_reference(struct connman_iptables *table,
				struct connman_iptables_entry *entry_before,
				struct connman_iptables_entry *modified_entry,
				gboolean is_removing)
{
	struct connman_iptables_entry *tmp;
	struct xt_standard_target *t;
	GList *list;
	int offset;

	offset = modified_entry->entry->next_offset;

	for (list = table->entries; list; list = list->next) {
		tmp = list->data;

		if (!is_jump(tmp))
			continue;

		t = (struct xt_standard_target *)ipt_get_target(tmp->entry);

		if (is_removing == TRUE) {
			if (t->verdict >= entry_before->offset)
				t->verdict -= offset;
		} else {
			if (t->verdict > entry_before->offset)
				t->verdict += offset;
		}
	}
}

static int iptables_add_entry(struct connman_iptables *table,
				struct ipt_entry *entry, GList *before,
					int builtin)
{
	struct connman_iptables_entry *e, *entry_before;

	if (table == NULL)
		return -1;

	e = g_try_malloc0(sizeof(struct connman_iptables_entry));
	if (e == NULL)
		return -1;

	e->entry = entry;
	e->builtin = builtin;

	table->entries = g_list_insert_before(table->entries, before, e);
	table->num_entries++;
	table->size += entry->next_offset;

	if (before == NULL) {
		e->offset = table->size - entry->next_offset;

		return 0;
	}

	entry_before = before->data;

	/*
	 * We've just appended/insterted a new entry. All references
	 * should be bumped accordingly.
	 */
	update_targets_reference(table, entry_before, e, FALSE);

	update_offsets(table);

	return 0;
}

static int remove_table_entry(struct connman_iptables *table,
				struct connman_iptables_entry *entry)
{
	int removed = 0;

	table->num_entries--;
	table->size -= entry->entry->next_offset;
	removed = entry->entry->next_offset;

	g_free(entry->entry);

	table->entries = g_list_remove(table->entries, entry);

	return removed;
}

static int iptables_flush_chain(struct connman_iptables *table,
						char *name)
{
	GList *chain_head, *chain_tail, *list, *next;
	struct connman_iptables_entry *entry;
	int builtin, removed = 0;

	chain_head = find_chain_head(table, name);
	if (chain_head == NULL)
		return -EINVAL;

	chain_tail = find_chain_tail(table, name);
	if (chain_tail == NULL)
		return -EINVAL;

	entry = chain_head->data;
	builtin = entry->builtin;

	if (builtin >= 0)
		list = chain_head;
	else
		list = chain_head->next;

	if (list == chain_tail->prev)
		return 0;

	while (list != chain_tail->prev) {
		entry = list->data;
		next = g_list_next(list);

		removed += remove_table_entry(table, entry);

		list = next;
	}

	if (builtin >= 0) {
		struct connman_iptables_entry *e;

		entry = list->data;

		entry->builtin = builtin;

		table->underflow[builtin] -= removed;

		for (list = chain_tail; list; list = list->next) {
			e = list->data;

			builtin = e->builtin;
			if (builtin < 0)
				continue;

			table->hook_entry[builtin] -= removed;
			table->underflow[builtin] -= removed;
		}
	}

	update_offsets(table);

	return 0;
}

static int iptables_add_chain(struct connman_iptables *table,
					char *name)
{
	GList *last;
	struct ipt_entry *entry_head;
	struct ipt_entry *entry_return;
	struct error_target *error;
	struct ipt_standard_target *standard;
	u_int16_t entry_head_size, entry_return_size;

	last = g_list_last(table->entries);

	/*
	 * An empty chain is composed of:
	 * - A head entry, with no match and an error target.
	 *   The error target data is the chain name.
	 * - A tail entry, with no match and a standard target.
	 *   The standard target verdict is XT_RETURN (return to the
	 *   caller).
	 */

	/* head entry */
	entry_head_size = sizeof(struct ipt_entry) +
				sizeof(struct error_target);
	entry_head = g_try_malloc0(entry_head_size);
	if (entry_head == NULL)
		goto err_head;

	memset(entry_head, 0, entry_head_size);

	entry_head->target_offset = sizeof(struct ipt_entry);
	entry_head->next_offset = entry_head_size;

	error = (struct error_target *) entry_head->elems;
	strcpy(error->t.u.user.name, IPT_ERROR_TARGET);
	error->t.u.user.target_size = ALIGN(sizeof(struct error_target));
	strcpy(error->error, name);

	if (iptables_add_entry(table, entry_head, last, -1) < 0)
		goto err_head;

	/* tail entry */
	entry_return_size = sizeof(struct ipt_entry) +
				sizeof(struct ipt_standard_target);
	entry_return = g_try_malloc0(entry_return_size);
	if (entry_return == NULL)
		goto err;

	memset(entry_return, 0, entry_return_size);

	entry_return->target_offset = sizeof(struct ipt_entry);
	entry_return->next_offset = entry_return_size;

	standard = (struct ipt_standard_target *) entry_return->elems;
	standard->target.u.user.target_size =
				ALIGN(sizeof(struct ipt_standard_target));
	standard->verdict = XT_RETURN;

	if (iptables_add_entry(table, entry_return, last, -1) < 0)
		goto err;

	return 0;

err:
	g_free(entry_return);
err_head:
	g_free(entry_head);

	return -ENOMEM;
}

static int iptables_delete_chain(struct connman_iptables *table, char *name)
{
	struct connman_iptables_entry *entry;
	GList *chain_head, *chain_tail;

	chain_head = find_chain_head(table, name);
	if (chain_head == NULL)
		return -EINVAL;

	entry = chain_head->data;

	/* We cannot remove builtin chain */
	if (entry->builtin >= 0)
		return -EINVAL;

	chain_tail = find_chain_tail(table, name);
	if (chain_tail == NULL)
		return -EINVAL;

	/* Chain must be flushed */
	if (chain_head->next != chain_tail->prev)
		return -EINVAL;

	remove_table_entry(table, entry);

	entry = chain_tail->prev->data;
	remove_table_entry(table, entry);

	update_offsets(table);

	return 0;
}

static struct ipt_entry *new_rule(struct ipt_ip *ip,
		char *target_name, struct xtables_target *xt_t,
		struct xtables_rule_match *xt_rm)
{
	struct xtables_rule_match *tmp_xt_rm;
	struct ipt_entry *new_entry;
	size_t match_size, target_size;

	match_size = 0;
	for (tmp_xt_rm = xt_rm; tmp_xt_rm != NULL; tmp_xt_rm = tmp_xt_rm->next)
		match_size += tmp_xt_rm->match->m->u.match_size;

	if (xt_t)
		target_size = ALIGN(xt_t->t->u.target_size);
	else
		target_size = ALIGN(sizeof(struct xt_standard_target));

	new_entry = g_try_malloc0(sizeof(struct ipt_entry) + target_size +
								match_size);
	if (new_entry == NULL)
		return NULL;

	memcpy(&new_entry->ip, ip, sizeof(struct ipt_ip));

	new_entry->target_offset = sizeof(struct ipt_entry) + match_size;
	new_entry->next_offset = sizeof(struct ipt_entry) + target_size +
								match_size;

	match_size = 0;
	for (tmp_xt_rm = xt_rm; tmp_xt_rm != NULL;
				tmp_xt_rm = tmp_xt_rm->next) {
		memcpy(new_entry->elems + match_size, tmp_xt_rm->match->m,
					tmp_xt_rm->match->m->u.match_size);
		match_size += tmp_xt_rm->match->m->u.match_size;
	}

	if (xt_t) {
		struct xt_entry_target *entry_target;

		entry_target = ipt_get_target(new_entry);
		memcpy(entry_target, xt_t->t, target_size);
	}

	return new_entry;
}

static void update_hooks(struct connman_iptables *table, GList *chain_head,
				struct ipt_entry *entry)
{
	GList *list;
	struct connman_iptables_entry *head, *e;
	int builtin;

	if (chain_head == NULL)
		return;

	head = chain_head->data;

	builtin = head->builtin;
	if (builtin < 0)
		return;

	table->underflow[builtin] += entry->next_offset;

	for (list = chain_head->next; list; list = list->next) {
		e = list->data;

		builtin = e->builtin;
		if (builtin < 0)
			continue;

		table->hook_entry[builtin] += entry->next_offset;
		table->underflow[builtin] += entry->next_offset;
	}
}

static struct ipt_entry *prepare_rule_inclusion(struct connman_iptables *table,
				struct ipt_ip *ip, char *chain_name,
				char *target_name, struct xtables_target *xt_t,
				int *builtin, struct xtables_rule_match *xt_rm)
{
	GList *chain_tail, *chain_head;
	struct ipt_entry *new_entry;
	struct connman_iptables_entry *head;

	chain_head = find_chain_head(table, chain_name);
	if (chain_head == NULL)
		return NULL;

	chain_tail = find_chain_tail(table, chain_name);
	if (chain_tail == NULL)
		return NULL;

	new_entry = new_rule(ip, target_name, xt_t, xt_rm);
	if (new_entry == NULL)
		return NULL;

	update_hooks(table, chain_head, new_entry);

	/*
	 * If the chain is builtin, and does not have any rule,
	 * then the one that we're inserting is becoming the head
	 * and thus needs the builtin flag.
	 */
	head = chain_head->data;
	if (head->builtin < 0)
		*builtin = -1;
	else if (chain_head == chain_tail->prev) {
		*builtin = head->builtin;
		head->builtin = -1;
	}

	return new_entry;
}

static int iptables_append_rule(struct connman_iptables *table,
				struct ipt_ip *ip, char *chain_name,
				char *target_name, struct xtables_target *xt_t,
				struct xtables_rule_match *xt_rm)
{
	GList *chain_tail;
	struct ipt_entry *new_entry;
	int builtin = -1, ret;

	DBG("");

	chain_tail = find_chain_tail(table, chain_name);
	if (chain_tail == NULL)
		return -EINVAL;

	new_entry = prepare_rule_inclusion(table, ip, chain_name,
					target_name, xt_t, &builtin, xt_rm);
	if (new_entry == NULL)
		return -EINVAL;

	ret = iptables_add_entry(table, new_entry, chain_tail->prev, builtin);
	if (ret < 0)
		g_free(new_entry);

	return ret;
}

static int iptables_insert_rule(struct connman_iptables *table,
				struct ipt_ip *ip, char *chain_name,
				char *target_name, struct xtables_target *xt_t,
				struct xtables_rule_match *xt_rm)
{
	struct ipt_entry *new_entry;
	int builtin = -1, ret;
	GList *chain_head;

	chain_head = find_chain_head(table, chain_name);
	if (chain_head == NULL)
		return -EINVAL;

	new_entry = prepare_rule_inclusion(table, ip, chain_name,
					target_name, xt_t, &builtin, xt_rm);
	if (new_entry == NULL)
		return -EINVAL;

	ret = iptables_add_entry(table, new_entry, chain_head->next, builtin);
	if (ret < 0)
		g_free(new_entry);

	return ret;
}

static gboolean is_same_ipt_entry(struct ipt_entry *i_e1,
					struct ipt_entry *i_e2)
{
	if (memcmp(&i_e1->ip, &i_e2->ip, sizeof(struct ipt_ip)) != 0)
		return FALSE;

	if (i_e1->target_offset != i_e2->target_offset)
		return FALSE;

	if (i_e1->next_offset != i_e2->next_offset)
		return FALSE;

	return TRUE;
}

static gboolean is_same_target(struct xt_entry_target *xt_e_t1,
					struct xt_entry_target *xt_e_t2)
{
	if (xt_e_t1 == NULL || xt_e_t2 == NULL)
		return FALSE;

	if (strcmp(xt_e_t1->u.user.name, IPT_STANDARD_TARGET) == 0) {
		struct xt_standard_target *xt_s_t1;
		struct xt_standard_target *xt_s_t2;

		xt_s_t1 = (struct xt_standard_target *) xt_e_t1;
		xt_s_t2 = (struct xt_standard_target *) xt_e_t2;

		if (xt_s_t1->verdict != xt_s_t2->verdict)
			return FALSE;
	} else {
		if (xt_e_t1->u.target_size != xt_e_t2->u.target_size)
			return FALSE;

		if (strcmp(xt_e_t1->u.user.name, xt_e_t2->u.user.name) != 0)
			return FALSE;
	}

	return TRUE;
}

static gboolean is_same_match(struct xt_entry_match *xt_e_m1,
				struct xt_entry_match *xt_e_m2)
{
	if (xt_e_m1 == NULL || xt_e_m2 == NULL)
		return FALSE;

	if (xt_e_m1->u.match_size != xt_e_m2->u.match_size)
		return FALSE;

	if (xt_e_m1->u.user.revision != xt_e_m2->u.user.revision)
		return FALSE;

	if (strcmp(xt_e_m1->u.user.name, xt_e_m2->u.user.name) != 0)
		return FALSE;

	return TRUE;
}

static int iptables_delete_rule(struct connman_iptables *table,
				struct ipt_ip *ip, char *chain_name,
				char *target_name, struct xtables_target *xt_t,
				struct xtables_match *xt_m,
				struct xtables_rule_match *xt_rm)
{
	GList *chain_tail, *chain_head, *list;
	struct xt_entry_target *xt_e_t = NULL;
	struct xt_entry_match *xt_e_m = NULL;
	struct connman_iptables_entry *entry;
	struct ipt_entry *entry_test;
	int builtin, removed;

	removed = 0;

	chain_head = find_chain_head(table, chain_name);
	if (chain_head == NULL)
		return -EINVAL;

	chain_tail = find_chain_tail(table, chain_name);
	if (chain_tail == NULL)
		return -EINVAL;

	if (!xt_t && !xt_m)
		return -EINVAL;

	entry_test = new_rule(ip, target_name, xt_t, xt_rm);
	if (entry_test == NULL)
		return -EINVAL;

	if (xt_t != NULL)
		xt_e_t = ipt_get_target(entry_test);
	if (xt_m != NULL)
		xt_e_m = (struct xt_entry_match *)entry_test->elems;

	entry = chain_head->data;
	builtin = entry->builtin;

	if (builtin >= 0)
		list = chain_head;
	else
		list = chain_head->next;

	for (entry = NULL; list != chain_tail->prev; list = list->next) {
		struct connman_iptables_entry *tmp;
		struct ipt_entry *tmp_e;

		tmp = list->data;
		tmp_e = tmp->entry;

		if (is_same_ipt_entry(entry_test, tmp_e) == FALSE)
			continue;

		if (xt_t != NULL) {
			struct xt_entry_target *tmp_xt_e_t;

			tmp_xt_e_t = ipt_get_target(tmp_e);

			if (!is_same_target(tmp_xt_e_t, xt_e_t))
				continue;
		}

		if (xt_m != NULL) {
			struct xt_entry_match *tmp_xt_e_m;

			tmp_xt_e_m = (struct xt_entry_match *)tmp_e->elems;

			if (!is_same_match(tmp_xt_e_m, xt_e_m))
				continue;
		}

		entry = tmp;
		break;
	}

	if (entry == NULL) {
		g_free(entry_test);
		return -EINVAL;
	}

	/* We have deleted a rule,
	 * all references should be bumped accordingly */
	if (list->next != NULL)
		update_targets_reference(table, list->next->data,
						list->data, TRUE);

	removed += remove_table_entry(table, entry);

	if (builtin >= 0) {
		list = list->next;
		if (list) {
			entry = list->data;
			entry->builtin = builtin;
		}

		table->underflow[builtin] -= removed;
		for (list = chain_tail; list; list = list->next) {
			entry = list->data;

			builtin = entry->builtin;
			if (builtin < 0)
				continue;

			table->hook_entry[builtin] -= removed;
			table->underflow[builtin] -= removed;
		}
	}

	update_offsets(table);

	return 0;
}

static int iptables_change_policy(struct connman_iptables *table,
					char *chain_name, char *policy)
{
	GList *chain_head;
	struct connman_iptables_entry *entry;
	struct xt_entry_target *target;
	struct xt_standard_target *t;
	int verdict;

	verdict = target_to_verdict(policy);
	if (verdict == 0)
		return -EINVAL;

	chain_head = find_chain_head(table, chain_name);
	if (chain_head == NULL)
		return -EINVAL;

	entry = chain_head->data;
	if (entry->builtin < 0)
		return -EINVAL;

	target = ipt_get_target(entry->entry);

	t = (struct xt_standard_target *)target;
	t->verdict = verdict;

	return 0;
}

static struct ipt_replace *iptables_blob(struct connman_iptables *table)
{
	struct ipt_replace *r;
	GList *list;
	struct connman_iptables_entry *e;
	unsigned char *entry_index;

	r = g_try_malloc0(sizeof(struct ipt_replace) + table->size);
	if (r == NULL)
		return NULL;

	memset(r, 0, sizeof(*r) + table->size);

	r->counters = g_try_malloc0(sizeof(struct xt_counters)
				* table->old_entries);
	if (r->counters == NULL) {
		g_free(r);
		return NULL;
	}

	strcpy(r->name, table->info->name);
	r->num_entries = table->num_entries;
	r->size = table->size;

	r->num_counters = table->old_entries;
	r->valid_hooks  = table->info->valid_hooks;

	memcpy(r->hook_entry, table->hook_entry, sizeof(table->hook_entry));
	memcpy(r->underflow, table->underflow, sizeof(table->underflow));

	entry_index = (unsigned char *)r->entries;
	for (list = table->entries; list; list = list->next) {
		e = list->data;

		memcpy(entry_index, e->entry, e->entry->next_offset);
		entry_index += e->entry->next_offset;
	}

	return r;
}

static void dump_ip(struct ipt_entry *entry)
{
	struct ipt_ip *ip = &entry->ip;
	char ip_string[INET6_ADDRSTRLEN];
	char ip_mask[INET6_ADDRSTRLEN];

	if (strlen(ip->iniface))
		connman_info("\tin %s", ip->iniface);

	if (strlen(ip->outiface))
		connman_info("\tout %s", ip->outiface);

	if (inet_ntop(AF_INET, &ip->src, ip_string, INET6_ADDRSTRLEN) != NULL &&
			inet_ntop(AF_INET, &ip->smsk,
					ip_mask, INET6_ADDRSTRLEN) != NULL)
		connman_info("\tsrc %s/%s", ip_string, ip_mask);

	if (inet_ntop(AF_INET, &ip->dst, ip_string, INET6_ADDRSTRLEN) != NULL &&
			inet_ntop(AF_INET, &ip->dmsk,
					ip_mask, INET6_ADDRSTRLEN) != NULL)
		connman_info("\tdst %s/%s", ip_string, ip_mask);
}

static void dump_target(struct connman_iptables *table,
				struct ipt_entry *entry)

{
	struct xtables_target *xt_t;
	struct xt_entry_target *target;

	target = ipt_get_target(entry);

	if (!strcmp(target->u.user.name, IPT_STANDARD_TARGET)) {
		struct xt_standard_target *t;

		t = (struct xt_standard_target *)target;

		switch (t->verdict) {
		case XT_RETURN:
			connman_info("\ttarget RETURN");
			break;

		case -NF_ACCEPT - 1:
			connman_info("\ttarget ACCEPT");
			break;

		case -NF_DROP - 1:
			connman_info("\ttarget DROP");
			break;

		case -NF_QUEUE - 1:
			connman_info("\ttarget QUEUE");
			break;

		case -NF_STOP - 1:
			connman_info("\ttarget STOP");
			break;

		default:
			connman_info("\tJUMP @%p (0x%x)",
				(char*)table->blob_entries->entrytable +
				t->verdict, t->verdict);
			break;
		}

		xt_t = xtables_find_target(IPT_STANDARD_TARGET,
						XTF_LOAD_MUST_SUCCEED);

		if(xt_t->print != NULL)
			xt_t->print(NULL, target, 1);
	} else {
		xt_t = xtables_find_target(target->u.user.name, XTF_TRY_LOAD);
		if (xt_t == NULL) {
			connman_info("\ttarget %s", target->u.user.name);
			return;
		}

		if(xt_t->print != NULL) {
			connman_info("\ttarget ");
			xt_t->print(NULL, target, 1);
		}
	}
}

static void dump_match(struct connman_iptables *table, struct ipt_entry *entry)
{
	struct xtables_match *xt_m;
	struct xt_entry_match *match;

	if (entry->elems == (unsigned char *)entry + entry->target_offset)
		return;

	match = (struct xt_entry_match *) entry->elems;

	if (!strlen(match->u.user.name))
		return;

	xt_m = xtables_find_match(match->u.user.name, XTF_TRY_LOAD, NULL);
	if (xt_m == NULL)
		goto out;

	if(xt_m->print != NULL) {
		connman_info("\tmatch ");
		xt_m->print(NULL, match, 1);

		return;
	}

out:
	connman_info("\tmatch %s", match->u.user.name);

}

static int dump_entry(struct ipt_entry *entry,
				struct connman_iptables *table)
{
	struct xt_entry_target *target;
	unsigned int offset;
	int builtin;

	offset = (char *)entry - (char *)table->blob_entries->entrytable;
	target = ipt_get_target(entry);
	builtin = is_hook_entry(table, entry);

	if (entry_to_offset(table, entry) + entry->next_offset ==
					table->blob_entries->size) {
		connman_info("End of CHAIN 0x%x", offset);
		return 0;
	}

	if (!strcmp(target->u.user.name, IPT_ERROR_TARGET)) {
		connman_info("USER CHAIN (%s) %p  match %p  target %p  size %d",
			target->data, entry, entry->elems,
			(char *)entry + entry->target_offset,
				entry->next_offset);

		return 0;
	} else if (builtin >= 0) {
		connman_info("CHAIN (%s) %p  match %p  target %p  size %d",
			hooknames[builtin], entry, entry->elems,
			(char *)entry + entry->target_offset,
				entry->next_offset);
	} else {
		connman_info("RULE %p  match %p  target %p  size %d", entry,
			entry->elems,
			(char *)entry + entry->target_offset,
				entry->next_offset);
	}

	dump_match(table, entry);
	dump_target(table, entry);
	dump_ip(entry);

	return 0;
}

static void iptables_dump(struct connman_iptables *table)
{
	connman_info("%s valid_hooks=0x%08x, num_entries=%u, size=%u",
			table->info->name,
			table->info->valid_hooks, table->info->num_entries,
				table->info->size);

	ENTRY_ITERATE(table->blob_entries->entrytable,
			table->blob_entries->size,
			dump_entry, table);

}

static int iptables_get_entries(struct connman_iptables *table)
{
	socklen_t entry_size;

	entry_size = sizeof(struct ipt_get_entries) + table->info->size;

	return getsockopt(table->ipt_sock, IPPROTO_IP, IPT_SO_GET_ENTRIES,
				table->blob_entries, &entry_size);
}

static int iptables_replace(struct connman_iptables *table,
					struct ipt_replace *r)
{
	return setsockopt(table->ipt_sock, IPPROTO_IP, IPT_SO_SET_REPLACE, r,
			 sizeof(*r) + r->size);
}

static int add_entry(struct ipt_entry *entry, struct connman_iptables *table)
{
	struct ipt_entry *new_entry;
	int builtin;

	new_entry = g_try_malloc0(entry->next_offset);
	if (new_entry == NULL)
		return -ENOMEM;

	memcpy(new_entry, entry, entry->next_offset);

	builtin = is_hook_entry(table, entry);

	return iptables_add_entry(table, new_entry, NULL, builtin);
}

static void table_cleanup(struct connman_iptables *table)
{
	GList *list;
	struct connman_iptables_entry *entry;

	close(table->ipt_sock);

	for (list = table->entries; list; list = list->next) {
		entry = list->data;

		g_free(entry->entry);
		g_free(entry);
	}

	g_list_free(table->entries);
	g_free(table->info);
	g_free(table->blob_entries);
	g_free(table);
}

static struct connman_iptables *iptables_init(char *table_name)
{
	struct connman_iptables *table = NULL;
	char *module = NULL;
	socklen_t s;

	DBG("%s", table_name);

	if (xtables_insmod("ip_tables", NULL, TRUE) != 0)
		goto err;

	module = g_strconcat("iptable_", table_name, NULL);
	if (module == NULL)
		goto err;

	if (xtables_insmod(module, NULL, TRUE) != 0)
		goto err;

	g_free(module);
	module = NULL;

	table = g_hash_table_lookup(table_hash, table_name);
	if (table != NULL)
		return table;

	table = g_try_new0(struct connman_iptables, 1);
	if (table == NULL)
		return NULL;

	table->info = g_try_new0(struct ipt_getinfo, 1);
	if (table->info == NULL)
		goto err;

	table->ipt_sock = socket(AF_INET, SOCK_RAW | SOCK_CLOEXEC, IPPROTO_RAW);
	if (table->ipt_sock < 0)
		goto err;

	s = sizeof(*table->info);
	strcpy(table->info->name, table_name);
	if (getsockopt(table->ipt_sock, IPPROTO_IP, IPT_SO_GET_INFO,
						table->info, &s) < 0)
		goto err;

	table->blob_entries = g_try_malloc0(sizeof(struct ipt_get_entries) +
						table->info->size);
	if (table->blob_entries == NULL)
		goto err;

	strcpy(table->blob_entries->name, table_name);
	table->blob_entries->size = table->info->size;

	if (iptables_get_entries(table) < 0)
		goto err;

	table->num_entries = 0;
	table->old_entries = table->info->num_entries;
	table->size = 0;

	memcpy(table->underflow, table->info->underflow,
				sizeof(table->info->underflow));
	memcpy(table->hook_entry, table->info->hook_entry,
				sizeof(table->info->hook_entry));

	ENTRY_ITERATE(table->blob_entries->entrytable,
			table->blob_entries->size,
				add_entry, table);

	g_hash_table_insert(table_hash, g_strdup(table_name), table);

	return table;

err:
	g_free(module);

	table_cleanup(table);

	return NULL;
}

static struct option iptables_opts[] = {
	{.name = "append",        .has_arg = 1, .val = 'A'},
	{.name = "delete",        .has_arg = 1, .val = 'D'},
	{.name = "flush-chain",   .has_arg = 1, .val = 'F'},
	{.name = "insert",        .has_arg = 1, .val = 'I'},
	{.name = "list",          .has_arg = 2, .val = 'L'},
	{.name = "new-chain",     .has_arg = 1, .val = 'N'},
	{.name = "policy",        .has_arg = 1, .val = 'P'},
	{.name = "delete-chain",  .has_arg = 1, .val = 'X'},
	{.name = "destination",   .has_arg = 1, .val = 'd'},
	{.name = "in-interface",  .has_arg = 1, .val = 'i'},
	{.name = "jump",          .has_arg = 1, .val = 'j'},
	{.name = "match",         .has_arg = 1, .val = 'm'},
	{.name = "out-interface", .has_arg = 1, .val = 'o'},
	{.name = "source",        .has_arg = 1, .val = 's'},
	{.name = "table",         .has_arg = 1, .val = 't'},
	{NULL},
};

struct xtables_globals iptables_globals = {
	.option_offset = 0,
	.opts = iptables_opts,
	.orig_opts = iptables_opts,
};

static struct xtables_target *prepare_target(struct connman_iptables *table,
							char *target_name)
{
	struct xtables_target *xt_t = NULL;
	gboolean is_builtin, is_user_defined;
	GList *chain_head = NULL;
	size_t target_size;

	is_builtin = FALSE;
	is_user_defined = FALSE;

	if (is_builtin_target(target_name))
		is_builtin = TRUE;
	else {
		chain_head = find_chain_head(table, target_name);
		if (chain_head != NULL && chain_head->next != NULL)
			is_user_defined = TRUE;
	}

	if (is_builtin || is_user_defined)
		xt_t = xtables_find_target(IPT_STANDARD_TARGET,
						XTF_LOAD_MUST_SUCCEED);
	else
		xt_t = xtables_find_target(target_name, XTF_TRY_LOAD);

	if (xt_t == NULL)
		return NULL;

	target_size = ALIGN(sizeof(struct ipt_entry_target)) + xt_t->size;

	xt_t->t = g_try_malloc0(target_size);
	if (xt_t->t == NULL)
		return NULL;

	xt_t->t->u.target_size = target_size;

	if (is_builtin || is_user_defined) {
		struct xt_standard_target *target;

		target = (struct xt_standard_target *)(xt_t->t);
		strcpy(target->target.u.user.name, IPT_STANDARD_TARGET);

		if (is_builtin == TRUE)
			target->verdict = target_to_verdict(target_name);
		else if (is_user_defined == TRUE) {
			struct connman_iptables_entry *target_rule;

			if (chain_head == NULL) {
				g_free(xt_t->t);
				return NULL;
			}

			target_rule = chain_head->next->data;
			target->verdict = target_rule->offset;
		}
	} else {
		strcpy(xt_t->t->u.user.name, target_name);
		xt_t->t->u.user.revision = xt_t->revision;
		if (xt_t->init != NULL)
			xt_t->init(xt_t->t);
	}

#if XTABLES_VERSION_CODE > 5
	if (xt_t->x6_options != NULL)
		iptables_globals.opts =
			xtables_options_xfrm(
				iptables_globals.orig_opts,
				iptables_globals.opts,
				xt_t->x6_options,
				&xt_t->option_offset);
	else
#endif
		iptables_globals.opts =
			xtables_merge_options(
#if XTABLES_VERSION_CODE > 5
				iptables_globals.orig_opts,
#endif
				iptables_globals.opts,
				xt_t->extra_opts,
				&xt_t->option_offset);

	if (iptables_globals.opts == NULL) {
		g_free(xt_t->t);
		xt_t = NULL;
	}

	return xt_t;
}

static struct xtables_match *prepare_matches(struct connman_iptables *table,
			struct xtables_rule_match **xt_rm, char *match_name)
{
	struct xtables_match *xt_m;
	size_t match_size;

	if (match_name == NULL)
		return NULL;

	xt_m = xtables_find_match(match_name, XTF_LOAD_MUST_SUCCEED, xt_rm);
	match_size = ALIGN(sizeof(struct ipt_entry_match)) + xt_m->size;

	xt_m->m = g_try_malloc0(match_size);
	if (xt_m->m == NULL)
		return NULL;

	xt_m->m->u.match_size = match_size;
	strcpy(xt_m->m->u.user.name, xt_m->name);
	xt_m->m->u.user.revision = xt_m->revision;

	if (xt_m->init != NULL)
		xt_m->init(xt_m->m);

	if (xt_m == xt_m->next)
		goto done;

#if XTABLES_VERSION_CODE > 5
	if (xt_m->x6_options != NULL)
		iptables_globals.opts =
			xtables_options_xfrm(
				iptables_globals.orig_opts,
				iptables_globals.opts,
				xt_m->x6_options,
				&xt_m->option_offset);
	else
#endif
			iptables_globals.opts =
			xtables_merge_options(
#if XTABLES_VERSION_CODE > 5
				iptables_globals.orig_opts,
#endif
				iptables_globals.opts,
				xt_m->extra_opts,
				&xt_m->option_offset);

	if (iptables_globals.opts == NULL) {
		g_free(xt_m->m);
		xt_m = NULL;
	}

done:
	return xt_m;
}

static int iptables_command(int argc, char *argv[])
{
	struct connman_iptables *table;
	struct xtables_rule_match *xt_rm, *tmp_xt_rm;
	struct xtables_match *xt_m, *xt_m_t;
	struct xtables_target *xt_t;
	struct ipt_ip ip;
	char *table_name, *chain, *new_chain, *match_name, *target_name;
	char *flush_chain, *delete_chain, *policy;
	int c, ret, in_len, out_len;
	gboolean dump, invert, insert, delete;
	struct in_addr src, dst;

	if (argc == 0)
		return -EINVAL;

	dump = FALSE;
	invert = FALSE;
	insert = FALSE;
	delete = FALSE;
	table_name = chain = new_chain = match_name = target_name = NULL;
	flush_chain = delete_chain = policy = NULL;
	memset(&ip, 0, sizeof(struct ipt_ip));
	table = NULL;
	xt_rm = NULL;
	xt_m = NULL;
	xt_t = NULL;
	ret = 0;

	/* extension's options will generate false-positives errors */
	opterr = 0;

	optind = 0;

	while ((c = getopt_long(argc, argv, "-A:F:I:L::N:P:X:d:j:i:m:o:s:t:",
					iptables_globals.opts, NULL)) != -1) {
		switch (c) {
		case 'A':
			/* It is either -A, -D or -I at once */
			if (chain)
				goto out;

			chain = optarg;
			break;

		case 'D':
			/* It is either -A, -D or -I at once */
			if (chain)
				goto out;

			chain = optarg;
			delete = TRUE;
			break;

		case 'F':
			flush_chain = optarg;
			break;

		case 'I':
			/* It is either -A, -D or -I at once */
			if (chain)
				goto out;

			chain = optarg;
			insert = TRUE;
			break;

		case 'L':
			dump = TRUE;
			break;

		case 'N':
			new_chain = optarg;
			break;

		case 'P':
			chain = optarg;
			if (optind < argc)
				policy = argv[optind++];
			else
				goto out;

			break;

		case 'X':
			delete_chain = optarg;
			break;

		case 'd':
			if (!inet_pton(AF_INET, optarg, &dst))
				break;

			ip.dst = dst;
			inet_pton(AF_INET, "255.255.255.255", &ip.dmsk);

			if (invert)
				ip.invflags |= IPT_INV_DSTIP;

			break;

		case 'i':
			in_len = strlen(optarg);

			if (in_len + 1 > IFNAMSIZ)
				break;

			strcpy(ip.iniface, optarg);
			memset(ip.iniface_mask, 0xff, in_len + 1);

			if (invert)
				ip.invflags |= IPT_INV_VIA_IN;

			break;

		case 'j':
			target_name = optarg;
			xt_t = prepare_target(table, target_name);
			if (xt_t == NULL)
				goto out;

			break;

		case 'm':
			match_name = optarg;
			xt_m = prepare_matches(table, &xt_rm, match_name);
			if (xt_m == NULL)
				goto out;

			break;

		case 'o':
			out_len = strlen(optarg);

			if (out_len + 1 > IFNAMSIZ)
				break;

			strcpy(ip.outiface, optarg);
			memset(ip.outiface_mask, 0xff, out_len + 1);

			if (invert)
				ip.invflags |= IPT_INV_VIA_OUT;

			break;

		case 's':
			if (!inet_pton(AF_INET, optarg, &src))
				break;

			ip.src = src;
			inet_pton(AF_INET, "255.255.255.255", &ip.smsk);

			if (invert)
				ip.invflags |= IPT_INV_SRCIP;

			break;

		case 't':
			table_name = optarg;

			table = iptables_init(table_name);
			if (table == NULL) {
				ret = -EINVAL;
				goto out;
			}

			break;

		case 1:
			if (optarg[0] == '!' && optarg[1] == '\0') {
				invert = TRUE;
				optarg[0] = '\0';
				continue;
			}

			connman_error("Invalid option");

			ret = -EINVAL;
			goto out;

		default:
#if XTABLES_VERSION_CODE > 5
			if (xt_t != NULL && (xt_t->x6_parse != NULL ||
						xt_t->parse != NULL) &&
					(c >= (int) xt_t->option_offset &&
					c < (int) xt_t->option_offset +
					XT_OPTION_OFFSET_SCALE)) {
				xtables_option_tpcall(c, argv,
							invert,	xt_t, NULL);

				break;
			}

			for (tmp_xt_rm = xt_rm; tmp_xt_rm != NULL;
						tmp_xt_rm = tmp_xt_rm->next) {
				xt_m_t = tmp_xt_rm->match;

				if (tmp_xt_rm->completed ||
						(xt_m_t->x6_parse == NULL &&
						 xt_m_t->parse == NULL))
					continue;

				if (c < (int) xt_m_t->option_offset ||
					c >= (int) xt_m_t->option_offset
					+ XT_OPTION_OFFSET_SCALE)
					continue;

				xtables_option_mpcall(c, argv,
							invert, xt_m_t, NULL);

				break;
			}
#else
			if (xt_t == NULL || xt_t->parse == NULL ||
				!xt_t->parse(c - xt_t->option_offset,
				argv, invert, &xt_t->tflags, NULL, &xt_t->t)) {

				for (tmp_xt_rm = xt_rm; tmp_xt_rm != NULL;
						tmp_xt_rm = tmp_xt_rm->next) {
					xt_m_t = tmp_xt_rm->match;

					if (tmp_xt_rm->completed ||
							xt_m_t->parse == NULL)
						continue;

					if (xt_m->parse(c - xt_m->option_offset,
						argv, invert, &xt_m->mflags,
						NULL, &xt_m->m))
						break;
				}
			}
#endif
			break;
		}

		invert = FALSE;
	}

#if XTABLES_VERSION_CODE > 5
	for (tmp_xt_rm = xt_rm; tmp_xt_rm != NULL;
				tmp_xt_rm = tmp_xt_rm->next)
		xtables_option_mfcall(tmp_xt_rm->match);

	if (xt_t != NULL)
		xtables_option_tfcall(xt_t);
#else
	for (tmp_xt_rm = xt_rm; tmp_xt_rm != NULL;
				tmp_xt_rm = tmp_xt_rm->next)
		if (tmp_xt_rm->match->final_check != NULL)
			tmp_xt_rm->match->final_check(
					tmp_xt_rm->match->mflags);

	if (xt_t != NULL && xt_t->final_check != NULL)
		xt_t->final_check(xt_t->tflags);
#endif

	if (table == NULL) {
		table_name = "filter";

		table = iptables_init(table_name);
		if (table == NULL) {
			ret = -EINVAL;
			goto out;
		}
	}

	if (delete_chain != NULL) {
		printf("Delete chain %s\n", delete_chain);

		iptables_delete_chain(table, delete_chain);

		goto out;
	}

	if (dump) {
		iptables_dump(table);

		ret = 0;
		goto out;
	}

	if (flush_chain) {
		DBG("Flush chain %s", flush_chain);

		iptables_flush_chain(table, flush_chain);

		goto out;
	}

	if (chain && new_chain) {
		ret = -EINVAL;
		goto out;
	}

	if (new_chain) {
		DBG("New chain %s", new_chain);

		ret = iptables_add_chain(table, new_chain);
		goto out;
	}

	if (chain) {
		if (policy != NULL) {
			printf("Changing policy of %s to %s\n", chain, policy);

			iptables_change_policy(table, chain, policy);

			goto out;
		}

		if (xt_t == NULL)
			goto out;

		if (delete == TRUE) {
			DBG("Deleting %s to %s (match %s)\n",
					target_name, chain, match_name);

			ret = iptables_delete_rule(table, &ip, chain,
					target_name, xt_t, xt_m, xt_rm);

			goto out;
		}

		if (insert == TRUE) {
			DBG("Inserting %s to %s (match %s)",
					target_name, chain, match_name);

			ret = iptables_insert_rule(table, &ip, chain,
						target_name, xt_t, xt_rm);

			goto out;
		} else {
			DBG("Adding %s to %s (match %s)",
					target_name, chain, match_name);

			ret = iptables_append_rule(table, &ip, chain,
						target_name, xt_t, xt_rm);

			goto out;
		}
	}

out:
	if (xt_t)
		g_free(xt_t->t);

	if (xt_m)
		g_free(xt_m->m);

	return ret;
}

int __connman_iptables_command(const char *format, ...)
{
	char **argv, **arguments, *command;
	int argc, i, ret;
	va_list args;

	if (format == NULL)
		return -EINVAL;

	va_start(args, format);

	command = g_strdup_vprintf(format, args);

	va_end(args);

	if (command == NULL)
		return -ENOMEM;

	arguments = g_strsplit_set(command, " ", -1);

	for (argc = 0; arguments[argc]; argc++);
	++argc;

	DBG("command %s argc %d", command, argc);

	argv = g_try_malloc0(argc * sizeof(char *));
	if (argv == NULL) {
		g_free(command);
		g_strfreev(arguments);
		return -ENOMEM;
	}

	argv[0] = "iptables";
	for (i = 1; i < argc; i++)
		argv[i] = arguments[i - 1];

	ret = iptables_command(argc, argv);

	g_free(command);
	g_strfreev(arguments);
	g_free(argv);

	return ret;
}


int __connman_iptables_commit(const char *table_name)
{
	struct connman_iptables *table;
	struct ipt_replace *repl;
	int err;

	DBG("%s", table_name);

	table = g_hash_table_lookup(table_hash, table_name);
	if (table == NULL)
		return -EINVAL;

	repl = iptables_blob(table);

	err = iptables_replace(table, repl);

	g_free(repl->counters);
	g_free(repl);

	if (err < 0)
	    return err;

	g_hash_table_remove(table_hash, table_name);

	return 0;
}

static void remove_table(gpointer user_data)
{
	struct connman_iptables *table = user_data;

	table_cleanup(table);
}

int __connman_iptables_init(void)
{
	DBG("");

	table_hash = g_hash_table_new_full(g_str_hash, g_str_equal,
						g_free, remove_table);

	xtables_init_all(&iptables_globals, NFPROTO_IPV4);

	return 0;

}

void __connman_iptables_cleanup(void)
{
	DBG("");

	g_hash_table_destroy(table_hash);

	xtables_free_opts(1);
}
