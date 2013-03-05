/*************************************************************************
 * prioq.c
 * 
 * Priority queue, allowing concurrent update by use of CAS primitives. 
 * 
 * Heavily relying on work by K A Fraser
 * Copyright (c) 2001-2003, K A Fraser
 * Copyright (c) 2013-2016, Jonatan Linden
 *
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 * * Redistributions of source code must retain the above copyright 
 * notice, this list of conditions and the following disclaimer.
 * 
 * * Redistributions in binary form must reproduce the above copyright 
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 * 
 * * The name of the author may not be used to endorse or promote products
 * derived from this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR 
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED 
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE 
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, 
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES 
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR 
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) 
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, 
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN 
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE 
 * POSSIBILITY OF SUCH DAMAGE.
 */


#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <inttypes.h>


#include "portable_defns.h"
#include "prioq.h"
#include "ptst.h"
#include "j_util.h"


static int gc_id[NUM_LEVELS];

/*
 * PRIVATE FUNCTIONS
 */

/*
 * Random level generator. Drop-off rate is 0.5 per level.
 * Returns value 1 <= level <= NUM_LEVELS.
 */
static int
get_level(ptst_t *ptst)
{
    unsigned long r = rand_next(ptst);
    int l = 1;
    r = (r >> 4) & ((1 << (NUM_LEVELS-1)) - 1);
    while ( (r & 1) ) { l++; r >>= 1; }
    return l;
}

/*
 * Allocate a new node, and initialise its @level field.
 * NB. Initialisation will eventually be pushed into garbage collector,
 * because of dependent read reordering.
 */

//unsigned int align_cnt;
static node_t *
alloc_node(ptst_t *ptst)
{
    int l;
    node_t *n;
    l = get_level(ptst);
    n = gc_alloc(ptst, gc_id[l - 1]);
    n->level = l;
//    if (!((uintptr_t)n & 0x7F)) {
//	unsigned int old = __sync_fetch_and_add(&align_cnt, 1);
//	if (!(old % 10000)) {
//	    printf("%u\n", old);
//	}
//    }
    
    return n;
}


/* Free a node to the garbage collector. */
static void 
free_node(ptst_t *ptst, sh_node_pt n)
{
    gc_free(ptst, (void *)n, gc_id[(n->level & LEVEL_MASK) - 1]);
}


static sh_node_pt 
weak_search_head(set_t *l)
{
    sh_node_pt x, x_next;
    setkey_t  x_next_k;
    int        i;
    x = l->head;
    for ( i = NUM_LEVELS - 1; i >= 0; i-- )
    {
        for ( ; ; )
        {
            x_next = x->next[i];
            x_next = get_unmarked_ref(x_next);
	    if (x_next == END) break;
	    if (!is_marked_ref(x_next->next[0])) break;
	    x = x_next;
	}
    }
    return x_next;
}


static int
weak_search_end(set_t *l, sh_node_pt *pa, int toplvl)
{
    sh_node_pt x, x_next;
    setkey_t  x_next_k;
    int        i;
    int lvl = 0;
    int start_lvl = toplvl == -1 ? NUM_LEVELS - 1 : toplvl;
    if (toplvl > 0) lvl = toplvl;
    
    x = l->head;
    for ( i = start_lvl; i >= 1; i-- )
    {
        for ( ; ; )
        {
	    x_next = x->next[i];
	    if (!is_marked_ref(x_next->next[0])) break;

	    /* first lvl that actually needs updating */
	    if (!lvl) lvl = i;
	    
            x = x_next;
	    assert(x != END);
	}

        pa[i] = x;
    }
    return lvl;
}


/*
 * Search for first N, with key >= @k at each level in @l.
 * RETURN VALUES:
 *  Array @pa: @pa[i] is non-deleted predecessor of N at level i
 *  Array @na: @na[i] is N itself, which should be pointed at by @pa[i]
 *  MAIN RETURN VALUE: same as @na[0].
 */
/* This function does not remove marked nodes. Use it optimistically. */
static sh_node_pt
weak_search_predecessors(set_t *l, setkey_t k,
			 sh_node_pt *pa, sh_node_pt *na, int bef)
{
    sh_node_pt x, x_next;
    setkey_t  x_next_k;
    int        i;
restart:
    x = l->head;
    for ( i = NUM_LEVELS - 1; i >= 0; i-- )
    {
        for ( ; ; )
        {
            x_next = x->next[i];
            x_next = get_unmarked_ref(x_next);

	    assert (x_next != END);
            x_next_k = x_next->k;
            if ( x_next_k > k || (bef && x_next_k == k)) break;

            x = x_next;
	}

        if ( pa ) pa[i] = x;
        if ( na ) na[i] = x_next;
    }

    return x;
}



/*
 * PUBLIC FUNCTIONS
 */

set_t *
set_alloc(int max_offset, int max_level)
{
    set_t *l;
    node_t *t, *h;
    int i;

    t = malloc(63+sizeof(node_t) + (NUM_LEVELS-1)*sizeof(node_t *));
    t = ((uintptr_t)t+63) & ~(size_t)0x3F;
    memset(t, 0, sizeof(node_t) + (NUM_LEVELS-1)*sizeof(node_t *));
    h = malloc(63+sizeof(node_t) + (NUM_LEVELS-1)*sizeof(node_t *));
    h = ((uintptr_t)h+63) & ~(size_t)0x3F;
    memset(h, 0, sizeof(*h) + (NUM_LEVELS-1)*sizeof(node_t *));

    assert(((unsigned long)t & 0x3FLU) == 0);
    assert(((unsigned long)h & 0x3FLU) == 0);


    t->k = SENTINEL_KEYMAX;
    h->k = SENTINEL_KEYMIN;
    h->level = NUM_LEVELS;
    for ( i = 0; i < NUM_LEVELS; i++ )
    {
        h->next[i] = t;
    }    

    printf("sentinel max: %lu\n", t->k);
    /*
     * Set the forward pointers of final node to other than NULL,
     * otherwise READ_FIELD() will continually execute costly barriers.
     * Note use of 0xfe -- that doesn't look like a marked value!
     */
#ifdef USE_FAA
    memset(t->next, 0xc0, NUM_LEVELS*sizeof(node_t *));
#else
    memset(t->next, 0xfe, NUM_LEVELS*sizeof(node_t *));
#endif


    l = malloc(sizeof(*l) + (NUM_LEVELS-1)*sizeof(node_t *));
    l->head = h;
    l->max_offset = max_offset;
    l->max_level  = max_level;

    assert((sizeof(int)+sizeof(setval_t) + sizeof(setkey_t) + 44) == CACHE_LINE_SIZE);
    printf("nodesize: %d\n", (int) sizeof(node_t));

    return l;
}

void 
set_update(set_t *l, setkey_t k, setval_t v)
{
    ptst_t    *ptst;
    sh_node_pt preds[NUM_LEVELS], succs[NUM_LEVELS];
    sh_node_pt pred, succ, new = NULL, new_next, old_next;
    int        i, level;
    sh_node_pt x, x_next;
    int loop0;
    
    ptst = critical_enter();
    
 retry:
    loop0 = 0;
    /* Initialise a new node for insertion. */
    if ( new == NULL )
    {
        new    = alloc_node(ptst);
        new->k = k;
        new->v = v;
    }
    level = new->level;

    weak_search_predecessors(l, k, preds, succs, 0);
    succ = succs[0];
    
    /* If successors don't change, this saves us some CAS operations. */
    for ( i = 0; i < level; i++ )
    {
        new->next[i] = succs[i];
    }

    /* We've committed when we've inserted at level 1. */
    WMB_NEAR_CAS(); /* make sure node fully initialised before inserting */
    old_next = CASPO(&preds[0]->next[0], succ, new);
    if ( old_next != succ )
    {
	/* either succ has been deleted (modifying preds[0]),
	 * or another insert has succeeded */
	if (is_marked_ref(preds[0]->next[0])) {
	    /* we don't aim at being inserted but at the lowest level */
	    new->level = 1;
	    x = get_unmarked_ref(preds[0]->next[0]);
	    do {
		loop0 ++;
		if (loop0 > 10) {
		    x = weak_search_head(l);
		    loop0 = 0;
		}
		x_next = x->next[0];
		if (!is_marked_ref(x_next)) {
		    /* The marker is on the preceding pointer. 
		     * Let's try to squeeze in here. */
		    new->next[0] = x_next;
		    if((old_next = CASPO(&x->next[0], x_next, new)) == x_next)
			goto success;
		} 
		x = get_unmarked_ref(x_next);
	    } while(1);
	} else {
	    /* competing insert. */
	    weak_search_predecessors(l, k, preds, succs, 0);
	    succ = succs[0];
	    goto retry;
	}
    }
    
    
    /* Insert at each of the other levels in turn. */
    i = 1;
    while ( i < level )
    {
        pred = preds[i];
        succ = succs[i];

        /* Someone *can* delete @new under our feet! */

        if ( new->k != k) goto success;

        /* Ensure forward pointer of new node is up to date. */
	new_next = new->next[i];
	if ( new_next != succ )
        {
            old_next = CASPO(&new->next[i], new_next, succ);
            if ( is_marked_ref(old_next) ) goto success;
            assert(old_next == new_next);
        }

        /* Ensure we have unique key values at every level. */
        assert(pred->k <= k);
        /* Replumb predecessor's forward pointer. */
        old_next = CASPO(&pred->next[i], succ, new);
        if ( old_next != succ )
        {
	    RMB(); /* get up-to-date view of the world. */
	    if (is_marked_ref(new->next[0]) || new->k != k) goto success;
	    
            weak_search_predecessors(l, k, preds, succs, 0);
	    succ = succs[0];
	    if (succ != new) goto success;
	    
            continue;
        }

        /* Succeeded at this level. */
        i++;
    }
success:
    critical_exit(ptst);
}

// Local cache of node.
__thread sh_node_pt pt, old_obs_hp;
__thread int old_offset;

setkey_t
set_removemin(set_t *l)
{
    setval_t   v = NULL;
    setkey_t   k = 0;
    ptst_t    *ptst;
    sh_node_pt preds[NUM_LEVELS];
    sh_node_pt x, cur, x_next, obs_hp;
    int offset = 0, lvl = 0;

    ptst = critical_enter();

    if (old_obs_hp == l->head->next[0]) {
	x = pt;
    } else {
	x = l->head;
	old_offset = 0;
	old_obs_hp = x->next[0];
    }
    //x = &l->head;
    //obs_hp = x->next[0];

    do {
	offset++;
#ifdef OPTIM
	if((x->level & LEVEL_MASK) > 2 && is_marked_ref(x->next[2]->next[0]))
	{
	    x_next = x->next[2]->next[0];
	    continue;
	}
#endif

	x_next = x->next[0]; // expensive
	// TODO: we must handle the case when we reach the end.
	assert(x_next != END);
	if (x_next == END) return NULL;
	
	if (!is_marked_ref(x_next)) { 
	    /* the marker is on the preceding pointer */
	    //x_next = FAO((sh_node_pt *)get_unmarked_ref(&x->next[0]), 1); /* linearisation */
	    x_next = __sync_fetch_and_add((uint64_t *)&x->next[0], 1);
	    /* linearisation */
	}
    }
    while ( is_marked_ref(x_next) && (x = get_unmarked_ref(x_next)));

    assert(!is_marked_ref(x_next));
    x = x_next;

    pt = x;
    old_offset += offset;

    /* save value */
    v = x_next->v;
    k = x_next->k;


    /* if the offset is big enough, try to clean up 
     * we swing the hp to the new auxiliary node x (which is deleted) 
     */
    obs_hp = old_obs_hp;
    if (old_offset <= l->max_offset || l->head->next[0] != obs_hp) goto out;

    x = get_marked_ref(x);
    /* fails if someone else already has updated the head node */
    if (obs_hp == CASPO(&l->head->next[0], obs_hp, x))
    {
	assert(old_offset > l->max_offset);
	/* Here we own every node between old hp and x.	 */

	/* retrieve the last deleted node for each level. */
	/* will not change. */
	lvl = weak_search_end(l, preds, -1);

	/* bail out if next resutructuring have started */
	if (l->head->next[0] != x) goto out;
	// update upper levels
	for (int i = lvl; i > 0; i--) {
	    sh_node_pt next = l->head->next[i];
	    while (next != CASPO(&l->head->next[i], next, preds[i]->next[i]))
	    { 
		// we have a new beginning of the list.
		weak_search_end(l, preds, i);
		next = l->head->next[i];
	    }
	}
	/* We successfully swung the top head pointer.
	 * Recycle all nodes from head pointer to the new logical head. */
	x_next = get_unmarked_ref(obs_hp);
	while (x_next != get_unmarked_ref(x)) {
	    free_node(ptst, x_next);
	    x_next = get_unmarked_ref(x_next->next[0]);
	}
    }
out:
    
    critical_exit(ptst);
    
    return k;
}

setval_t
set_remove(set_t *l, setkey_t key)
{
    node_t *node;
    
    node = weak_search_predecessors(l, key, NULL, NULL, 1);
    node = FAO(&node->next[0], 1);
    if (is_marked_ref(node)) return NULL;
    
    return node;
}

unsigned int
num_digits (unsigned int i)
{
    return i > 0 ? (int) log10 ((double) i) + 1 : 1;
}

unsigned int
highest_level(set_t *l) {
    sh_node_pt x, x_next;
    x = l->head;
    for ( int i = l->max_level - 1; i >= 1; i-- )
    {
	x_next = x->next[i];
	if (x_next->next[0] != END)
	    return i;
	
    }
    return 0;
}

void
pprint (set_t *q)
{
    char *keyfrm = "%1.1f ";
    char *keyfrm1 = "-> %1.1f ";
    //char *keyfrm = "%3d ";
    sh_node_pt n, bottom;
    char *seps[] = {"-------", "----", "---", "--"};
    unsigned int lvl = highest_level(q);

    printf("\n");
    for (int i = lvl; i >= 0; i--) {
	/* start at top level */
	printf("l%2d: ", i);

	n = q->head;
	printf(keyfrm, n->k);

	

	n = get_unmarked_ref(q->head->next[i]);
	bottom = get_unmarked_ref(q->head->next[0]);
    
	/* while toplevel has not reached end node... */
	while (n != END) {
	    /* and while bottom level has intermediate nodes */
	    while (bottom != n)
	    {
		/* print arrows */
		//printf("%s", seps[1 -1 ]);
		printf("-------");
		
		bottom = get_unmarked_ref(bottom->next[0]);
	    }
	    /* bottom == n */
	    
	    printf(keyfrm1, n->k);
	    
	    bottom = get_unmarked_ref(bottom->next[0]);
	    n = get_unmarked_ref(n->next[i]);
	}
	printf(" -|\n");
    }

    printf("top: ");
    n = q->head;
    while (n != END) {
	printf(" t%2d,d%d", (int) n->level & LEVEL_MASK, is_marked_ref(n->next[0]));
	n = get_unmarked_ref(n->next[0]);
    }
    printf("\n\n");
    
}

void
seq_test ()
{
    _init_ptst_subsystem();
    _init_gc_subsystem();
    _init_set_subsystem();

    set_t *q = set_alloc(5, 6);

    set_update(q, (double)5.1, (void *)5);
    set_update(q, (double)7, (void *)7);
    set_removemin(q);
    set_update(q, 6, (void *)6);
    set_update(q, 4, (void *)4);
    set_removemin(q);
    set_update(q, 10, (void *)10);
    set_update(q, 9, (void *)9);
    
    set_remove(q, 10);
    set_update(q, 4, (void *)4);
    set_removemin(q);
    set_removemin(q);
    set_removemin(q);
    set_update(q, 4, (void *)4);
    pprint(q);
    set_removemin(q);
    pprint(q);
    
}

void _init_set_subsystem(void)
{
    int i;

    for ( i = 0; i < NUM_LEVELS; i++ )
    {
	// aligned memory
        gc_id[i] = gc_add_allocator(64*((63 + sizeof(node_t) + i*sizeof(node_t *))/64));
    }
}






