/*  $Id$

    Part of SWI-Prolog

    Author:        Jan Wielemaker
    E-mail:        J.Wielemaker@cs.vu.nl
    WWW:           http://www.swi-prolog.org
    Copyright (C): 2002-2010, University of Amsterdam
			      VU University Amsterdam

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define WITH_MD5 1
#define WITH_PL_MUTEX 1
#define _GNU_SOURCE 1			/* get rwlocks from glibc */

#ifdef _REENTRANT
#ifdef __WINDOWS__
#include <malloc.h>			/* alloca() */
#define inline __inline
#ifndef SIZEOF_LONG
#define SIZEOF_LONG 4
#endif
#else
#if (!defined(__GNUC__) || defined(__hpux)) && defined(HAVE_ALLOCA_H)
#include <alloca.h>
#endif
#include <errno.h>
#endif
#endif

#include "rdf_db.h"
#include <wctype.h>
#include <ctype.h>
#ifdef WITH_MD5
#include "md5.h"
#include "murmur.h"
#include "memory.h"
#include "buffer.h"

#undef ERROR				/* also in wingdi.h; we do not care */
#define ERROR -1

static void md5_triple(triple *t, md5_byte_t *digest);
static void sum_digest(md5_byte_t *digest, md5_byte_t *add);
static void dec_digest(md5_byte_t *digest, md5_byte_t *add);
#endif


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
The ids form a mask. This must be kept consistent with monitor_mask/2 in
rdf_db.pl!
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

typedef enum
{ EV_ASSERT      = 0x0001,		/* triple */
  EV_ASSERT_LOAD = 0x0002,		/* triple */
  EV_RETRACT     = 0x0004,		/* triple */
  EV_UPDATE      = 0x0008,		/* old, new */
  EV_NEW_LITERAL = 0x0010,		/* literal */
  EV_OLD_LITERAL = 0x0020,		/* literal */
  EV_TRANSACTION = 0x0040,		/* id, begin/end */
  EV_LOAD	 = 0x0080,		/* id, begin/end */
  EV_REHASH	 = 0x0100		/* begin/end */
} broadcast_id;

static int broadcast(broadcast_id id, void *a1, void *a2);


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
We now use malloc/free/realloc  calls  with   explicit  sizes  to  allow
maintaining statistics as well as to   prepare  for dealing with special
memory  pools  associated  with  databases.  Using  -DDIRECT_MALLOC  the
library uses plain malloc to facilitate malloc debuggers.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#ifdef DIRECT_MALLOC

#define rdf_malloc(db, size)		malloc(size)
#define rdf_free(db, ptr, size)		free(ptr)
#define rdf_realloc(db, ptr, old, new)  realloc(ptr, new)

#else /*DIRECT_MALLOC*/

#if CHECK_MALLOC_SIZES
void *
rdf_malloc(rdf_db *db, size_t size)
{ size_t bytes = size + sizeof(size_t);
  size_t *ptr = PL_malloc(bytes);

  *ptr++ = size;
  if ( db )
    db->core += size;

  return ptr;
}

void
rdf_free(rdf_db *db, void *ptr, size_t size)
{ size_t *p = ptr;

  assert(p[-1] == size);

  db->core -= size;
  PL_free(&p[-1]);
}


void *
rdf_realloc(rdf_db *db, void *ptr, size_t old, size_t new)
{ size_t *p = ptr;
  size_t bytes = new + sizeof(size_t);

  assert(p[-1] == old);
  p = PL_realloc(&p[-1], bytes);
  *p++ = new;
  db->core += new-old;

  return p;
}

#else /*CHECK_MALLOC_SIZES*/

void *
rdf_malloc(rdf_db *db, size_t size)
{ if ( db )
    db->core += size;

//  return PL_malloc_atomic_uncollectable(size);
  return PL_malloc_unmanaged(size);
}

void
rdf_free(rdf_db *db, void *ptr, size_t size)
{ db->core -= size;

  PL_free(ptr);
}


void *
rdf_realloc(rdf_db *db, void *ptr, size_t old, size_t new)
{ db->core += new-old;

  return PL_realloc(ptr, new);
}

#endif /*CHECK_MALLOC_SIZES*/
#endif /*DIRECT_MALLOC*/

static functor_t FUNCTOR_literal1;
static functor_t FUNCTOR_literal2;
static functor_t FUNCTOR_colon2;

static functor_t FUNCTOR_triples1;
static functor_t FUNCTOR_triples2;
static functor_t FUNCTOR_resources1;
static functor_t FUNCTOR_predicates1;
static functor_t FUNCTOR_duplicates1;
static functor_t FUNCTOR_literals1;
static functor_t FUNCTOR_subject1;
static functor_t FUNCTOR_predicate1;
static functor_t FUNCTOR_object1;
static functor_t FUNCTOR_graph1;
static functor_t FUNCTOR_indexed16;
static functor_t FUNCTOR_hash_quality1;
static functor_t FUNCTOR_hash3;

static functor_t FUNCTOR_exact1;
static functor_t FUNCTOR_plain1;
static functor_t FUNCTOR_substring1;
static functor_t FUNCTOR_word1;
static functor_t FUNCTOR_prefix1;
static functor_t FUNCTOR_like1;
static functor_t FUNCTOR_le1;
static functor_t FUNCTOR_between2;
static functor_t FUNCTOR_ge1;

static functor_t FUNCTOR_symmetric1;
static functor_t FUNCTOR_inverse_of1;
static functor_t FUNCTOR_transitive1;
static functor_t FUNCTOR_rdf_subject_branch_factor1;    /* S --> BF*O */
static functor_t FUNCTOR_rdf_object_branch_factor1;	/* O --> BF*S */
static functor_t FUNCTOR_rdfs_subject_branch_factor1;	/* S --> BF*O */
static functor_t FUNCTOR_rdfs_object_branch_factor1;	/* O --> BF*S */

static functor_t FUNCTOR_searched_nodes1;
static functor_t FUNCTOR_lang2;
static functor_t FUNCTOR_type2;

static functor_t FUNCTOR_gc2;
static functor_t FUNCTOR_core1;

static functor_t FUNCTOR_assert4;
static functor_t FUNCTOR_retract4;
static functor_t FUNCTOR_update5;
static functor_t FUNCTOR_new_literal1;
static functor_t FUNCTOR_old_literal1;
static functor_t FUNCTOR_transaction2;
static functor_t FUNCTOR_load2;
static functor_t FUNCTOR_rehash1;
static functor_t FUNCTOR_begin1;
static functor_t FUNCTOR_end1;

static atom_t   ATOM_user;
static atom_t	ATOM_exact;
static atom_t	ATOM_plain;
static atom_t	ATOM_prefix;
static atom_t	ATOM_substring;
static atom_t	ATOM_word;
static atom_t	ATOM_like;
static atom_t	ATOM_error;
static atom_t	ATOM_begin;
static atom_t	ATOM_end;
static atom_t	ATOM_infinite;

static atom_t	ATOM_subPropertyOf;

static predicate_t PRED_call1;

#define MATCH_EXACT		0x01	/* exact triple match */
#define MATCH_SUBPROPERTY	0x02	/* Use subPropertyOf relations */
#define MATCH_SRC		0x04	/* Match graph location */
#define MATCH_INVERSE		0x08	/* use symmetric match too */
#define MATCH_QUAL		0x10	/* Match qualifiers too */
#define MATCH_DUPLICATE		(MATCH_EXACT|MATCH_QUAL)

static int match_triples(triple *t, triple *p, unsigned flags);
static int update_duplicates_add(rdf_db *db, triple *t);
static void update_duplicates_del(rdf_db *db, triple *t);
static void unlock_atoms(rdf_db *db, triple *t);
static void lock_atoms(rdf_db *db, triple *t);
static void unlock_atoms_literal(literal *lit);

static size_t	triple_hash_key(triple *t, int which);
static size_t	object_hash(triple *t);
static void	link_triple_hash(rdf_db *db, triple *t);
static void	init_triple_walker(triple_walker *tw, rdf_db *db,
				   triple *t, int index);
static triple  *next_triple(triple_walker *tw);
static void	free_triple(rdf_db *db, triple *t, int linger);

static void	create_reachability_matrix(rdf_db *db, predicate_cloud *cloud);
static int	get_predicate(rdf_db *db, term_t t, predicate **p);
static predicate_cloud *new_predicate_cloud(rdf_db *db, predicate **p, size_t count);
static int	unify_literal(term_t lit, literal *l);
static int	check_predicate_cloud(predicate_cloud *c);


		 /*******************************
		 *	       LOCKING		*
		 *******************************/

#define LOCK_LIT(db)			simpleMutexLock(&db->locks.literal)
#define UNLOCK_LIT(db)			simpleMutexUnlock(&db->locks.literal)

static void
INIT_LOCK(rdf_db *db)
{ simpleMutexInit(&db->locks.literal);
  simpleMutexInit(&db->locks.misc);
  simpleMutexInit(&db->locks.gc);
}


		 /*******************************
		 *	   DEBUG SUPPORT	*
		 *******************************/

#ifdef O_DEBUG

#define PRT_SRC	0x1

static void
print_literal(literal *lit)
{ switch(lit->objtype)
  { case OBJ_STRING:
      switch(lit->qualifier)
      { case Q_TYPE:
	  Sdprintf("%s^^\"%s\"",
		   PL_atom_chars(lit->value.string),
		   PL_atom_chars(lit->type_or_lang));
	  break;
	case Q_LANG:
	  Sdprintf("%s@\"%s\"",
		   PL_atom_chars(lit->value.string),
		   PL_atom_chars(lit->type_or_lang));
	  break;
	default:
	{ size_t len;
	  const char *s;
	  const wchar_t *w;

	  if ( (s = PL_atom_nchars(lit->value.string, &len)) )
	  { if ( strlen(s) == len )
	      Sdprintf("\"%s\"", s);
	    else
	      Sdprintf("\"%s\" (len=%d)", s, len);
	  } else if ( (w = PL_atom_wchars(lit->value.string, &len)) )
	  { unsigned int i;
	    Sputc('L', Serror);
	    Sputc('"', Serror);
	    for(i=0; i<len; i++)
	    { if ( w[i] < 0x7f )
		Sputc(w[i], Serror);
	      else
		Sfprintf(Serror, "\\\\u%04x", w[i]);
	    }
	    Sputc('"', Serror);
	  }
	  break;
	}
      }
      break;
    case OBJ_INTEGER:
      Sdprintf("%ld", lit->value.integer);
      break;
    case OBJ_DOUBLE:
      Sdprintf("%f", lit->value.real);
      break;
    case OBJ_TERM:
    { fid_t fid = PL_open_foreign_frame();
      term_t term = PL_new_term_ref();

      PL_recorded_external(lit->value.term.record, term);
      PL_write_term(Serror, term, 1200,
		    PL_WRT_QUOTED|PL_WRT_NUMBERVARS|PL_WRT_PORTRAY);
      PL_discard_foreign_frame(fid);
      break;
    }
    default:
      assert(0);
  }
}


static void
print_object(triple *t)
{ if ( t->object_is_literal )
  { print_literal(t->object.literal);
  } else
  { Sdprintf("%s", PL_atom_chars(t->object.resource));
  }
}


static void
print_src(triple *t)
{ if ( t->line == NO_LINE )
    Sdprintf(" [%s]", PL_atom_chars(t->graph));
  else
    Sdprintf(" [%s:%ld]", PL_atom_chars(t->graph), t->line);
}


static void
print_triple(triple *t, int flags)
{ Sdprintf("<%s %s ",
	   PL_atom_chars(t->subject),
	   PL_atom_chars(t->predicate.r->name));
  print_object(t);
  if ( (flags & PRT_SRC) )
    print_src(t);
  Sdprintf(">");
}

#endif

		 /*******************************
		 *	     STORAGE		*
		 *******************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Tables that allow finding the hash-chains   for a particular index. They
are currently crafted by hand, such that the compiler knowns the mapping
is  constant.  check_index_tables()  verifies  that    the   tables  are
consistent.  To add an index:

    * Increment INDEX_TABLES in rdf_db.h
    * Add the index to col_index[]
    * Assign it a (consistent) position in index_col[]
    * If decide wich unindexed queries are best mapped
      to the new index and add them to alt_index[]
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#define ICOL(i) (index_col[i])

static const int index_col[16] =
{ 0,					/* BY_NONE */
  1,					/* BY_S */
  2,					/* BY_P */
  3,					/* BY_SP */
  4,					/* BY_O */
  ~0,					/* BY_SO */
  5,					/* BY_PO */
  6,					/* BY_SPO */

  7,					/* BY_G */
  8,					/* BY_SG */
  9,					/* BY_PG */
 ~0,					/* BY_SPG */
 ~0,					/* BY_OG */
 ~0,					/* BY_SOG */
 ~0,					/* BY_POG */
 ~0					/* BY_SPOG */
};

static int col_index[INDEX_TABLES] =
{ BY_NONE,
  BY_S,
  BY_P,
  BY_SP,
  BY_O,
  BY_PO,
  BY_SPO,
  BY_G,
  BY_SG,
  BY_PG
};

static const char *col_name[INDEX_TABLES] =
{ "-",
  "S",
  "P",
  "SP",
  "O",
  "PO",
  "SPO",
  "G",
  "SG",
  "PG"
};

static const int alt_index[16] =
{ BY_NONE,				/* BY_NONE */
  BY_S,					/* BY_S */
  BY_P,					/* BY_P */
  BY_SP,				/* BY_SP */
  BY_O,					/* BY_O */
  BY_S,					/* BY_SO */
  BY_PO,				/* BY_PO */
  BY_SPO,				/* BY_SPO */

  BY_G,					/* BY_G */
  BY_SG,				/* BY_SG */
  BY_PG,				/* BY_PG */
  BY_SP,				/* BY_SPG */
  BY_O,					/* BY_OG */
  BY_S,					/* BY_SOG */
  BY_PO,				/* BY_POG */
  BY_SPO				/* BY_SPOG */
};


static void
check_index_tables()
{ int i, ic;

  for(i=0; i<16; i++)
  { if ( (ic=index_col[i]) != ~0 )
    { assert(col_index[ic] == i);
    }
  }

  for(i=0; i<16; i++)
  { int ai = alt_index[i];

    assert(index_col[ai] != ~0);
  }

  for(i=0; i<INDEX_TABLES; i++)
  { ic = col_index[i];
    assert(alt_index[ic] == ic);
  }
}


		 /*******************************
		 *	      LISTS		*
		 *******************************/

static int
add_list(rdf_db *db, list *list, void *value)
{ cell *c;

  for(c=list->head; c; c=c->next)
  { if ( c->value == value )
      return FALSE;			/* already a member */
  }

  c = rdf_malloc(db, sizeof(*c));
  c->value = value;
  c->next = NULL;

  if ( list->tail )
    list->tail->next = c;
  else
    list->head = c;

  list->tail = c;

  return TRUE;
}


static int
del_list(rdf_db *db, list *list, void *value)
{ cell *c, *p = NULL;

  for(c=list->head; c; p=c, c=c->next)
  { if ( c->value == value )
    { if ( p )
	p->next = c->next;
      else
	list->head = c->next;

      if ( !c->next )
	list->tail = p;

      rdf_free(db, c, sizeof(*c));

      return TRUE;
    }
  }

  return FALSE;				/* not a member */
}


static void
free_list(rdf_db *db, list *list)
{ cell *c, *n;

  for(c=list->head; c; c=n)
  { n = c->next;
    rdf_free(db, c, sizeof(*c));
  }

  list->head = list->tail = NULL;
}


		 /*******************************
		 *	     ATOM SETS		*
		 *******************************/

#define CHUNKSIZE 4000				/* normally a page */
#define ATOMSET_INITIAL_ENTRIES 16

typedef struct mchunk
{ struct mchunk *next;
  size_t used;
  char buf[CHUNKSIZE];
} mchunk;

typedef struct atom_cell
{ struct atom_cell *next;
  atom_t     atom;
} atom_cell;

typedef struct
{ atom_cell **entries;				/* Hash entries */
  size_t      size;				/* Hash-table size */
  size_t      count;				/* # atoms stored */
  mchunk     *node_store;
  mchunk      store0;
  atom_cell  *entries0[ATOMSET_INITIAL_ENTRIES];
} atomset;


static void *
alloc_atomset(void *ptr, size_t size)
{ void *p;
  atomset *as = ptr;

  assert(size < CHUNKSIZE);

  if ( as->node_store->used + size > CHUNKSIZE )
  { mchunk *ch = malloc(sizeof(mchunk));

    ch->used = 0;
    ch->next = as->node_store;
    as->node_store = ch;
  }

  p = &as->node_store->buf[as->node_store->used];
  as->node_store->used += size;

  return p;
}


static void
init_atomset(atomset *as)
{ as->node_store = &as->store0;
  as->node_store->next = NULL;
  as->node_store->used = 0;

  memset(as->entries0, 0, sizeof(as->entries0));
  as->entries = as->entries0;
  as->size = ATOMSET_INITIAL_ENTRIES;
  as->count = 0;
}


static void
destroy_atomset(atomset *as)
{ mchunk *ch, *next;

  for(ch=as->node_store; ch != &as->store0; ch = next)
  { next = ch->next;
    free(ch);
  }

  if ( as->entries != as->entries0 )
    free(as->entries);
}


static void
rehash_atom_set(atomset *as)
{ size_t newsize = as->size*2;
  atom_cell **new = malloc(newsize*sizeof(atom_cell*));
  int i;

  memset(new, 0, newsize*sizeof(atom_cell*));

  for(i=0; i<as->size; i++)
  { atom_cell *c, *n;

    for(c=as->entries[i]; c; c=n)
    { size_t inew = atom_hash(c->atom)&(newsize-1);

      n = c->next;
      c->next = new[inew];
      new[inew] = c;
    }
  }

  if ( as->entries == as->entries0 )
  { as->entries = new;
  } else
  { atom_cell **old = as->entries;
    as->entries = new;
    free(old);
  }
}


static int
add_atomset(atomset *as, atom_t atom)
{ size_t i = atom_hash(atom)&(as->size-1);
  atom_cell *c;

  for(c=as->entries[i]; c; c=c->next)
  { if ( c->atom == atom )
      return 0;
  }

  if ( ++as->count > 2*as->size )
  { rehash_atom_set(as);
    i = atom_hash(atom)&(as->size-1);
  }

  c = alloc_atomset(as, sizeof(*c));
  c->atom = atom;
  c->next = as->entries[i];
  as->entries[i] = c;

  return 1;
}


		 /*******************************
		 *	    PREDICATES		*
		 *******************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Predicates are represented as first class   citizens  for three reasons:
quickly  answer  on  the  transitive   rdfs:subPropertyOf  relation  for
rdf_hash/3,  keep  track  of  statistics  that   are  useful  for  query
optimization  (#triples,  branching   factor)    and   keep   properties
(inverse/transitive).

To answer the rdfs:subPropertyOf quickly,   predicates  are organised in
`clouds', where a cloud defines a   set  of predicates connected through
rdfs:subPropertyOf triples. The cloud numbers  its members and maintains
a bit-matrix that contains the closure  of the reachability. Initially a
predicate has a simple cloud of size 1. merge_clouds() and split_cloud()
deals with adding  and  deleting   rdfs:subPropertyOf  relations.  These
operations try to modify the clouds that have   no triples, so it can be
done without a rehash. If this fails, the predicates keep their own hash
to make search without rdfs:subPropertyOf  still   possible  (so  we can
avoid frequent updates while loading triples),   sets  the cloud `dirty'
flag and the DB's need_update flag. Queries that need rdfs:subPropertyOf
find the need_update flag,  which   calls  organise_predicates(),  which
cause a rehash if some predicates  have   changed  hash-code  to the new
cloud they have become part of.

TBD: We can do a partial re-hash in that case!
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static int
init_pred_table(rdf_db *db)
{ size_t bytes = sizeof(predicate**)*INITIAL_PREDICATE_TABLE_SIZE;
  predicate **p = PL_malloc_uncollectable(bytes);
  int i, count = INITIAL_PREDICATE_TABLE_SIZE;

  memset(p, 0, bytes);
  for(i=0; i<MSB(count); i++)
    db->predicates.blocks[i] = p;

  db->predicates.bucket_count       = count;
  db->predicates.bucket_count_epoch = count;
  db->predicates.count              = 0;

  return TRUE;
}


static int
resize_pred_table(rdf_db *db)
{ int i = MSB(db->predicates.bucket_count);
  size_t bytes  = sizeof(predicate**)*db->predicates.bucket_count;
  predicate **p = PL_malloc_uncollectable(bytes);

  memset(p, 0, bytes);
  db->predicates.blocks[i] = p-db->predicates.bucket_count;
  db->predicates.bucket_count *= 2;
  DEBUG(1, Sdprintf("Resized predicate table to %ld\n",
		    (long)db->predicates.bucket_count));

  return TRUE;
}


typedef struct pred_walker
{ rdf_db       *db;			/* RDF DB */
  atom_t	name;			/* Name of the predicate */
  size_t	unbounded_hash;		/* Atom's hash */
  size_t	bcount;			/* current bucket count */
  predicate    *current;		/* current location */
} pred_walker;


static void
init_predicate_walker(pred_walker *pw, rdf_db *db, atom_t name)
{ pw->db	     = db;
  pw->name	     = name;
  pw->unbounded_hash = atom_hash(name);
  pw->bcount	     = db->predicates.bucket_count_epoch;
  pw->current	     = NULL;
}

static predicate*
next_predicate(pred_walker *pw)
{ predicate *p;

  if ( pw->current )
  { p = pw->current;
    pw->current = p->next;
  } else if ( pw->bcount <= pw->db->predicates.bucket_count )
  { do
    { int entry = pw->unbounded_hash % pw->bcount;
      p = pw->db->predicates.blocks[MSB(entry)][entry];
      pw->bcount *= 2;
    } while(!p && pw->bcount <= pw->db->predicates.bucket_count );

    if ( p )
      pw->current = p->next;
  } else
    return NULL;

  return p;
}


static predicate *
existing_predicate(rdf_db *db, atom_t name)
{ pred_walker pw;
  predicate *p;

  init_predicate_walker(&pw, db, name);
  while((p=next_predicate(&pw)))
  { if ( p->name == name )
      return p;
  }

  return NULL;
}


predicate *
lookup_predicate(rdf_db *db, atom_t name)
{ predicate *p, **pp;
  predicate_cloud *cp;
  int entry;

  if ( (p=existing_predicate(db, name)) )
    return p;

  LOCK_MISC(db);
  if ( (p=existing_predicate(db, name)) )
  { UNLOCK_MISC(db);
    return p;
  }

  p = rdf_malloc(db, sizeof(*p));
  memset(p, 0, sizeof(*p));
  p->name = name;
  cp = new_predicate_cloud(db, &p, 1);
  p->hash = cp->hash;
  PL_register_atom(name);
  if ( db->predicates.count > db->predicates.bucket_count )
    resize_pred_table(db);
  entry = atom_hash(name) % db->predicates.bucket_count;
  pp = &db->predicates.blocks[MSB(entry)][entry];
  p->next = *pp;
  *pp = p;
  db->predicates.count++;
  DEBUG(5, Sdprintf("Pred %s (count = %d)\n",
		    PL_atom_chars(name), db->predicates.count));
  UNLOCK_MISC(db);

  return p;
}


static const char *
pname(predicate *p)
{ if ( p->name )
    return PL_atom_chars(p->name);
  else
  { static char *ring[10];
    static int ri = 0;
    char buf[25];
    char *r;

    Ssprintf(buf, "__D%p", p);
    ring[ri++] = r = strdup(buf);
    if ( ri == 10 )
    { ri = 0;
      free(ring[ri]);
    }

    return (const char*)r;
  }
}


static int
organise_predicates(rdf_db *db)		/* TBD: rename&move */
{ int i;
  int changed = 0;

  DEBUG(2, Sdprintf("rdf_db: fixing predicate clouds\n"));

  for(i=0; i<db->predicates.bucket_count; i++)
  { predicate *p = db->predicates.blocks[MSB(i)][i];

    for( ; p; p = p->next )
    { predicate_cloud *cloud = p->cloud;

      if ( cloud->dirty )
      { predicate **cp;
	int i2;

	for(i2=0, cp = cloud->members; i2 < cloud->size; i2++, cp++)
	{ if ( (*cp)->hash != cloud->hash )
	  { (*cp)->hash = cloud->hash;
	    if ( (*cp)->triple_count > 0 )
	      changed++;
	  }
	}
	cloud->dirty = FALSE;
      }
    }
  }

  return changed;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Keep track of the triple count. This   now uses GCC atomic instructions.
We need to provide emulations thereof in  a portability header. See also
MSB().
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static inline void
register_predicate(rdf_db *db, triple *t)
{ __sync_add_and_fetch(&t->predicate.r->triple_count, 1);
}


static inline void
unregister_predicate(rdf_db *db, triple *t)
{ __sync_sub_and_fetch(&t->predicate.r->triple_count, 1);
}


		 /*******************************
		 *	 PREDICATE CLOUDS	*
		 *******************************/

static predicate_cloud *
new_predicate_cloud(rdf_db *db, predicate **p, size_t count)
{ predicate_cloud *cloud = rdf_malloc(db, sizeof(*cloud));

  memset(cloud, 0, sizeof(*cloud));
  cloud->hash = rdf_murmer_hash(&cloud, sizeof(cloud), MURMUR_SEED);
  if ( count )
  { int i;
    predicate **p2;

    cloud->size = count;
    cloud->members = rdf_malloc(db, sizeof(predicate*)*count);
    memcpy(cloud->members, p, sizeof(predicate*)*count);

    for(i=0, p2=cloud->members; i<cloud->size; i++, p2++)
      (*p2)->cloud = cloud;
  }
  create_reachability_matrix(db, cloud);

  return cloud;
}


static void
free_predicate_cloud(rdf_db *db, predicate_cloud *cloud)
{ if ( cloud->members )
  { rdf_free(db, cloud->members, sizeof(predicate*)*cloud->size);
  }

  rdf_free(db, cloud, sizeof(*cloud));
}


static size_t
triples_in_predicate_cloud(predicate_cloud *cloud)
{ size_t triples = 0;
  predicate **p;
  int i;

  for(i=0, p=cloud->members; i<cloud->size; i++, p++)
    triples += (*p)->triple_count;

  return triples;
}


/* Add the predicates of c2 to c1 and destroy c2.  Returns c1 */

static predicate_cloud *
append_clouds(rdf_db *db, predicate_cloud *c1, predicate_cloud *c2, int update_hash)
{ predicate **p;
  int i;

  for(i=0, p=c2->members; i<c2->size; i++, p++)
  { (*p)->cloud = c1;
    if ( update_hash )
      (*p)->hash = c1->hash;
  }

  if ( c1->size > 0 && c2->size > 0 )
  { c1->members = rdf_realloc(db, c1->members,
			      c1->size*sizeof(predicate*),
			      (c1->size+c2->size)*sizeof(predicate*));
    memcpy(&c1->members[c1->size], c2->members, c2->size*sizeof(predicate*));
    c1->size += c2->size;
    free_predicate_cloud(db, c2);
  } else if ( c2->size > 0 )
  { c1->members = c2->members;
    c1->size = c2->size;
    c2->members = NULL;
    free_predicate_cloud(db, c2);
  } else
  { free_predicate_cloud(db, c2);
  }

  return c1;
}


/* merge two predicate clouds.  If either of them has no triples we
   can do the merge without rehashing the database.  Note that this
   code is only called from addSubPropertyOf().  If c1==c2, we added
   an rdfs:subPropertyOf between two predicates in the same cloud.
   we must still update the matrix, though we could do it a bit more
   efficient.  I doubt this is worth the trouble though.
*/

static predicate_cloud *
merge_clouds(rdf_db *db, predicate_cloud *c1, predicate_cloud *c2)
{ predicate_cloud *cloud;

  if ( c1 != c2 )
  { if ( triples_in_predicate_cloud(c1) == 0 )
    { cloud = append_clouds(db, c2, c1, TRUE);
    } else if ( triples_in_predicate_cloud(c2) == 0 )
    { cloud = append_clouds(db, c1, c2, TRUE);
    } else
    { cloud = append_clouds(db, c1, c2, FALSE);
      cloud->dirty = TRUE;
      db->need_update++;
    }
  } else
  { cloud = c1;
  }

  DEBUG(1, if ( !db->need_update )
	   { check_predicate_cloud(cloud);
	   });

  create_reachability_matrix(db, cloud);

  return cloud;
}


/* split a cloud into multiple disjoint clouds.  The first cloud is
   given the hash of the original, so we only need to update if new
   clouds are created.  Ideally we should se whether it is possible
   to give the orginal hash to the one and only non-empty cloud to
   avoid re-hashing alltogether.
*/

static void
pred_reachable(predicate *start, char *visited, predicate **nodes, int *size)
{ if ( !visited[start->label] )
  { cell *c;

    visited[start->label] = TRUE;
    nodes[(*size)++] = start;
    for(c=start->subPropertyOf.head; c; c=c->next)
      pred_reachable(c->value, visited, nodes, size);
    for(c=start->siblings.head; c; c=c->next)
      pred_reachable(c->value, visited, nodes, size);
  }
}


static int
split_cloud(rdf_db *db, predicate_cloud *cloud,
	    predicate_cloud **parts, int size)
{ char *done        = alloca(cloud->size*sizeof(char));
  predicate **graph = alloca(cloud->size*sizeof(predicate*));
  int found = 0;
  int i;

  memset(done, 0, cloud->size*sizeof(char));
  for(i=0; i<cloud->size; i++)
  { if ( !done[i] )
    { predicate *start = cloud->members[i];
      predicate_cloud *new_cloud;
      int gsize = 0;

      pred_reachable(start, done, graph, &gsize);
      new_cloud = new_predicate_cloud(db, graph, gsize);
      DEBUG(1, Sdprintf("Split cloud %d from %s --> %p with %d members\n",
			found, pname(start), new_cloud, gsize));
      if ( found == 0 )
      { new_cloud->hash = cloud->hash;
	new_cloud->dirty = cloud->dirty;
      } else
      { new_cloud->dirty = TRUE;	/* preds come from another cloud */
	db->need_update++;
      }
      parts[found++] = new_cloud;
    }
  }

  free_predicate_cloud(db, cloud);

  return found;
}


static size_t
predicate_hash(predicate *p)
{ return p->hash;
}


static void
addSubPropertyOf(rdf_db *db, predicate *sub, predicate *super)
{ /*DEBUG(2, Sdprintf("addSubPropertyOf(%s, %s)\n", pname(sub), pname(super)));*/

  if ( add_list(db, &sub->subPropertyOf, super) )
  { add_list(db, &super->siblings, sub);
    merge_clouds(db, sub->cloud, super->cloud);
  }
}


/* deleting an rdfs:subPropertyOf.  This is a bit naughty.  If the
   cloud is still connected we only need to refresh the reachability
   matrix.  Otherwise the cloud breaks in maximum two clusters.  We
   can decide to leave it as is, which saves a re-hash of the triples
   but harms indexing.  Alternative we can create a new cloud for one
   of the clusters and re-hash.
*/

static void
delSubPropertyOf(rdf_db *db, predicate *sub, predicate *super)
{ if ( del_list(db, &sub->subPropertyOf, super) )
  { del_list(db, &super->siblings, sub);
 /* if ( not worth the trouble )
      create_reachability_matrix(db, sub->cloud);
    else */
    { predicate_cloud *parts[2];
      split_cloud(db, sub->cloud, parts, 2);
    }
  }
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Reachability matrix.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#define WBITSIZE (sizeof(int)*8)

static size_t
byte_size_bitmatrix(size_t w, size_t h)
{ size_t wsize = ((w*h)+WBITSIZE-1)/WBITSIZE;

  return (size_t)(intptr_t)&((bitmatrix*)NULL)->bits[wsize];
}


static bitmatrix *
alloc_bitmatrix(rdf_db *db, size_t w, size_t h)
{ size_t size = byte_size_bitmatrix(w, h);
  bitmatrix *m = rdf_malloc(db, size);

  memset(m, 0, size);
  m->width = w;
  m->heigth = h;

  return m;
}


static void
free_bitmatrix(rdf_db *db, bitmatrix *bm)
{ size_t size = byte_size_bitmatrix(bm->width, bm->heigth);

  rdf_free(db, bm, size);
}


#undef setbit				/* conflict in HPUX 11.23 */

static void
setbit(bitmatrix *m, int i, int j)
{ size_t ij = m->width*i+j;
  size_t word = ij/WBITSIZE;
  int bit  = ij%WBITSIZE;

  m->bits[word] |= 1<<bit;
}


static int
testbit(bitmatrix *m, int i, int j)
{ size_t ij = m->width*i+j;
  size_t word = ij/WBITSIZE;
  int bit  = ij%WBITSIZE;

  return ((m->bits[word] & (1<<bit)) != 0);
}


static int
label_predicate_cloud(predicate_cloud *cloud)
{ predicate **p;
  int i;

  for(i=0, p=cloud->members; i<cloud->size; i++, p++)
    (*p)->label = i;

  return i;
}


static void
fill_reachable(bitmatrix *bm, predicate *p0, predicate *p)
{ if ( !testbit(bm, p0->label, p->label) )
  { cell *c;

    DEBUG(2, Sdprintf("    Reachable [%s (%d)]\n", pname(p), p->label));
    setbit(bm, p0->label, p->label);
    for(c = p->subPropertyOf.head; c; c=c->next)
      fill_reachable(bm, p0, c->value);
  }
}


static void
create_reachability_matrix(rdf_db *db, predicate_cloud *cloud)
{ bitmatrix *m = alloc_bitmatrix(db, cloud->size, cloud->size);
  predicate **p;
  int i;

  label_predicate_cloud(cloud);
  for(i=0, p=cloud->members; i<cloud->size; i++, p++)
  { DEBUG(1, Sdprintf("Reachability for %s (%d)\n", pname(*p), (*p)->label));

    fill_reachable(m, *p, *p);
  }

  if ( cloud->reachable )
    free_bitmatrix(db, cloud->reachable);

  cloud->reachable = m;
}


static int
isSubPropertyOf(predicate *sub, predicate *p)
{ if ( sub->cloud == p->cloud )
    return testbit(sub->cloud->reachable, sub->label, p->label);

  return FALSE;
}

		 /*******************************
		 *   PRINT PREDICATE HIERARCHY	*
		 *******************************/

static int
check_predicate_cloud(predicate_cloud *c)
{ predicate **p;
  int errors = 0;
  int i;

  DEBUG(1, if ( c->dirty ) Sdprintf("Cloud is dirty\n"));

  for(i=0, p=c->members; i<c->size; i++, p++)
  { if ( !c->dirty )
    { if ( (*p)->hash != c->hash )
      { Sdprintf("Hash of %s doesn't match cloud hash\n", pname(*p));
	errors++;
      }
    }
    if ( (*p)->cloud != c )
    { Sdprintf("Wrong cloud of %s\n", pname(*p));
      errors++;
    }
  }

  return errors;
}


static void
print_reachability_cloud(predicate *p)
{ int x, y;
  predicate_cloud *cloud = p->cloud;

  check_predicate_cloud(cloud);

  Sdprintf("Reachability matrix:\n");
  for(x=0; x<cloud->reachable->width; x++)
    Sdprintf("%d", x%10);
  Sdprintf("\n");
  for(y=0; y<cloud->reachable->heigth; y++)
  { for(x=0; x<cloud->reachable->width; x++)
    { if ( testbit(cloud->reachable, x, y) )
	Sdprintf("X");
      else
	Sdprintf(".");
    }

    Sdprintf(" %2d %s\n", y, PL_atom_chars(cloud->members[y]->name));
    assert(cloud->members[y]->label == y);
  }
}


static foreign_t
rdf_print_predicate_cloud(term_t t)
{ predicate *p;
  rdf_db *db = DB;

  if ( !get_predicate(db, t, &p) )
    return FALSE;

  print_reachability_cloud(p);

  return TRUE;
}



/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Branching  factors  are  crucial  in  ordering    the  statements  of  a
conjunction. These functions compute  the   average  branching factor in
both directions ("subject --> P  -->  object"   and  "object  -->  P -->
subject") by determining the number of unique   values at either side of
the predicate. This number  is  only   recomputed  if  it  is considered
`dirty'.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static int
update_predicate_counts(rdf_db *db, predicate *p, int which)
{ size_t total = 0;

  if ( which == DISTINCT_DIRECT )
  { size_t changed;

    if ( p->triple_count >= p->distinct_updated[DISTINCT_DIRECT] )
      changed = p->triple_count - p->distinct_updated[DISTINCT_DIRECT];
    else
      changed = p->distinct_updated[DISTINCT_DIRECT] - p->triple_count;

    if ( changed < p->distinct_updated[DISTINCT_DIRECT] )
      return TRUE;

    if ( p->triple_count == 0 )
    { p->distinct_count[which]    = 0;
      p->distinct_subjects[which] = 0;
      p->distinct_objects[which]  = 0;

      return TRUE;
    }
  } else
  { size_t changed = db->generation - p->distinct_updated[DISTINCT_SUB];

    if ( changed < p->distinct_count[DISTINCT_SUB] )
      return TRUE;
  }

  { atomset subject_set;
    atomset object_set;
    triple t;
    triple *byp;
    triple_walker tw;

    memset(&t, 0, sizeof(t));
    t.predicate.r = p;
    t.indexed |= BY_P;

    init_atomset(&subject_set);
    init_atomset(&object_set);
    init_triple_walker(&tw, db, &t, t.indexed);
    while((byp=next_triple(&tw)))
    { if ( byp->lifespan.died == GEN_MAX && !byp->is_duplicate )
      { if ( (which == DISTINCT_DIRECT && byp->predicate.r == p) ||
	     (which != DISTINCT_DIRECT && isSubPropertyOf(byp->predicate.r, p)) )
	{ total++;
	  add_atomset(&subject_set, byp->subject);
	  add_atomset(&object_set, object_hash(byp)); /* NOTE: not exact! */
	}
      }
    }

    p->distinct_count[which]    = total;
    p->distinct_subjects[which] = subject_set.count;
    p->distinct_objects[which]  = object_set.count;

    destroy_atomset(&subject_set);
    destroy_atomset(&object_set);

    if ( which == DISTINCT_DIRECT )
      p->distinct_updated[DISTINCT_DIRECT] = total;
    else
      p->distinct_updated[DISTINCT_SUB] = db->generation;

    DEBUG(1, Sdprintf("%s: distinct subjects (%s): %ld, objects: %ld\n",
		      PL_atom_chars(p->name),
		      (which == DISTINCT_DIRECT ? "rdf" : "rdfs"),
		      p->distinct_subjects[which],
		      p->distinct_objects[which]));
  }

  return TRUE;
}


static void
invalidate_distinct_counts(rdf_db *db)
{ int i;

  for(i=0; i<db->predicates.bucket_count; i++)
  { predicate *p = db->predicates.blocks[MSB(i)][i];

    for( ; p; p = p->next )
    { p->distinct_updated[DISTINCT_SUB] = 0;
      p->distinct_count[DISTINCT_SUB] = 0;
      p->distinct_subjects[DISTINCT_SUB] = 0;
      p->distinct_objects[DISTINCT_SUB] = 0;
    }
  }
}


static double
subject_branch_factor(rdf_db *db, predicate *p, int which)
{ if ( !update_predicate_counts(db, p, which) )
    return FALSE;

  if ( p->distinct_subjects[which] == 0 )
    return 0.0;				/* 0 --> 0 */

  return (double)p->distinct_count[which] /
         (double)p->distinct_subjects[which];
}


static double
object_branch_factor(rdf_db *db, predicate *p, int which)
{ if ( !update_predicate_counts(db, p, which) )
    return FALSE;

  if ( p->distinct_objects[which] == 0 )
    return 0.0;				/* 0 --> 0 */

  return (double)p->distinct_count[which] /
         (double)p->distinct_objects[which];
}




		 /*******************************
		 *	   NAMED GRAPHS		*
		 *******************************/

/* MT: all calls must be locked
*/

static int
init_graph_table(rdf_db *db)
{ size_t bytes = sizeof(graph**)*INITIAL_PREDICATE_TABLE_SIZE;
  graph **p = PL_malloc_uncollectable(bytes);
  int i, count = INITIAL_PREDICATE_TABLE_SIZE;

  memset(p, 0, bytes);
  for(i=0; i<MSB(count); i++)
    db->graphs.blocks[i] = p;

  db->graphs.bucket_count       = count;
  db->graphs.bucket_count_epoch = count;
  db->graphs.count              = 0;

  return TRUE;
}


static int
resize_graph_table(rdf_db *db)
{ int i = MSB(db->graphs.bucket_count);
  size_t bytes  = sizeof(graph**)*db->graphs.bucket_count;
  graph **p = PL_malloc_uncollectable(bytes);

  memset(p, 0, bytes);
  db->graphs.blocks[i] = p-db->graphs.bucket_count;
  db->graphs.bucket_count *= 2;
  DEBUG(1, Sdprintf("Resized graph table to %ld\n",
		    (long)db->graphs.bucket_count));

  return TRUE;
}


typedef struct graph_walker
{ rdf_db       *db;			/* RDF DB */
  atom_t	name;			/* Name of the graph */
  size_t	unbounded_hash;		/* Atom's hash */
  size_t	bcount;			/* current bucket count */
  graph	       *current;		/* current location */
} graph_walker;


static void
init_graph_walker(graph_walker *gw, rdf_db *db, atom_t name)
{ gw->db	     = db;
  gw->name	     = name;
  gw->unbounded_hash = atom_hash(name);
  gw->bcount	     = db->graphs.bucket_count_epoch;
  gw->current	     = NULL;
}

static graph*
next_graph(graph_walker *gw)
{ graph *g;

  if ( gw->current )
  { g = gw->current;
    gw->current = g->next;
  } else if ( gw->bcount <= gw->db->graphs.bucket_count )
  { do
    { int entry = gw->unbounded_hash % gw->bcount;
      g = gw->db->graphs.blocks[MSB(entry)][entry];
      gw->bcount *= 2;
    } while(!g && gw->bcount <= gw->db->graphs.bucket_count );

    if ( g )
      gw->current = g->next;
  } else
    return NULL;

  return g;
}


static graph *
existing_graph(rdf_db *db, atom_t name)
{ graph_walker gw;
  graph *g;

  init_graph_walker(&gw, db, name);
  while((g=next_graph(&gw)))
  { if ( g->name == name )
      return g;
  }

  return g;
}


static graph *
lookup_graph(rdf_db *db, atom_t name)
{ graph *g, **gp;
  int entry;

  if ( (g=existing_graph(db, name)) )
    return g;

  LOCK_MISC(db);
  if ( (g=existing_graph(db, name)) )
  { UNLOCK_MISC(db);
    return g;
  }

  g = rdf_malloc(db, sizeof(*g));
  memset(g, 0, sizeof(*g));
  g->name = name;
  g->md5 = TRUE;
  PL_register_atom(name);
  if ( db->graphs.count > db->graphs.bucket_count )
    resize_graph_table(db);
  entry = atom_hash(name) % db->graphs.bucket_count;
  gp = &db->graphs.blocks[MSB(entry)][entry];
  g->next = *gp;
  *gp = g;
  db->graphs.count++;
  UNLOCK_MISC(db);

  return g;
}


static void
erase_graphs(rdf_db *db)
{ int i;

  for(i=0; i<db->graphs.bucket_count; i++)
  { graph *n, *g = db->graphs.blocks[MSB(i)][i];

    db->graphs.blocks[MSB(i)][i] = NULL;

    for( ; g; g = n )
    { n = g->next;

      PL_unregister_atom(g->name);
      if ( g->source )
	PL_unregister_atom(g->source);
      rdf_free(db, g, sizeof(*g));
    }
  }

  db->graphs.count = 0;
  db->last_graph = NULL;
}


static void
register_graph(rdf_db *db, triple *t)
{ graph *src;

  if ( !t->graph )
    return;

  if ( db->last_graph && db->last_graph->name == t->graph )
  { src = db->last_graph;
  } else
  { src = lookup_graph(db, t->graph);
    db->last_graph = src;
  }

  src->triple_count++;
#ifdef WITH_MD5
  if ( src->md5 )
  { md5_byte_t digest[16];
    md5_triple(t, digest);
    sum_digest(src->digest, digest);
  }
#endif
}


static void
unregister_graph(rdf_db *db, triple *t)
{ graph *src;

  if ( !t->graph )
    return;

  if ( db->last_graph && db->last_graph->name == t->graph )
  { src = db->last_graph;
  } else
  { src = lookup_graph(db, t->graph);
    db->last_graph = src;
  }

  src->triple_count--;
#ifdef WITH_MD5
  if ( src->md5 )
  { md5_byte_t digest[16];
    md5_triple(t, digest);
    dec_digest(src->digest, digest);
  }
#endif
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
rdf_graph(-Graph) is nondet.

Return a list holding the names  of   all  currently defined graphs. We
return a list to avoid the need for complicated long locks.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

typedef struct enum_graph
{ graph *g;
  int i;
} enum_graph;

static foreign_t
rdf_graph(term_t name, control_t h)
{ rdf_db *db = DB;
  graph *g;
  enum_graph *eg;
  atom_t a;

  switch( PL_foreign_control(h) )
  { case PL_FIRST_CALL:
      if ( PL_is_variable(name) )
      { eg = rdf_malloc(db, sizeof(*eg));
	eg->i  = 0;
	eg->g  = NULL;
	goto next;
      } else if ( PL_get_atom_ex(name, &a) )
      { graph *g;

	if ( (g=existing_graph(db, a)) &&
	     g->triple_count > 0 )
	  return TRUE;
      }
      return FALSE;
    case PL_REDO:
      eg = PL_foreign_context_address(h);
      goto next;
    case PL_PRUNED:
      eg = PL_foreign_context_address(h);
      rdf_free(db, eg, sizeof(*eg));
      return TRUE;
    default:
      assert(0);
      return FALSE;
  }

next:
  if ( !(g=eg->g) )
  { while (!(g = db->graphs.blocks[MSB(eg->i)][eg->i]) )
    { if ( ++eg->i >= db->graphs.bucket_count )
	goto fail;
    }
  }

  if ( !PL_unify_atom(name, g->name) )
  { fail:
    rdf_free(db, eg, sizeof(*eg));
    return FALSE;
  }

  if ( !(eg->g = g->next) )
  { if ( ++eg->i >= db->graphs.bucket_count )
      goto fail;
  }
  PL_retry_address(eg);
}


static foreign_t
rdf_graph_source(term_t graph_name, term_t source, term_t modified)
{ atom_t gn;
  rdf_db *db = DB;

  if ( !get_atom_or_var_ex(graph_name, &gn) )
    return FALSE;

  if ( gn )
  { graph *s;

    if ( (s = existing_graph(db, gn)) && s->source)
    { return ( PL_unify_atom(source, s->source) &&
	       PL_unify_float(modified, s->modified) );
    }
  } else
  { atom_t src;

    if ( PL_get_atom_ex(source, &src) )
    { int i;

      for(i=0; i<db->graphs.bucket_count; i++)
      { graph *g = db->graphs.blocks[MSB(i)][i];

	for(; g; g=g->next)
	{ if ( g->source == src )
	  { return ( PL_unify_atom(graph_name, g->name) &&
		     PL_unify_float(modified, g->modified) );
	  }
	}
      }
    }
  }

  return FALSE;
}


static foreign_t
rdf_set_graph_source(term_t graph_name, term_t source, term_t modified)
{ atom_t gn, src;
  int rc = FALSE;
  rdf_db *db = DB;
  graph *s;
  double mtime;

  if ( !PL_get_atom_ex(graph_name, &gn) ||
       !PL_get_atom_ex(source, &src) ||
       !PL_get_float_ex(modified, &mtime) )
    return FALSE;

  if ( (s = lookup_graph(db, gn)) )
  { LOCK_MISC(db);
    if ( s->source != src )
    { if ( s->source )
	PL_unregister_atom(s->source);
      s->source = src;
      PL_register_atom(s->source);
    }
    s->modified = mtime;
    UNLOCK_MISC(db);
    rc = TRUE;
  }

  return rc;
}


static foreign_t
rdf_unset_graph_source(term_t graph_name)
{ atom_t gn;
  rdf_db *db = DB;
  graph *s;

  if ( !PL_get_atom_ex(graph_name, &gn) )
    return FALSE;
  if ( (s = lookup_graph(db, gn)) )
  { LOCK_MISC(db);
    if ( s->source )
    { PL_unregister_atom(s->source);
      s->source = 0;
    }
    s->modified = 0.0;
    UNLOCK_MISC(db);
  }

  return TRUE;
}



		 /*******************************
		 *	     LITERALS		*
		 *******************************/

static inline void
prepare_literal_ex(literal_ex *lex)
{ SECURE(lex->magic = LITERAL_EX_MAGIC);

  if ( lex->literal->objtype == OBJ_STRING )
  { lex->atom.handle = lex->literal->value.string;
    lex->atom.resolved = FALSE;
  }
}


static literal *
new_literal(rdf_db *db)
{ literal *lit = rdf_malloc(db, sizeof(*lit));
  memset(lit, 0, sizeof(*lit));
  lit->references = 1;

  return lit;
}


static int
free_literal_value(rdf_db *db, literal *lit)
{ int rc = TRUE;

  unlock_atoms_literal(lit);

  if ( lit->shared && !db->resetting )
  { literal_ex lex;

    lit->shared = FALSE;
    rc = broadcast(EV_OLD_LITERAL, lit, NULL);
    DEBUG(2,
	  Sdprintf("Delete %p from literal table: ", lit);
	  print_literal(lit);
	  Sdprintf("\n"));

    lex.literal = lit;
    prepare_literal_ex(&lex);

    if ( !skiplist_delete(&db->literals, &lex) )
    { Sdprintf("Failed to delete %p (size=%ld): ", lit, db->literals.count);
      print_literal(lit);
      Sdprintf("\n");
      assert(0);
    }
  }

  if ( lit->objtype == OBJ_TERM &&
       lit->value.term.record )
  { if ( lit->term_loaded )
      rdf_free(db, lit->value.term.record, lit->value.term.len);
    else
      PL_erase_external(lit->value.term.record);
  }

  return rc;
}


static int
free_literal(rdf_db *db, literal *lit)
{ int rc = TRUE;

  if ( lit->shared )
  { simpleMutexLock(&db->queries.write.lock);
    if ( --lit->references == 0 )
    { rc = free_literal_value(db, lit);
      simpleMutexUnlock(&db->queries.write.lock);

      rdf_free(db, lit, sizeof(*lit));
    } else
    { simpleMutexUnlock(&db->queries.write.lock);
    }
  } else				/* not shared; no locking needed */
  { if ( --lit->references == 0 )
    { rc = free_literal_value(db, lit);

      rdf_free(db, lit, sizeof(*lit));
    }
  }

  return rc;
}


static literal *
copy_literal(rdf_db *db, literal *lit)
{ lit->references++;
  return lit;
}


static void
alloc_literal_triple(rdf_db *db, triple *t)
{ if ( !t->object_is_literal )
  { t->object.literal = new_literal(db);
    t->object_is_literal = TRUE;
  }
}


static void
lock_atoms_literal(literal *lit)
{ if ( !lit->atoms_locked )
  { lit->atoms_locked = TRUE;

    switch(lit->objtype)
    { case OBJ_STRING:
	PL_register_atom(lit->value.string);
	if ( lit->qualifier )
	  PL_register_atom(lit->type_or_lang);
	break;
    }
  }
}


static void
unlock_atoms_literal(literal *lit)
{ if ( lit->atoms_locked )
  { lit->atoms_locked = FALSE;

    switch(lit->objtype)
    { case OBJ_STRING:
	PL_unregister_atom(lit->value.string);
	if ( lit->qualifier )
	  PL_unregister_atom(lit->type_or_lang);
	break;
    }
  }
}


		 /*******************************
		 *	     LITERAL DB		*
		 *******************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
compare_literals() sorts literals.  Ordering is defined as:

	* Numeric literals < string literals < term literals
	* Numeric literals (int and float) are sorted by value
	* String literals are sorted alhabetically
		- case independent, but uppercase before lowercase
		- locale (strcoll) sorting?
		- delete dyadrics
		- first on string, then on type, then on language
	* Terms are sorted on Prolog standard order of terms
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static int
cmp_qualifier(const literal *l1, const literal *l2)
{ if ( l1->qualifier == l2->qualifier )
    return cmp_atoms(l1->type_or_lang, l2->type_or_lang);

  return l1->qualifier - l2->qualifier;
}


static int
compare_literals(literal_ex *lex, literal *l2)
{ literal *l1 = lex->literal;

  SECURE(assert(lex->magic == LITERAL_EX_MAGIC));

  if ( l1->objtype == l2->objtype )
  { int rc;

    switch(l1->objtype)
    { case OBJ_INTEGER:
      { int64_t v1 = l1->value.integer;
	int64_t v2 = l2->value.integer;
	rc = v1 < v2 ? -1 : v1 > v2 ? 1 : 0;
	break;
      }
      case OBJ_DOUBLE:
      { double v1 = l1->value.real;
	double v2 = l2->value.real;
	rc = v1 < v2 ? -1 : v1 > v2 ? 1 : 0;
	break;
      }
      case OBJ_STRING:
      { rc = cmp_atom_info(&lex->atom, l2->value.string);
	break;
      }
      case OBJ_TERM:
      { fid_t fid = PL_open_foreign_frame();
	term_t t1 = PL_new_term_ref();
	term_t t2 = PL_new_term_ref();
					/* can also be handled in literal_ex */
	PL_recorded_external(l1->value.term.record, t1);
	PL_recorded_external(l2->value.term.record, t2);
	rc = PL_compare(t1, t2);

	PL_discard_foreign_frame(fid);
	break;
      }
      default:
	assert(0);
        return 0;
    }

    if ( rc != 0 )
      return rc;
    return cmp_qualifier(l1, l2);
  } else if ( l1->objtype == OBJ_INTEGER && l2->objtype == OBJ_DOUBLE )
  { double v1 = (double)l1->value.integer;
    double v2 = l2->value.real;
    return v1 < v2 ? -1 : v1 > v2 ? 1 : -1;
  } else if ( l1->objtype == OBJ_DOUBLE && l2->objtype == OBJ_INTEGER )
  { double v1 = l1->value.real;
    double v2 = (double)l2->value.integer;
    return v1 < v2 ? -1 : v1 > v2 ? 1 : 1;
  } else
  { return l1->objtype - l2->objtype;
  }
}


static int
sl_compare_literals(void *p1, void *p2, void *cd)
{ literal_ex *lex = p1;
  literal *l2 = *(literal**)p2;
  (void)cd;

  return compare_literals(lex, l2);
}


static void *
sl_rdf_malloc(size_t bytes, void *cd)
{ return rdf_malloc(cd, bytes);
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Create the sorted literal tree. Note  that   we  do  not register a free
handler  for  the  tree  as  nodes   are  either  already  destroyed  by
free_literal() or by rdf_reset_db().
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static int
init_literal_table(rdf_db *db)
{ skiplist_init(&db->literals,
		sizeof(literal*),	/* Payload size */
		db,			/* Client data */
		sl_compare_literals,	/* Compare */
		sl_rdf_malloc,		/* Allocate */
		NULL);			/* Destroy */

  return TRUE;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
share_literal() takes a literal  and  replaces   it  with  one  from the
literal database if there is a match.   On a match, the argument literal
is destroyed. Without a match it adds   the  literal to the database and
returns it.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static literal *
share_literal(rdf_db *db, literal *from)
{ literal **data;
  literal_ex lex;
  int is_new;

  lex.literal = from;
  prepare_literal_ex(&lex);

  data = skiplist_insert(&db->literals, &lex, &is_new);

  if ( !is_new )
  { literal *l2 = *data;

    DEBUG(2,
	  Sdprintf("Replace %p by %p:\n", from, l2);
	  Sdprintf("\tfrom: "); print_literal(from);
	  Sdprintf("\n\tto: "); print_literal(l2);
	  Sdprintf("\n"));

    l2->references++;
    free_literal(db, from);

    return l2;
  } else
  { DEBUG(2,
	  Sdprintf("Insert %p into literal table: ", from);
	  print_literal(from);
	  Sdprintf("\n"));

    assert(from->references==1);
    from->shared = TRUE;
    broadcast(EV_NEW_LITERAL, from, NULL);
    return from;
  }
}


		 /*******************************
		 *	      TRIPLES		*
		 *******************************/

static int
init_triple_hash(rdf_db *db, int index, size_t count)
{ triple_hash *h = &db->hash[index];
  size_t bytes = sizeof(triple_bucket)*count;
  triple_bucket *t = PL_malloc_uncollectable(bytes);
  int i;

  memset(t, 0, bytes);
  memset(h, 0, sizeof(*h));
  for(i=0; i<MSB(count); i++)
    h->blocks[i] = t;

  h->bucket_count_epoch = h->bucket_count = count;

  return TRUE;
}


static int
resize_triple_hash(rdf_db *db, int index)
{ triple_hash *hash = &db->hash[index];
  int i = MSB(hash->bucket_count);
  size_t bytes  = sizeof(triple_bucket)*hash->bucket_count;
  triple_bucket *t = PL_malloc_uncollectable(bytes);

  memset(t, 0, bytes);
  hash->blocks[i] = t-hash->bucket_count;
  hash->bucket_count *= 2;
  DEBUG(1, Sdprintf("Resized triple index %s to %ld\n",
		    col_name[index], (long)hash->bucket_count));

  return TRUE;
}


static void
reset_triple_hash(rdf_db *db, triple_hash *hash)
{ size_t bytes = sizeof(triple_bucket)*hash->bucket_count_epoch;
  int i;

  memset(hash->blocks[0], 0, bytes);
  for(i=MSB(hash->bucket_count_epoch); i<MAX_TBLOCKS; i++)
  { if ( hash->blocks[i] )
    { PL_free(hash->blocks[i]);
      hash->blocks[i] = NULL;
    }
  }
  hash->bucket_count = hash->bucket_count_epoch;
}


static int
count_different(triple_bucket *tb, int index)
{ triple *t;
  atomset hash_set;
  int rc;

  init_atomset(&hash_set);
  for(t=tb->head; t; t=t->tp.next[ICOL(index)])
    add_atomset(&hash_set, (atom_t)triple_hash_key(t, index));
  rc = hash_set.count;
  destroy_atomset(&hash_set);

  return rc;
}


static float
triple_hash_quality(rdf_db *db, int index)
{ triple_hash *hash = &db->hash[index];
  int i;
  float q = 0;
  size_t total = 0;

  if ( index == 0 )
    return 1.0;

  for(i=0; i<hash->bucket_count; i++)
  { int entry = MSB(i);
    triple_bucket *tb = &hash->blocks[entry][i];
    int different = count_different(tb, col_index[index]);

    if ( tb->count )
    { q += (float)tb->count/(float)different;
      total += tb->count;
    }
  }

  return total == 0 ? 1.0 : q/(float)total;
}


/* Consider resizing the hash-tables.  This seems to work quite ok, but there
   are some issues:

    * When should this be called? Just doubling the number of triples is
    too simple. The ones based on triple_hash_quality() could easily get
    poor. Note that once poor, the dynamic expansion trick makes it
    impossible to improve without stopping all threads.

    * triple_hash_quality() is quite costly. This could use sampling and
    it migth be a good idea to sample only the latest expansion.
*/

static void
consider_triple_rehash(rdf_db *db)
{ if ( db->created - db->freed > db->hash[ICOL(BY_SPO)].bucket_count )
  { int i;

    for(i=1; i<INDEX_TABLES; i++)
    { int resize = FALSE;

      switch(col_index[i])
      { case BY_S:
	  if ( db->resources.hash.count > db->hash[i].bucket_count )
	    resize = TRUE;
	  break;
	case BY_P:
	  if ( db->predicates.count > db->hash[i].bucket_count )
	    resize = TRUE;
	  break;
	case BY_O:
	  if ( (db->resources.hash.count + db->literals.count) >
	       db->hash[i].bucket_count )
	    resize = TRUE;
	  break;
	case BY_SPO:
	  if ( db->created - db->freed > db->hash[i].bucket_count )
	    resize = TRUE;
	  break;
	case BY_G:
	  if ( db->graphs.count > db->graphs.bucket_count )
	    resize = TRUE;
	  break;
	case BY_PO:
	case BY_SG:
	case BY_SP:
	case BY_PG:
	  if ( triple_hash_quality(db, i) < 0.5 )
	    resize = TRUE;
	  break;
	default:
	  assert(0);
      }

      if ( resize )
	resize_triple_hash(db, i);
    }
  }
}


static int
init_tables(rdf_db *db)
{ int ic;

  db->hash[ICOL(BY_NONE)].blocks[0] = &db->by_none;
  db->hash[ICOL(BY_NONE)].bucket_count_epoch = 1;
  db->hash[ICOL(BY_NONE)].bucket_count = 1;

  for(ic=BY_S; ic<INDEX_TABLES; ic++)
  { if ( !init_triple_hash(db, ic, INITIAL_TABLE_SIZE) )
      return FALSE;
  }

  return (init_resource_db(db, &db->resources) &&
	  init_pred_table(db) &&
	  init_graph_table(db) &&
	  init_literal_table(db));
}


		 /*******************************
		 *               C		*
		 *******************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Garbage collect triples, given that the   oldest  running query reads at
generation gen.	 There are two thing we can do:

  - Remove any triple that died before gen.  These triples must be left
    to GC.  We can do this
  - Reindex triples that are indexed before the latest hash-table
    resize.  There are two ways:
    - Decide based on the generation where the triple was added.
    - See whether the triple is not optimally hashed.
      - We could do that in match_triples()?
        - Have a next_matching_triple()?
	  - Problem: false info when using a 2nd best hash
      - We could walk a hash-chain, and find the ones that are
        incorrectly hashed.
  - Implement as `doing a little', so we can either mix it into the
    normal workflow or run a thread.

There should be only one GC thread.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void
gc_hash_chain(rdf_db *db, size_t bucket_no, int icol, gen_t gen)
{ triple_bucket *bucket = &db->hash[icol].blocks[MSB(bucket_no)][bucket_no];
  triple *prev = NULL;
  triple *t = bucket->head;

  for(; t; t=t->tp.next[icol])
  { if ( t->lifespan.died < gen )
    { if ( prev )
	prev->tp.next[icol] = t->tp.next[icol];
      else
	bucket->head = t->tp.next[icol];
      if ( t == bucket->tail )
	bucket->tail = prev;

      if ( --t->linked == 0 )
      { DEBUG(0, Sdprintf("GC at gen=%ld..%ld: ",
			  (long)t->lifespan.born,
			  (long)t->lifespan.died);
	         print_triple(t, 0);
	         Sdprintf("\n"));

	free_triple(db, t, TRUE);
      }
    } else
    { prev=t;
    }
  }
}


static void
gc_hash(rdf_db *db, int icol, gen_t gen)
{ size_t mb = db->hash[icol].bucket_count;
  size_t b;

  for(b=0; b<mb; b++)
    gc_hash_chain(db, b, icol, gen);
}


static void
gc_hashes(rdf_db *db, gen_t gen)
{ int icol;

  for(icol=0; icol<INDEX_TABLES; icol++)
    gc_hash(db, icol, gen);
}



static int
gc_db(rdf_db *db, gen_t gen)
{ simpleMutexLock(&db->locks.gc);
  if ( db->gc.busy )
  { simpleMutexUnlock(&db->locks.gc);
    return FALSE;			/* in progress */
  }

  DEBUG(0, Sdprintf("RDF GC; gen = %ld\n", (long)gen));

  db->gc.busy = TRUE;
  gc_hashes(db, gen);
  db->gc.count++;
  db->gc.busy = FALSE;
  simpleMutexUnlock(&db->locks.gc);

  return TRUE;
}


static foreign_t
rdf_gc(void)
{ rdf_db *db = DB;
  gen_t gen = oldest_query_geneneration(db);

  gc_db(db, gen);

  return TRUE;
}


		 /*******************************
		 *	  OVERALL DATABASE	*
		 *******************************/

static rdf_db *
new_db(void)
{ rdf_db *db = PL_malloc_uncollectable(sizeof(*db));

  memset(db, 0, sizeof(*db));
  INIT_LOCK(db);
  init_tables(db);

  return db;
}


static triple *
new_triple(rdf_db *db)
{ triple *t = rdf_malloc(db, sizeof(*t));
  memset(t, 0, sizeof(*t));
  t->allocated = TRUE;

  return t;
}


/* reindex_triple() is used to add a duplicate of the triple to the table
   using the new indexing regime. The old one will be left to GC, but it
   can only be really deleted if no queries exist with the old indexing.
   This will be based on the generation.
*/

static void
reindex_triple(rdf_db *db, triple *t)
{ triple *t2 = rdf_malloc(db, sizeof(*t));

  *t2 = *t;
  memset(&t2->tp, 0, sizeof(t2->tp));
  link_triple_hash(db, t2);
  t->lifespan.died = 0;			/* Died long ago */
}


static void
free_triple(rdf_db *db, triple *t, int linger)
{ unlock_atoms(db, t);

  if ( t->object_is_literal && t->object.literal )
    free_literal(db, t->object.literal);
  if ( t->match == STR_MATCH_BETWEEN )
    free_literal_value(db, &t->tp.end);

  if ( t->allocated )
  { if ( linger )
      PL_linger(t);
    else
      rdf_free(db, t, sizeof(*t));
  }
}


static size_t
literal_hash(literal *lit)
{ if ( lit->hash )
  { return lit->hash;
  } else
  { unsigned int hash;

    switch(lit->objtype)
    { case OBJ_STRING:
	hash = atom_hash_case(lit->value.string);
        break;
      case OBJ_INTEGER:
      case OBJ_DOUBLE:
	hash = rdf_murmer_hash(&lit->value.integer,
			       sizeof(lit->value.integer),
			       MURMUR_SEED);
        break;
      case OBJ_TERM:
	hash = rdf_murmer_hash(lit->value.term.record,
			       (int)lit->value.term.len,
			       MURMUR_SEED);
	break;
      default:
	assert(0);
	return 0;
    }

    if ( !hash )
      hash = 0x1;			/* cannot be 0 */

    lit->hash = hash;
    return lit->hash;
  }
}


static size_t
object_hash(triple *t)
{ if ( t->object_is_literal )
  { return literal_hash(t->object.literal);
  } else
  { return atom_hash(t->object.resource);
  }
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
triple_hash_key() computes the hash for a triple   on  a given index. It
can only be called for indices defined in the col_index-array. Note that
the returned value is unconstrained and  needs   to  be taken modulo the
table-size.

If   you   change   anything   here,   you    might   need   to   update
init_cursor_from_literal().
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static size_t
triple_hash_key(triple *t, int which)
{ size_t v;

  assert(t->resolve_pred == FALSE);

  switch(which)
  { case BY_NONE:
      return 0;
    case BY_S:
      v = atom_hash(t->subject);
      break;
    case BY_P:
      v = predicate_hash(t->predicate.r);
      break;
    case BY_O:
      v = object_hash(t);
      break;
    case BY_SP:
      v = atom_hash(t->subject) ^ predicate_hash(t->predicate.r);
      break;
    case BY_PO:
      v = predicate_hash(t->predicate.r) ^ object_hash(t);
      break;
    case BY_SPO:
      v = (atom_hash(t->subject)<<1) ^
	  predicate_hash(t->predicate.r) ^
	  object_hash(t);
      break;
    case BY_G:
      v = atom_hash(t->graph);
      break;
    case BY_SG:
      v = atom_hash(t->subject) ^ atom_hash(t->graph);
      break;
    case BY_PG:
      v = predicate_hash(t->predicate.r) ^ atom_hash(t->graph);
      break;
    default:
      v = 0;				/* make compiler silent */
      assert(0);
  }

  return v;
}


/* init_triple_walker() and next_triple() are the primitives to walk indexed
   triples.  The pattern is:

	triple_walker tw;

	init_triple_walker(&tw, db, pattern, index);
	while((t=next_triple(tw)))
	  <do your job>

  TBD: Get the generation into this story.  Most likely it is better to
  deal with this in this low-level loop then outside. We will handle
  this in the next cycle.
*/

static void
init_triple_walker(triple_walker *tw, rdf_db *db, triple *pattern, int which)
{ tw->unbounded_hash = triple_hash_key(pattern, which);
  tw->icol	     = ICOL(which);
  tw->hash	     = &db->hash[tw->icol];
  tw->bcount	     = tw->hash->bucket_count_epoch;
  tw->current	     = NULL;
}


static triple *
next_hash_triple(triple_walker *tw)
{ triple *rc;

  if ( tw->bcount <= tw->hash->bucket_count )
  { do
    { int entry = tw->unbounded_hash % tw->bcount;
      triple_bucket *bucket = &tw->hash->blocks[MSB(entry)][entry];

      rc = bucket->head;
      do
      { tw->bcount *= 2;
      } while ( tw->bcount <= tw->hash->bucket_count &&
		tw->unbounded_hash % tw->bcount == entry );
    } while(!rc && tw->bcount <= tw->hash->bucket_count );

    if ( rc )
      tw->current = rc->tp.next[tw->icol];
  } else
  { rc = NULL;
  }

  return rc;
}


static inline triple *
next_triple(triple_walker *tw)
{ triple *rc;

  if ( (rc=tw->current) )
  { tw->current = rc->tp.next[tw->icol];
    return rc;
  } else
  { return next_hash_triple(tw);
  }
}


static inline void
destroy_triple_walker(rdf_db *db, triple_walker *tw)
{
}


static void
set_next_triple(triple_walker *tw, triple *t)
{ tw->current = t;
}



/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
by_inverse[] returns the index key to use   for inverse search as needed
to realise symmetric and inverse predicates.

Note that this only deals with the   non-G(graph)  indices because it is
only used by rdf_has/3 and rdf_reachable/3.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static int by_inverse[8] =
{ BY_NONE,				/* BY_NONE = 0 */
  BY_O,					/* BY_S    = 1 */
  BY_P,					/* BY_P    = 2 */
  BY_PO,				/* BY_SP   = 3 */
  BY_S,					/* BY_O    = 4 */
  BY_SO,				/* BY_SO   = 5 */
  BY_SP,				/* BY_PO   = 6 */
  BY_SP,				/* BY_SPO  = 7 */
};


static void
link_triple_hash(rdf_db *db, triple *t)
{ int ic;

  if ( db->by_none.tail )		/* non-indexed chain */
    db->by_none.tail->tp.next[ICOL(BY_NONE)] = t;
  else
    db->by_none.head = t;
  db->by_none.tail = t;

  for(ic=1; ic<INDEX_TABLES; ic++)
  { triple_hash *hash = &db->hash[ic];
    int i = col_index[ic];
    int key = triple_hash_key(t, i) % hash->bucket_count;
    triple_bucket *bucket = &hash->blocks[MSB(key)][key];

    if ( bucket->tail )
    { bucket->tail->tp.next[ic] = t;
    } else
    { bucket->head = t;
    }
    bucket->tail = t;
    bucket->count++;
  }

  t->linked = INDEX_TABLES;
}


typedef enum
{ DUP_NONE,
  DUP_DUPLICATE,
  DUP_DISCARDED
} dub_state;


static dub_state
discard_duplicate(rdf_db *db, triple *t)
{ triple_walker tw;
  triple *d;
  const int indexed = BY_SPO;
  dub_state rc = DUP_NONE;

  assert(t->is_duplicate == FALSE);
  assert(t->duplicates == 0);

  init_triple_walker(&tw, db, t, indexed);
  while((d=next_triple(&tw)) && d != t)
  { if ( match_triples(d, t, MATCH_DUPLICATE) )
    { if ( d->graph == t->graph &&
	   (d->line == NO_LINE || d->line == t->line) )
      { free_triple(db, t, FALSE);

	return DUP_DISCARDED;
      }

      rc = DUP_DUPLICATE;
    }
  }
  destroy_triple_walker(db, &tw);

  return rc;
}


/* MT: Caller must be hold db->queries.write.lock

   Return: FALSE if nothing changed; TRUE if the database has changed
   TBD: Not all of this requires locking.  Most should be moved out of
   the lock:

	- Check for duplicates (?)
	- Consider re-hash
	- subProperty admin
*/

int
link_triple(rdf_db *db, triple *t)
{ dub_state dup;

  if ( t->linked )
    return FALSE;

  if ( (dup=discard_duplicate(db, t)) == DUP_DISCARDED )
    return FALSE;

  if ( t->object_is_literal )
    t->object.literal = share_literal(db, t->object.literal);

  link_triple_hash(db, t);
  consider_triple_rehash(db);

  if ( dup == DUP_DUPLICATE && update_duplicates_add(db, t) )
    goto ok;				/* is a duplicate */

					/* keep track of subPropertyOf */
  if ( t->predicate.r->name == ATOM_subPropertyOf &&
       t->object_is_literal == FALSE )
  { predicate *me    = lookup_predicate(db, t->subject);
    predicate *super = lookup_predicate(db, t->object.resource);

    addSubPropertyOf(db, me, super);
  }

ok:
  db->created++;
  register_predicate(db, t);
  register_graph(db, t);

  return TRUE;
}


/* MT: Caller must be hold db->queries.write.lock
*/

void
erase_triple(rdf_db *db, triple *t)
{ update_duplicates_del(db, t);

  if ( t->predicate.r->name == ATOM_subPropertyOf &&
       t->object_is_literal == FALSE )
  { predicate *me    = lookup_predicate(db, t->subject);
    predicate *super = lookup_predicate(db, t->object.resource);

    delSubPropertyOf(db, me, super);
  }

  unregister_graph(db, t);
  unregister_predicate(db, t);
  db->erased++;
}


static int
match_literals(int how, literal *p, literal *e, literal *v)
{ literal_ex lex;

  lex.literal = p;
  prepare_literal_ex(&lex);

  DEBUG(2, { Sdprintf("match_literals(");
	     print_literal(p);
	     Sdprintf(", ");
	     print_literal(v);
	     Sdprintf(")\n"); });

  switch(how)
  { case STR_MATCH_LE:
      return compare_literals(&lex, v) >= 0;
    case STR_MATCH_GE:
      return compare_literals(&lex, v) <= 0;
    case STR_MATCH_BETWEEN:
      if ( compare_literals(&lex, v) <= 0 )
      { lex.literal = e;
	prepare_literal_ex(&lex);

	if ( compare_literals(&lex, v) >= 0 )
	  return TRUE;
      }
      return FALSE;
    default:
      return match_atoms(how, p->value.string, v->value.string);
  }
}


static int
match_object(triple *t, triple *p, unsigned flags)
{ if ( p->object_is_literal )
  { if ( t->object_is_literal )
    { literal *plit = p->object.literal;
      literal *tlit = t->object.literal;

      if ( !plit->objtype && !plit->qualifier )
	return TRUE;

      if ( plit->objtype && plit->objtype != tlit->objtype )
	return FALSE;

      switch( plit->objtype )
      { case 0:
	  if ( plit->qualifier &&
	       tlit->qualifier != plit->qualifier )
	    return FALSE;
	  return TRUE;
	case OBJ_STRING:
	  if ( (flags & MATCH_QUAL) ||
	       p->match == STR_MATCH_PLAIN )
	  { if ( tlit->qualifier != plit->qualifier )
	      return FALSE;
	  } else
	  { if ( plit->qualifier && tlit->qualifier &&
		 tlit->qualifier != plit->qualifier )
	      return FALSE;
	  }
	  if ( plit->type_or_lang &&
	       tlit->type_or_lang != plit->type_or_lang )
	    return FALSE;
	  if ( plit->value.string )
	  { if ( tlit->value.string != plit->value.string )
	    { if ( p->match >= STR_MATCH_EXACT )
	      { return match_literals(p->match, plit, &p->tp.end, tlit);
	      } else
	      { return FALSE;
	      }
	    }
	  }
	  return TRUE;
	case OBJ_INTEGER:
	  if ( p->match >= STR_MATCH_LE )
	    return match_literals(p->match, plit, &p->tp.end, tlit);
	  return tlit->value.integer == plit->value.integer;
	case OBJ_DOUBLE:
	  if ( p->match >= STR_MATCH_LE )
	    return match_literals(p->match, plit, &p->tp.end, tlit);
	  return tlit->value.real == plit->value.real;
	case OBJ_TERM:
	  if ( p->match >= STR_MATCH_LE )
	    return match_literals(p->match, plit, &p->tp.end, tlit);
	  if ( plit->value.term.record &&
	       plit->value.term.len != tlit->value.term.len )
	    return FALSE;
	  return memcmp(tlit->value.term.record, plit->value.term.record,
			plit->value.term.len) == 0;
	default:
	  assert(0);
      }
    }
    return FALSE;
  } else
  { if ( p->object.resource )
    { if ( t->object_is_literal ||
	   (p->object.resource != t->object.resource) )
	return FALSE;
    }
  }

  return TRUE;
}



/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Match triple t to pattern p.  Erased triples are always skipped.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static int
match_triples(triple *t, triple *p, unsigned flags)
{ /* DEBUG(3, Sdprintf("match_triple(");
	   print_triple(t, 0);
	   Sdprintf(")\n"));
  */

  if ( p->subject && t->subject != p->subject )
    return FALSE;
  if ( !match_object(t, p, flags) )
    return FALSE;
  if ( flags & MATCH_SRC )
  { if ( p->graph && t->graph != p->graph )
      return FALSE;
    if ( p->line && t->line != p->line )
      return FALSE;
  }
					/* last; may be expensive */
  if ( p->predicate.r && t->predicate.r != p->predicate.r )
  { if ( (flags & MATCH_SUBPROPERTY) )
      return isSubPropertyOf(t->predicate.r, p->predicate.r);
    else
      return FALSE;
  }
  return TRUE;
}


		 /*******************************
		 *	      SAVE/LOAD		*
		 *******************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
The RDF triple format.  This format is intended for quick save and load
and not for readability or exchange.  Parts are based on the SWI-Prolog
Quick Load Format (implemented in pl-wic.c).

	<file>		::= <magic>
			    <version>
			    ['S' <graph-name>]
			    ['F' <graph-source>]
		            ['t' <modified>]
			    ['M' <md5>]
			    {<triple>}
			    'E'

	<magic>		::= "RDF-dump\n"
	<version>	::= <integer>

	<md5>		::= <byte>*		(16 bytes digest)

	<triple>	::= 'T'
	                    <subject>
			    <predicate>
			    <object>
			    <graph>

	<subject>	::= <resource>
	<predicate>	::= <resource>

	<object>	::= "R" <resource>
			  | "L" <atom>

	<resource>	::= <atom>

	<atom>		::= "X" <integer>
			    "A" <string>
			    "W" <utf-8 string>

	<string>	::= <integer><bytes>

	<graph-name>	::= <atom>
	<graph-source>	::= <atom>

	<graph>	::= <graph-file>
			    <line>
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#define SAVE_MAGIC "RDF-dump\n"
#define SAVE_VERSION 2

typedef struct saved
{ atom_t name;
  size_t as;
  struct saved *next;
} saved;


typedef struct save_context
{ saved ** saved_table;
  size_t   saved_size;
  size_t   saved_id;
} save_context;


size_t
next_table_size(size_t s0)
{ size_t size = 2;

  while(size < s0)
    size *= 2;

  return size;
}

static void
init_saved(rdf_db *db, save_context *ctx)
{ size_t size = next_table_size((db->created - db->erased)/8);
  size_t bytes = size * sizeof(*ctx->saved_table);

  ctx->saved_table = rdf_malloc(db, bytes);
  memset(ctx->saved_table, 0, bytes);
  ctx->saved_size = size;
  ctx->saved_id = 0;
}

static void
destroy_saved(rdf_db *db, save_context *ctx)
{ if ( ctx->saved_table )
  { saved **s = ctx->saved_table;
    int i;

    for(i=0; i<ctx->saved_size; i++, s++)
    { saved *c, *n;

      for(c=*s; c; c = n)
      { n = c->next;
	rdf_free(db, c, sizeof(saved));
      }
    }

    rdf_free(db, ctx->saved_table, ctx->saved_size*sizeof(*ctx->saved_table));
  }
}

#define INT64BITSIZE (sizeof(int64_t)*8)
#define PLMINLONG   ((int64_t)((uint64_t)1<<(INT64BITSIZE-1)))

static void
save_int(IOSTREAM *fd, int64_t n)
{ int m;
  int64_t absn = (n >= 0 ? n : -n);

  if ( n != PLMINLONG )
  { if ( absn < ((intptr_t)1 << 5) )
    { Sputc((int)(n & 0x3f), fd);
      return;
    } else if ( absn < ((intptr_t)1 << 13) )
    { Sputc((int)(((n >> 8) & 0x3f) | (1 << 6)), fd);
      Sputc((int)(n & 0xff), fd);
      return;
    } else if ( absn < ((intptr_t)1 << 21) )
    { Sputc((int)(((n >> 16) & 0x3f) | (2 << 6)), fd);
      Sputc((int)((n >> 8) & 0xff), fd);
      Sputc((int)(n & 0xff), fd);
      return;
    }
  }

  for(m = sizeof(n); ; m--)
  { int b = (int)((absn >> (((m-1)*8)-1)) & 0x1ff);

    if ( b == 0 )
      continue;
    break;
  }

  Sputc(m | (3 << 6), fd);

  for( ; m > 0; m--)
  { int b = (int)((n >> ((m-1)*8)) & 0xff);

    Sputc(b, fd);
  }
}


#define BYTES_PER_DOUBLE sizeof(double)
#ifdef WORDS_BIGENDIAN
static const int double_byte_order[] = { 7,6,5,4,3,2,1,0 };
#else
static const int double_byte_order[] = { 0,1,2,3,4,5,6,7 };
#endif

static int
save_double(IOSTREAM *fd, double f)
{ unsigned char *cl = (unsigned char *)&f;
  unsigned int i;

  for(i=0; i<BYTES_PER_DOUBLE; i++)
    Sputc(cl[double_byte_order[i]], fd);

  return TRUE;
}


static int
save_atom(rdf_db *db, IOSTREAM *out, atom_t a, save_context *ctx)
{ int hash = atom_hash(a) % ctx->saved_size;
  saved *s;
  size_t len;
  const char *chars;
  unsigned int i;
  const wchar_t *wchars;

  for(s=ctx->saved_table[hash]; s; s= s->next)
  { if ( s->name == a )
    { Sputc('X', out);
      save_int(out, s->as);

      return TRUE;
    }
  }

  s = rdf_malloc(db, sizeof(*s));
  s->name = a;
  s->as = ctx->saved_id++;
  s->next = ctx->saved_table[hash];
  ctx->saved_table[hash] = s;

  if ( (chars = PL_atom_nchars(a, &len)) )
  { Sputc('A', out);
    save_int(out, len);
    for(i=0; i<len; i++, chars++)
      Sputc(*chars&0xff, out);
  } else if ( (wchars = PL_atom_wchars(a, &len)) )
  { IOENC enc = out->encoding;

    Sputc('W', out);
    save_int(out, len);
    out->encoding = ENC_UTF8;
    for(i=0; i<len; i++, wchars++)
    { wint_t c = *wchars;

      SECURE(assert(c>=0 && c <= 0x10ffff));
      Sputcode(c, out);
    }
    out->encoding = enc;
  } else
    return FALSE;

  return TRUE;
}


static void
write_triple(rdf_db *db, IOSTREAM *out, triple *t, save_context *ctx)
{ Sputc('T', out);

  save_atom(db, out, t->subject, ctx);
  save_atom(db, out, t->predicate.r->name, ctx);

  if ( t->object_is_literal )
  { literal *lit = t->object.literal;

    if ( lit->qualifier )
    { assert(lit->type_or_lang);
      Sputc(lit->qualifier == Q_LANG ? 'l' : 't', out);
      save_atom(db, out, lit->type_or_lang, ctx);
    }

    switch(lit->objtype)
    { case OBJ_STRING:
	Sputc('L', out);
	save_atom(db, out, lit->value.string, ctx);
	break;
      case OBJ_INTEGER:
	Sputc('I', out);
	save_int(out, lit->value.integer);
	break;
      case OBJ_DOUBLE:
      {	Sputc('F', out);
	save_double(out, lit->value.real);
	break;
      }
      case OBJ_TERM:
      { const char *s = lit->value.term.record;
	size_t len = lit->value.term.len;

	Sputc('T', out);
	save_int(out, len);
	while(len-- > 0)
	  Sputc(*s++, out);

	break;
      }
      default:
	assert(0);
    }
  } else
  { Sputc('R', out);
    save_atom(db, out, t->object.resource, ctx);
  }

  save_atom(db, out, t->graph, ctx);
  save_int(out, t->line);
}


static void
write_source(rdf_db *db, IOSTREAM *out, atom_t src, save_context *ctx)
{ graph *s = existing_graph(db, src);

  if ( s && s->source )
  { Sputc('F', out);
    save_atom(db, out, s->source, ctx);
    Sputc('t', out);
    save_double(out, s->modified);
  }
}


static void
write_md5(rdf_db *db, IOSTREAM *out, atom_t src)
{ graph *s = existing_graph(db, src);

  if ( s )
  { md5_byte_t *p = s->digest;
    int i;

    Sputc('M', out);
    for(i=0; i<16; i++)
      Sputc(*p++, out);
  }
}


static int
save_db(query *q, IOSTREAM *out, atom_t src)
{ rdf_db *db = q->db;
  triple *t, p;
  save_context ctx;
  triple_walker tw;

  memset(&p, 0, sizeof(p));
  init_saved(db, &ctx);

  Sfprintf(out, "%s", SAVE_MAGIC);
  save_int(out, SAVE_VERSION);
  if ( src )
  { Sputc('S', out);			/* start of graph header */
    save_atom(db, out, src, &ctx);
    write_source(db, out, src, &ctx);
    write_md5(db, out, src);
    p.graph = src;
    p.indexed = BY_G;
  } else
  { p.indexed = BY_NONE;
  }
  if ( Sferror(out) )
    return FALSE;

  init_triple_walker(&tw, db, &p, p.indexed);
  while((t=next_triple(&tw)))
  { if ( alive_triple(q, t) &&
	 (!src || t->graph == src) )
    { write_triple(db, out, t, &ctx);
      if ( Sferror(out) )
	return FALSE;
    }
  }
  Sputc('E', out);
  if ( Sferror(out) )
    return FALSE;

  destroy_saved(db, &ctx);

  return TRUE;
}


static foreign_t
rdf_save_db(term_t stream, term_t graph)
{ rdf_db *db = DB;
  query *q;
  IOSTREAM *out;
  atom_t src;
  int rc;

  if ( !PL_get_stream_handle(stream, &out) )
    return PL_type_error("stream", stream);
  if ( !get_atom_or_var_ex(graph, &src) )
    return FALSE;

  q = open_query(db);
  rc = save_db(q, out, src);
  close_query(q);

  return rc;
}


static int64_t
load_int(IOSTREAM *fd)
{ int64_t first = Sgetc(fd);
  int bytes, shift, b;

  if ( !(first & 0xc0) )		/* 99% of them: speed up a bit */
  { first <<= (INT64BITSIZE-6);
    first >>= (INT64BITSIZE-6);

    return first;
  }

  bytes = (int) ((first >> 6) & 0x3);
  first &= 0x3f;

  if ( bytes <= 2 )
  { for( b = 0; b < bytes; b++ )
    { first <<= 8;
      first |= Sgetc(fd) & 0xff;
    }

    shift = (sizeof(first)-1-bytes)*8 + 2;
  } else
  { int m;

    bytes = (int)first;
    first = 0L;

    for(m=0; m<bytes; m++)
    { first <<= 8;
      first |= Sgetc(fd) & 0xff;
    }
    shift = (sizeof(first)-bytes)*8;
  }

  first <<= shift;
  first >>= shift;

  return first;
}


static int
load_double(IOSTREAM *fd, double *fp)
{ double f;
  unsigned char *cl = (unsigned char *)&f;
  unsigned int i;

  for(i=0; i<BYTES_PER_DOUBLE; i++)
  { int c = Sgetc(fd);

    if ( c == -1 )
    { *fp = 0.0;
      return FALSE;
    }
    cl[double_byte_order[i]] = c;
  }

  *fp = f;
  return TRUE;
}


typedef struct ld_context
{ long		loaded_id;		/* keep track of atoms */
  atom_t       *loaded_atoms;
  long		atoms_size;
  atom_t	graph;			/* for single-graph files */
  atom_t	graph_source;
  double	modified;
  int		has_digest;
  md5_byte_t    digest[16];
  atom_hash    *graph_table;		/* multi-graph file */
  query	       *query;
  triple_buffer	triples;
} ld_context;


static void
add_atom(rdf_db *db, atom_t a, ld_context *ctx)
{ if ( ctx->loaded_id >= ctx->atoms_size )
  { if ( ctx->atoms_size == 0 )
    { ctx->atoms_size = 1024;
      ctx->loaded_atoms = malloc(sizeof(atom_t)*ctx->atoms_size);
    } else
    { size_t  bytes;

      ctx->atoms_size *= 2;
      bytes = sizeof(atom_t)*ctx->atoms_size;
      ctx->loaded_atoms = realloc(ctx->loaded_atoms, bytes);
    }
  }

  ctx->loaded_atoms[ctx->loaded_id++] = a;
}


static atom_t
load_atom(rdf_db *db, IOSTREAM *in, ld_context *ctx)
{ switch(Sgetc(in))
  { case 'X':
    { intptr_t idx = (intptr_t)load_int(in);
      return ctx->loaded_atoms[idx];
    }
    case 'A':
    { size_t len = (size_t)load_int(in);
      atom_t a;

      if ( len < 1024 )
      { char buf[1024];
	Sfread(buf, 1, len, in);
	a = PL_new_atom_nchars(len, buf);
      } else
      { char *buf = rdf_malloc(db, len);
	Sfread(buf, 1, len, in);
	a = PL_new_atom_nchars(len, buf);
	rdf_free(db, buf, len);
      }

      add_atom(db, a, ctx);
      return a;
    }
    case 'W':
    { int len = (int)load_int(in);
      atom_t a;
      wchar_t buf[1024];
      wchar_t *w;
      IOENC enc = in->encoding;
      int i;

      if ( len < 1024 )
	w = buf;
      else
	w = rdf_malloc(db, len*sizeof(wchar_t));

      in->encoding = ENC_UTF8;
      for(i=0; i<len; i++)
      { w[i] = Sgetcode(in);
	SECURE(assert(w[i]>=0 && w[i] <= 0x10ffff));
      }
      in->encoding = enc;

      a = PL_new_atom_wchars(len, w);
      if ( w != buf )
	rdf_free(db, w, len*sizeof(wchar_t));

      add_atom(db, a, ctx);
      return a;
    }
    default:
    { assert(0);
      return 0;
    }
  }
}


static triple *
load_triple(rdf_db *db, IOSTREAM *in, ld_context *ctx)
{ triple *t = new_triple(db);
  int c;

  t->subject   = load_atom(db, in, ctx);
  t->predicate.r = lookup_predicate(db, load_atom(db, in, ctx));
  if ( (c=Sgetc(in)) == 'R' )
  { t->object.resource = load_atom(db, in, ctx);
  } else
  { literal *lit = new_literal(db);

    t->object_is_literal = TRUE;
    t->object.literal = lit;

  value:
    switch(c)
    { case 'L':
	lit->objtype = OBJ_STRING;
	lit->value.string = load_atom(db, in, ctx);
	break;
      case 'I':
	lit->objtype = OBJ_INTEGER;
	lit->value.integer = load_int(in);
	break;
      case 'F':
	lit->objtype = OBJ_DOUBLE;
        load_double(in, &lit->value.real);
	break;
      case 'T':
      { unsigned int i;
	char *s;

	lit->objtype = OBJ_TERM;
	lit->value.term.len = (size_t)load_int(in);
	lit->value.term.record = rdf_malloc(db, lit->value.term.len);
	lit->term_loaded = TRUE;	/* see free_literal() */
	s = (char *)lit->value.term.record;

	for(i=0; i<lit->value.term.len; i++)
	  s[i] = Sgetc(in);

	break;
      }
      case 'l':
	lit->qualifier = Q_LANG;
	lit->type_or_lang = load_atom(db, in, ctx);
	c = Sgetc(in);
	goto value;
      case 't':
	lit->qualifier = Q_TYPE;
	lit->type_or_lang = load_atom(db, in, ctx);
	c = Sgetc(in);
	goto value;
      default:
	assert(0);
        return NULL;
    }
  }
  t->graph = load_atom(db, in, ctx);
  t->line  = (unsigned long)load_int(in);
  if ( !ctx->graph )
  { if ( !ctx->graph_table )
      ctx->graph_table = new_atom_hash(64);
    add_atom_hash(ctx->graph_table, t->graph);
  }

  return t;
}


static int
load_magic(IOSTREAM *in)
{ char *s = SAVE_MAGIC;

  for( ; *s; s++)
  { if ( Sgetc(in) != *s )
      return FALSE;
  }

  return TRUE;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Note that we have two types  of   saved  states.  One holding many named
graphs and one holding the content of exactly one named graph.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static int
load_db(rdf_db *db, IOSTREAM *in, ld_context *ctx)
{ int version;
  int c;

  if ( !load_magic(in) )
    return FALSE;
  version = (int)load_int(in);

  while((c=Sgetc(in)) != EOF)
  { switch(c)
    { case 'T':
      { triple *t;

	if ( !(t=load_triple(db, in, ctx)) )
	  return FALSE;
	buffer_triple(&ctx->triples, t);
        break;
      }
					/* file holding exactly one graph */
      case 'S':				/* name of the graph */
      { ctx->graph = load_atom(db, in, ctx);
        break;
      }
      case 'M':				/* MD5 of the graph */
      { int i;

	for(i=0; i<16; i++)
	  ctx->digest[i] = Sgetc(in);
	ctx->has_digest = TRUE;

	break;
      }
      case 'F':				/* file of the graph */
	ctx->graph_source = load_atom(db, in, ctx);
	break;				/* end of one-graph handling */
      case 't':
	load_double(in, &ctx->modified);
        break;
      case 'E':				/* end of file */
	return TRUE;
      default:
	break;
    }
  }

  PL_warning("Illegal RDF triple file");

  return FALSE;
}


static int
link_loaded_triples(rdf_db *db, ld_context *ctx)
{ graph *graph;
  triple **t;

  if ( ctx->graph )			/* lookup named graph */
  { graph = lookup_graph(db, ctx->graph);
    if ( ctx->graph_source && graph->source != ctx->graph_source )
    { if ( graph->source )
	PL_unregister_atom(graph->source);
      graph->source = ctx->graph_source;
      PL_register_atom(graph->source);
      graph->modified = ctx->modified;
    }

    if ( ctx->has_digest )
    { if ( graph->md5 )
      { graph->md5 = FALSE;		/* kill repetitive MD5 update */
      } else
      { ctx->has_digest = FALSE;
      }
    }
  } else
  { graph = NULL;
  }

					/* TBD: broadcast(EV_ASSERT_LOAD, ...) */
  for(t=ctx->triples.base; t<ctx->triples.top; t++)
    lock_atoms(db, *t);
  add_triples(ctx->query, ctx->triples.base, ctx->triples.top-ctx->triples.base);

					/* update the graph info */
  if ( ctx->has_digest )
  { sum_digest(graph->digest, ctx->digest);
    graph->md5 = TRUE;
  }

  db->generation += (ctx->triples.top - ctx->triples.base);

  return TRUE;
}


static int
append_graph_to_list(ptr_hash_node *node, void *closure)
{ atom_t graph = (atom_t)node->value;
  term_t tail  = (term_t)closure;
  term_t head  = PL_new_term_ref();
  int rc;

  rc = (PL_unify_list(tail, head, tail) &&
	PL_unify_atom(head, graph));
  PL_reset_term_refs(head);

  return rc;
}


static foreign_t
rdf_load_db(term_t stream, term_t id, term_t graphs)
{ ld_context ctx;
  rdf_db *db = DB;
  IOSTREAM *in;
  int rc;

  if ( !PL_get_stream_handle(stream, &in) )
    return PL_type_error("stream", stream);

  memset(&ctx, 0, sizeof(ctx));
  init_triple_buffer(&ctx.triples);
  rc = load_db(db, in, &ctx);
  PL_release_stream(in);
  if ( !rc )
  { return FALSE;			/* TBD: Discard partial load */
  }

  ctx.query = open_query(db);
  broadcast(EV_LOAD, (void*)id, (void*)ATOM_begin);

  if ( (rc=link_loaded_triples(db, &ctx)) )
  { if ( ctx.graph_table )
    { term_t tail = PL_copy_term_ref(graphs);

      rc = ( for_atom_hash(ctx.graph_table, append_graph_to_list, (void*)tail) &&
	     PL_unify_nil(tail) );

      destroy_atom_hash(ctx.graph_table);
    } else
    { rc = PL_unify_atom(graphs, ctx.graph);
    }
  }

  broadcast(EV_LOAD, (void*)id, (void*)ATOM_end);
  close_query(ctx.query);

  if ( ctx.loaded_atoms )
  { atom_t *ap, *ep;

    for(ap=ctx.loaded_atoms, ep=ap+ctx.loaded_id; ap<ep; ap++)
      PL_unregister_atom(*ap);

    free(ctx.loaded_atoms);
  }

  return rc;
}


#ifdef WITH_MD5
		 /*******************************
		 *	     MD5 SUPPORT	*
		 *******************************/

/* md5_type is used to keep the MD5 independent from the internal
   numbers
*/
static const char md5_type[] =
{ 0x0,					/* OBJ_UNKNOWN */
  0x3,					/* OBJ_INTEGER */
  0x4,					/* OBJ_DOUBLE */
  0x2,					/* OBJ_STRING */
  0x5					/* OBJ_TERM */
};

static void
md5_triple(triple *t, md5_byte_t *digest)
{ md5_state_t state;
  size_t len;
  md5_byte_t tmp[2];
  const char *s;
  literal *lit;

  md5_init(&state);
  s = PL_blob_data(t->subject, &len, NULL);
  md5_append(&state, (const md5_byte_t *)s, (int)len);
  md5_append(&state, (const md5_byte_t *)"P", 1);
  s = PL_blob_data(t->predicate.r->name, &len, NULL);
  md5_append(&state, (const md5_byte_t *)s, (int)len);
  tmp[0] = 'O';
  if ( t->object_is_literal )
  { lit = t->object.literal;
    tmp[1] = md5_type[lit->objtype];

    switch(lit->objtype)
    { case OBJ_STRING:
	s = PL_blob_data(lit->value.string, &len, NULL);
	break;
      case OBJ_INTEGER:			/* TBD: byte order issues */
	s = (const char *)&lit->value.integer;
	len = sizeof(lit->value.integer);
	break;
      case OBJ_DOUBLE:
	s = (const char *)&lit->value.real;
	len = sizeof(lit->value.real);
	break;
      case OBJ_TERM:
	s = (const char *)lit->value.term.record;
	len = lit->value.term.len;
	break;
      default:
	assert(0);
    }
  } else
  { s = PL_blob_data(t->object.resource, &len, NULL);
    tmp[1] = 0x1;			/* old OBJ_RESOURCE */
    lit = NULL;
  }
  md5_append(&state, tmp, 2);
  md5_append(&state, (const md5_byte_t *)s, (int)len);
  if ( lit && lit->qualifier )
  { assert(lit->type_or_lang);
    md5_append(&state,
	       (const md5_byte_t *)(lit->qualifier == Q_LANG ? "l" : "t"),
	       1);
    s = PL_blob_data(lit->type_or_lang, &len, NULL);
    md5_append(&state, (const md5_byte_t *)s, (int)len);
  }
  if ( t->graph )
  { md5_append(&state, (const md5_byte_t *)"S", 1);
    s = PL_blob_data(t->graph, &len, NULL);
    md5_append(&state, (const md5_byte_t *)s, (int)len);
  }

  md5_finish(&state, digest);
}


static void
sum_digest(md5_byte_t *digest, md5_byte_t *add)
{ md5_byte_t *p, *q;
  int n;

  for(p=digest, q=add, n=16; --n>=0; )
    *p++ += *q++;
}


static void
dec_digest(md5_byte_t *digest, md5_byte_t *add)
{ md5_byte_t *p, *q;
  int n;

  for(p=digest, q=add, n=16; --n>=0; )
    *p++ -= *q++;
}


static int
md5_unify_digest(term_t t, md5_byte_t digest[16])
{ char hex_output[16*2];
  int di;
  char *pi;
  static char hexd[] = "0123456789abcdef";

  for(pi=hex_output, di = 0; di < 16; ++di)
  { *pi++ = hexd[(digest[di] >> 4) & 0x0f];
    *pi++ = hexd[digest[di] & 0x0f];
  }

  return PL_unify_atom_nchars(t, 16*2, hex_output);
}


static foreign_t
rdf_md5(term_t graph_name, term_t md5)
{ atom_t src;
  int rc;
  rdf_db *db = DB;

  if ( !get_atom_or_var_ex(graph_name, &src) )
    return FALSE;

  if ( src )
  { graph *s;

    if ( (s = existing_graph(db, src)) )
    { rc = md5_unify_digest(md5, s->digest);
    } else
    { md5_byte_t digest[16];

      memset(digest, 0, sizeof(digest));
      rc = md5_unify_digest(md5, digest);
    }
  } else
  { md5_byte_t digest[16];
    int i;

    memset(&digest, 0, sizeof(digest));
    for(i=0; i<db->graphs.bucket_count; i++)
    { graph *g = db->graphs.blocks[MSB(i)][i];

      for( ; g; g = g->next )
	sum_digest(digest, g->digest);
    }

    return md5_unify_digest(md5, digest);
  }

  return rc;
}


static foreign_t
rdf_atom_md5(term_t text, term_t times, term_t md5)
{ char *s;
  int n, i;
  size_t len;
  md5_byte_t digest[16];

  if ( !PL_get_nchars(text, &len, &s, CVT_ALL|CVT_EXCEPTION) )
    return FALSE;
  if ( !PL_get_integer_ex(times, &n) )
    return FALSE;
  if ( n < 1 )
    return PL_domain_error("positive_integer", times);

  for(i=0; i<n; i++)
  { md5_state_t state;
    md5_init(&state);
    md5_append(&state, (const md5_byte_t *)s, (int)len);
    md5_finish(&state, digest);
    s = (char *)digest;
    len = sizeof(digest);
  }

  return md5_unify_digest(md5, digest);
}



#endif /*WITH_MD5*/


		 /*******************************
		 *	       ATOMS		*
		 *******************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Lock atoms in triple against AGC. Note that the predicate name is locked
in the predicate structure.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void
lock_atoms(rdf_db *db, triple *t)
{ if ( !t->atoms_locked )
  { t->atoms_locked = TRUE;

    lookup_resource(&db->resources, t->subject);
    if ( t->object_is_literal )
    { lock_atoms_literal(t->object.literal);
    } else
    { lookup_resource(&db->resources, t->object.resource);
    }
  }
}


static void
unlock_atoms(rdf_db *db, triple *t)
{ if ( t->atoms_locked )
  { t->atoms_locked = FALSE;

    //PL_unregister_atom(t->subject);	/* TBD */
    if ( !t->object_is_literal )
    { //PL_unregister_atom(t->object.resource); /*TBD*/
    }
  }
}


		 /*******************************
		 *      PROLOG CONVERSION	*
		 *******************************/

#define LIT_TYPED	0x1
#define LIT_NOERROR	0x2
#define LIT_PARTIAL	0x4

static int
get_lit_atom_ex(term_t t, atom_t *a, int flags)
{ if ( PL_get_atom(t, a) )
    return TRUE;
  if ( (flags & LIT_PARTIAL) && PL_is_variable(t) )
  { *a = 0L;
    return TRUE;
  }

  return PL_type_error("atom", t);
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
get_literal() processes the argument  of  a   literal/1  term  passes as
object.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static int
get_literal(rdf_db *db, term_t litt, literal *lit, int flags)
{ if ( PL_get_atom(litt, &lit->value.string) )
  { lit->objtype = OBJ_STRING;
  } else if ( PL_is_integer(litt) && PL_get_int64(litt, &lit->value.integer) )
  { lit->objtype = OBJ_INTEGER;
  } else if ( PL_get_float(litt, &lit->value.real) )
  { lit->objtype = OBJ_DOUBLE;
  } else if ( PL_is_functor(litt, FUNCTOR_lang2) )
  { term_t a = PL_new_term_ref();

    _PL_get_arg(1, litt, a);
    if ( !get_lit_atom_ex(a, &lit->type_or_lang, flags) )
      return FALSE;
    _PL_get_arg(2, litt, a);
    if ( !get_lit_atom_ex(a, &lit->value.string, flags) )
      return FALSE;

    lit->qualifier = Q_LANG;
    lit->objtype = OBJ_STRING;
  } else if ( PL_is_functor(litt, FUNCTOR_type2) &&
	      !(flags & LIT_TYPED) )	/* avoid recursion */
  { term_t a = PL_new_term_ref();

    _PL_get_arg(1, litt, a);
    if ( !get_lit_atom_ex(a, &lit->type_or_lang, flags) )
      return FALSE;
    lit->qualifier = Q_TYPE;
    _PL_get_arg(2, litt, a);

    return get_literal(db, a, lit, LIT_TYPED|flags);
  } else if ( !PL_is_ground(litt) )
  { if ( !(flags & LIT_PARTIAL) )
      return PL_type_error("rdf_object", litt);
    if ( !PL_is_variable(litt) )
      lit->objtype = OBJ_TERM;
  } else
  { lit->value.term.record = PL_record_external(litt, &lit->value.term.len);
    lit->objtype = OBJ_TERM;
  }

  return TRUE;
}


static int
get_object(rdf_db *db, term_t object, triple *t)
{ if ( PL_get_atom(object, &t->object.resource) )
  { assert(!t->object_is_literal);
  } else if ( PL_is_functor(object, FUNCTOR_literal1) )
  { term_t a = PL_new_term_ref();

    _PL_get_arg(1, object, a);
    alloc_literal_triple(db, t);
    return get_literal(db, a, t->object.literal, 0);
  } else
    return PL_type_error("rdf_object", object);

  return TRUE;
}


static int
get_src(term_t src, triple *t)
{ if ( src && !PL_is_variable(src) )
  { if ( PL_get_atom(src, &t->graph) )
    { t->line = NO_LINE;
    } else if ( PL_is_functor(src, FUNCTOR_colon2) )
    { term_t a = PL_new_term_ref();
      long line;

      _PL_get_arg(1, src, a);
      if ( !get_atom_or_var_ex(a, &t->graph) )
	return FALSE;
      _PL_get_arg(2, src, a);
      if ( PL_get_long(a, &line) )
	t->line = line;
      else if ( !PL_is_variable(a) )
	return PL_type_error("integer", a);
    } else
      return PL_type_error("rdf_graph", src);
  }

  return TRUE;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Return values:
	-1: exception
	 0: no predicate
	 1: the predicate
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static int
get_existing_predicate(rdf_db *db, term_t t, predicate **p)
{ atom_t name;

  if ( !PL_get_atom(t, &name ) )
  { if ( PL_is_functor(t, FUNCTOR_literal1) )
      return 0;				/* rdf(_, literal(_), _) */
    return PL_type_error("atom", t);
  }

  if ( (*p = existing_predicate(db, name)) )
    return 1;

  DEBUG(5, Sdprintf("No predicate %s\n", PL_atom_chars(name)));
  return 0;				/* no predicate */
}


static int
get_predicate(rdf_db *db, term_t t, predicate **p)
{ atom_t name;

  if ( !PL_get_atom_ex(t, &name ) )
    return FALSE;

  *p = lookup_predicate(db, name);
  return TRUE;
}


static int
get_triple(rdf_db *db,
	   term_t subject, term_t predicate, term_t object,
	   triple *t)
{ if ( !PL_get_atom_ex(subject, &t->subject) ||
       !get_predicate(db, predicate, &t->predicate.r) ||
       !get_object(db, object, t) )
    return FALSE;

  return TRUE;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
get_partial_triple() creates a triple  for   matching  purposes.  It can
return FALSE for  two  reasons.  Mostly   (type)  errors,  but  also  if
resources are accessed that do not   exist  and therefore the subsequent
matching will always fail. This  is   notably  the  case for predicates,
which are first class citizens to this library.

Return values:
	1: ok
	0: no predicate
       -1: error
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static int
get_partial_triple(rdf_db *db,
		   term_t subject, term_t predicate, term_t object,
		   term_t src, triple *t)
{ int rc;
  int ipat = 0;

  if ( subject && !get_resource_or_var_ex(subject, &t->subject) )
    return FALSE;
  if ( !PL_is_variable(predicate) &&
       (rc=get_existing_predicate(db, predicate, &t->predicate.r)) != 1 )
    return rc;
					/* the object */
  if ( object && !PL_is_variable(object) )
  { if ( PL_get_atom(object, &t->object.resource) )
    { assert(!t->object_is_literal);
    } else if ( PL_is_functor(object, FUNCTOR_literal1) )
    { term_t a = PL_new_term_ref();

      _PL_get_arg(1, object, a);
      alloc_literal_triple(db, t);
      if ( !get_literal(db, a, t->object.literal, LIT_PARTIAL) )
	return FALSE;
    } else if ( PL_is_functor(object, FUNCTOR_literal2) )
    { term_t a = PL_new_term_ref();
      literal *lit;

      alloc_literal_triple(db, t);
      lit = t->object.literal;

      _PL_get_arg(1, object, a);
      if ( PL_is_functor(a, FUNCTOR_exact1) )
	t->match = STR_MATCH_EXACT;
      else if ( PL_is_functor(a, FUNCTOR_plain1) )
	t->match = STR_MATCH_PLAIN;
      else if ( PL_is_functor(a, FUNCTOR_substring1) )
	t->match = STR_MATCH_SUBSTRING;
      else if ( PL_is_functor(a, FUNCTOR_word1) )
	t->match = STR_MATCH_WORD;
      else if ( PL_is_functor(a, FUNCTOR_prefix1) )
	t->match = STR_MATCH_PREFIX;
      else if ( PL_is_functor(a, FUNCTOR_like1) )
	t->match = STR_MATCH_LIKE;
      else if ( PL_is_functor(a, FUNCTOR_le1) )
	t->match = STR_MATCH_LE;
      else if ( PL_is_functor(a, FUNCTOR_ge1) )
	t->match = STR_MATCH_GE;
      else if ( PL_is_functor(a, FUNCTOR_between2) )
      { term_t e = PL_new_term_ref();

	_PL_get_arg(2, a, e);
	memset(&t->tp.end, 0, sizeof(t->tp.end));
	if ( !get_literal(db, e, &t->tp.end, 0) )
	  return FALSE;
	t->match = STR_MATCH_BETWEEN;
      } else
	return PL_domain_error("match_type", a);

      _PL_get_arg(1, a, a);
      if ( t->match >= STR_MATCH_LE )
      { if ( !get_literal(db, a, lit, 0) )
	  return FALSE;
      } else
      { if ( !get_atom_or_var_ex(a, &lit->value.string) )
	  return FALSE;
	lit->objtype = OBJ_STRING;
      }
    } else
      return PL_type_error("rdf_object", object);
  }
					/* the graph */
  if ( !get_src(src, t) )
    return FALSE;

  if ( t->subject )
    ipat |= BY_S;
  if ( t->predicate.r )
    ipat |= BY_P;
  if ( t->object_is_literal )
  { literal *lit = t->object.literal;

    switch( lit->objtype )
    { case OBJ_UNTYPED:
	break;
      case OBJ_STRING:
	if ( lit->objtype == OBJ_STRING )
	{ if ( lit->value.string &&
	       t->match <= STR_MATCH_EXACT )
	    ipat |= BY_O;
	}
        break;
      case OBJ_INTEGER:
      case OBJ_DOUBLE:
	ipat |= BY_O;
        break;
      case OBJ_TERM:
	if ( PL_is_ground(object) )
	  ipat |= BY_O;
        break;
      default:
	assert(0);
    }
  } else if ( t->object.resource )
  { ipat |= BY_O;
  }
  if ( t->graph )
    ipat |= BY_G;

  db->indexed[ipat]++;			/* statistics */
  t->indexed = alt_index[ipat];

  return TRUE;
}


static int
inverse_partial_triple(triple *t)
{ predicate *i = 0;

  if ( !t->inversed &&
       (!t->predicate.r || (i=t->predicate.r->inverse_of)) &&
       !t->object_is_literal )
  { atom_t o = t->object.resource;

    t->object.resource = t->subject;
    t->subject = o;

    if ( t->predicate.r )
      t->predicate.r = i;

    t->indexed  = by_inverse[t->indexed];
    t->inversed = TRUE;

    return TRUE;
  }

  return FALSE;
}


static int
get_graph(term_t src, triple *t)
{ if ( PL_get_atom(src, &t->graph) )
  { t->line = NO_LINE;
    return TRUE;
  }

  if ( PL_is_functor(src, FUNCTOR_colon2) )
  { term_t a = PL_new_term_ref();
    long line;

    _PL_get_arg(1, src, a);
    if ( !PL_get_atom_ex(a, &t->graph) )
      return FALSE;
    _PL_get_arg(2, src, a);
    if ( !PL_get_long_ex(a, &line) )
      return FALSE;
    t->line = line;

    return TRUE;
  }

  return PL_type_error("rdf_graph", src);
}


static int
unify_graph(term_t src, triple *t)
{ switch( PL_term_type(src) )
  { case PL_VARIABLE:
    { if ( t->line == NO_LINE )
	return PL_unify_atom(src, t->graph);
      else
	goto full_term;
    }
    case PL_ATOM:
    { atom_t a;
      return (PL_get_atom(src, &a) &&
	      a == t->graph);
    }
    case PL_TERM:
    { if ( t->line == NO_LINE )
      { return PL_unify_term(src,
			     PL_FUNCTOR, FUNCTOR_colon2,
			       PL_ATOM, t->graph,
			       PL_VARIABLE);
      } else
      { full_term:
	return PL_unify_term(src,
			     PL_FUNCTOR, FUNCTOR_colon2,
			       PL_ATOM, t->graph,
			       PL_LONG, t->line);
      }
    }
    default:
      return PL_type_error("rdf_graph", src);
  }
}


static int
same_graph(triple *t1, triple *t2)
{ return t1->line  == t2->line &&
         t1->graph == t2->graph;
}



static int
put_literal_value(term_t v, literal *lit)
{ switch(lit->objtype)
  { case OBJ_STRING:
      PL_put_atom(v, lit->value.string);
      break;
    case OBJ_INTEGER:
      PL_put_variable(v);
      return PL_unify_int64(v, lit->value.integer);
    case OBJ_DOUBLE:
      return PL_put_float(v, lit->value.real);
    case OBJ_TERM:
      return PL_recorded_external(lit->value.term.record, v);
    default:
      assert(0);
      return FALSE;
  }

  return TRUE;
}


static int
unify_literal(term_t lit, literal *l)
{ term_t v = PL_new_term_ref();

  if ( !put_literal_value(v, l) )
    return FALSE;

  if ( l->qualifier )
  { functor_t qf;

    assert(l->type_or_lang);

    if ( l->qualifier == Q_LANG )
      qf = FUNCTOR_lang2;
    else
      qf = FUNCTOR_type2;

    if ( PL_unify_term(lit, PL_FUNCTOR, qf,
			 PL_ATOM, l->type_or_lang,
			 PL_TERM, v) )
      return TRUE;

    if ( PL_exception(0) )
      return FALSE;

    return PL_unify(lit, v);		/* allow rdf(X, Y, literal(foo)) */
  } else if ( PL_unify(lit, v) )
  { return TRUE;
  } else if ( PL_is_functor(lit, FUNCTOR_lang2) &&
	      l->objtype == OBJ_STRING )
  { term_t a = PL_new_term_ref();
    _PL_get_arg(2, lit, a);
    return PL_unify(a, v);
  } else if ( PL_is_functor(lit, FUNCTOR_type2) )
  { term_t a = PL_new_term_ref();
    _PL_get_arg(2, lit, a);
    return PL_unify(a, v);
  } else
    return FALSE;
}



static int
unify_object(term_t object, triple *t)
{ if ( t->object_is_literal )
  { term_t lit = PL_new_term_ref();

    if ( PL_unify_functor(object, FUNCTOR_literal1) )
      _PL_get_arg(1, object, lit);
    else if ( PL_is_functor(object, FUNCTOR_literal2) )
      _PL_get_arg(2, object, lit);
    else
      return FALSE;

    return unify_literal(lit, t->object.literal);
  } else
  { return PL_unify_atom(object, t->object.resource);
  }
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
TRUE:  ok
FALSE: failure
ERROR: error
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static int
unify_triple(term_t subject, term_t pred, term_t object,
	     term_t src, triple *t, int inversed)
{ predicate *p = t->predicate.r;
  fid_t fid;

  if ( inversed )
  { term_t tmp = object;
    object = subject;
    subject = tmp;

    if ( !(p = p->inverse_of) )
      return FALSE;
  }

  fid = PL_open_foreign_frame();

  if ( !PL_unify_atom(subject, t->subject) ||
       !PL_unify_atom(pred, p->name) ||
       !unify_object(object, t) ||
       (src && !unify_graph(src, t)) )
  { if ( PL_exception(0) )
    { PL_close_foreign_frame(fid);
      return ERROR;
    }

    PL_discard_foreign_frame(fid);
    return FALSE;
  } else
  { PL_close_foreign_frame(fid);
    return TRUE;
  }
}


		 /*******************************
		 *	DUBLICATE HANDLING	*
		 *******************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
According to the RDF specs, duplicate triples  have no meaning, but they
slow down search and often produce   duplicate results in search. Worse,
some coding styles proposed in the  OWL documents introduce huge amounts
of duplicate triples. We cannot  simply  ignore   a  triple  if  it is a
duplicate as a subsequent retract  would   delete  the final triple. For
example, after loading two  files  that   contain  the  same  triple and
unloading one of these files the database would be left without triples.

In our solution, if a triple is added as a duplicate, it is flagged such
using  the  flag  is_duplicate.  The  `principal'  triple  has  a  count
`duplicates',  indicating  the  number  of   duplicate  triples  in  the
database.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */


static int
update_duplicates_add(rdf_db *db, triple *t)
{ triple_walker tw;
  triple *d;
  const int indexed = BY_SPO;

  assert(t->is_duplicate == FALSE);
  assert(t->duplicates == 0);

  init_triple_walker(&tw, db, t, indexed);
  while((d=next_triple(&tw)) && d != t)
  { if ( match_triples(d, t, MATCH_DUPLICATE) )
    { t->is_duplicate = TRUE;
      assert( !d->is_duplicate );

      d->duplicates++;

      DEBUG(2,
	    print_triple(t, PRT_SRC);
	    Sdprintf(" %p: %d-th duplicate: ", t, d->duplicates);
	    Sdprintf("Principal: %p at", d);
	    print_src(d);
	    Sdprintf("\n"));

      assert(d->duplicates);		/* check overflow */
      db->duplicates++;
      return TRUE;
    }
  }

  return FALSE;
}


static void				/* t is about to be deleted */
update_duplicates_del(rdf_db *db, triple *t)
{ const int indexed = BY_SPO;

  if ( t->duplicates )			/* I am the principal one */
  { triple_walker tw;
    triple *d;

    DEBUG(2,
	  print_triple(t, PRT_SRC);
	  Sdprintf(": DEL principal %p, %d duplicates: ", t, t->duplicates));

    db->duplicates--;
    init_triple_walker(&tw, db, t, indexed);
    while( (d=next_triple(&tw)) )
    { if ( d != t && match_triples(d, t, MATCH_DUPLICATE) )
      { assert(d->is_duplicate);
	d->is_duplicate = FALSE;
	d->duplicates = t->duplicates-1;
	DEBUG(2,
	      Sdprintf("New principal: %p at", d);
	      print_src(d);
	      Sdprintf("\n"));

	return;
      }
    }
    assert(0);
  } else if ( t->is_duplicate )		/* I am a duplicate */
  { triple_walker tw;
    triple *d;

    DEBUG(2,
	  print_triple(t, PRT_SRC);
	  Sdprintf(": DEL: is a duplicate: "));

    db->duplicates--;
    init_triple_walker(&tw, db, t, indexed);
    while( (d=next_triple(&tw)) )
    { if ( d != t && match_triples(d, t, MATCH_DUPLICATE) )
      { if ( d->duplicates )
	{ d->duplicates--;
	  DEBUG(2,
		Sdprintf("Principal %p at ", d);
		print_src(d);
		Sdprintf(" has %d duplicates\n", d->duplicates));
	  return;
	}
      }
    }
    Sdprintf("FATAL\n");
    PL_halt(1);
    assert(0);
  }
}


		 /*******************************
		 *	    TRANSACTIONS	*
		 *******************************/

int
put_begin_end(term_t t, functor_t be, int level)
{ term_t av;

  return ( (av = PL_new_term_ref()) &&
	   PL_put_integer(av, level) &&
	   PL_cons_functor_v(t, be, av) );
}


static foreign_t
rdf_transaction(term_t goal, term_t id)
{ int rc;
  rdf_db *db = DB;
  query *q;
  triple_buffer added;
  triple_buffer deleted;

  q = open_transaction(db, &added, &deleted);
  q->transaction_data.prolog_id = id;
  rc = PL_call_predicate(NULL, PL_Q_PASS_EXCEPTION, PRED_call1, goal);

  if ( rc )
  { if ( !empty_transaction(q) )
    { term_t be;

      if ( !(be=PL_new_term_ref()) ||
	   !put_begin_end(be, FUNCTOR_begin1, 0) ||
	   !broadcast(EV_TRANSACTION, (void*)id, (void*)be) ||
	   !put_begin_end(be, FUNCTOR_end1, 0) )
	return FALSE;

      commit_transaction(q);

      if ( !broadcast(EV_TRANSACTION, (void*)id, (void*)be) )
	return FALSE;
    } else
    { close_transaction(q);
    }
  } else
  { discard_transaction(q);
  }

  return rc;
}


		 /*******************************
		 *	     PREDICATES		*
		 *******************************/

/** rdf_active_transactions_(-List)

Provides list of parent transactions in the calling thread
*/

static foreign_t
rdf_active_transactions(term_t list)
{ rdf_db *db = DB;
  query *q = open_query(db);
  term_t tail = PL_copy_term_ref(list);
  term_t head = PL_new_term_ref();
  query *t;

  for(t = q->transaction; t; t=t->transaction)
  { if ( !PL_unify_list(tail, head, tail) ||
         !PL_unify(head, t->transaction_data.prolog_id) )
    { close_query(q);
      return FALSE;
    }
  }

  close_query(q);

  return PL_unify_nil(tail);
}


static foreign_t
rdf_assert4(term_t subject, term_t predicate, term_t object, term_t src)
{ rdf_db *db = DB;
  triple *t = new_triple(db);
  query *q;

  if ( !get_triple(db, subject, predicate, object, t) )
  { free_triple(db, t, FALSE);
    return FALSE;
  }
  if ( src )
  { if ( !get_graph(src, t) )
    { free_triple(db, t, FALSE);
      return FALSE;
    }
  } else
  { t->graph = ATOM_user;
    t->line = NO_LINE;
  }
  lock_atoms(db, t);

  q = open_query(db);
  add_triples(q, &t, 1);
  close_query(q);

  return TRUE;
}


static foreign_t
rdf_assert3(term_t subject, term_t predicate, term_t object)
{ return rdf_assert4(subject, predicate, object, 0);
}


static void	free_search_state(search_state *state);

static void
init_cursor_from_literal(search_state *state, literal *cursor)
{ triple *p = &state->pattern;
  size_t iv;

  DEBUG(3,
	Sdprintf("Trying literal search for ");
	print_literal(cursor);
	Sdprintf("\n"));

  p->indexed |= BY_O;
  p->indexed &= ~BY_G;			/* No graph indexing supported */
  if ( p->indexed == BY_SO )
    p->indexed = BY_S;			/* we do not have index BY_SO */

  switch(p->indexed)			/* keep in sync with triple_hash_key() */
  { case BY_O:
      iv = literal_hash(cursor);
      break;
    case BY_PO:
      iv = predicate_hash(p->predicate.r) ^ literal_hash(cursor);
      break;
    case BY_SPO:
      iv = (atom_hash(p->subject)<<1) ^
	   predicate_hash(p->predicate.r) ^
	   literal_hash(cursor);
      break;
    default:
      iv = 0;				/* make compiler silent */
      assert(0);
  }

  init_triple_walker(&state->cursor, state->db, p, p->indexed);
  state->literal_cursor = cursor;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
init_search_state(search_state *state, query *q)
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static int
init_search_state(search_state *state, query *query)
{ triple *p = &state->pattern;

  if ( get_partial_triple(state->db,
			  state->subject, state->predicate, state->object,
			  state->src, p) != TRUE )
  { free_triple(state->db, p, FALSE);
    return FALSE;
  }

  state->query = query;

  if ( (p->match == STR_MATCH_PREFIX ||	p->match == STR_MATCH_LIKE) &&
       p->indexed != BY_SP &&
       (state->prefix = first_atom(p->object.literal->value.string, p->match)))
  { literal lit;
    literal **rlitp;

    lit = *p->object.literal;
    lit.value.string = state->prefix;
    state->lit_ex.literal = &lit;
    prepare_literal_ex(&state->lit_ex);
    rlitp = skiplist_find_first(&state->db->literals,
				&state->lit_ex, &state->literal_state);
    if ( rlitp )
    { init_cursor_from_literal(state, *rlitp);
      state->has_literal_state = TRUE;
    } else
    { free_search_state(state);
      return FALSE;
    }
  } else if ( p->indexed != BY_SP && p->match >= STR_MATCH_LE )
  { literal **rlitp;

    state->lit_ex.literal = p->object.literal;
    prepare_literal_ex(&state->lit_ex);

    switch(p->match)
    { case STR_MATCH_LE:
	rlitp = skiplist_find_first(&state->db->literals,
				    NULL, &state->literal_state);
        break;
      case STR_MATCH_GE:
	rlitp = skiplist_find_first(&state->db->literals,
				    &state->lit_ex, &state->literal_state);
        break;
      case STR_MATCH_BETWEEN:
	rlitp = skiplist_find_first(&state->db->literals,
				    &state->lit_ex, &state->literal_state);
        state->lit_ex.literal = &p->tp.end;
	prepare_literal_ex(&state->lit_ex);
        break;
      default:
	assert(0);
    }

    if ( rlitp )
    { init_cursor_from_literal(state, *rlitp);
    } else
    { free_search_state(state);
      return FALSE;
    }
  } else
  { init_triple_walker(&state->cursor, state->db, p, p->indexed);
  }

  return TRUE;
}


static void
free_search_state(search_state *state)
{ if ( state->query )
    close_query(state->query);

  free_triple(state->db, &state->pattern, FALSE);
  if ( state->prefix )
    PL_unregister_atom(state->prefix);
  if ( state->allocated )		/* also means redo! */
    rdf_free(state->db, state, sizeof(*state));
  destroy_triple_walker(state->db, &state->cursor);
}


static foreign_t
allow_retry_state(search_state *state)
{ PL_retry_address(state);
}


/* TBD: simplify.   Maybe split for resource and literal search, as
   both involve mutual exclusive complications to this routine,
*/

static int
next_search_state(search_state *state)
{ triple *t;
  triple_walker *tw = &state->cursor;
  triple *p = &state->pattern;
  term_t retpred;
  int unify_pred;

  if ( state->realpred )
  { retpred = state->realpred;
    if ( PL_is_variable(state->predicate) )
    { if ( !PL_unify(state->predicate, retpred) )
	return FALSE;
    }
  } else
  { retpred = state->predicate;
    unify_pred = FALSE;
  }

retry:
  while( (t = next_triple(tw)) )
  { if ( t->is_duplicate && !state->src )
      continue;				/* TBD: Table of returned */
    if ( !alive_triple(state->query, t) )
      continue;

					/* hash-collision, skip */
    if ( state->has_literal_state )
    { if ( !(t->object_is_literal &&
	     t->object.literal == state->literal_cursor) )
	continue;
    }

    if ( match_triples(t, p, state->flags) )
    { int rc;

      if ( (rc=unify_triple(state->subject, retpred, state->object,
			    state->src, t, p->inversed)) == FALSE )
	continue;
      if ( rc == ERROR )
	return FALSE;				/* makes rdf/3 return FALSE */

    inv_alt:
      while( (t = next_triple(tw)) )
      { if ( state->has_literal_state )
	{ if ( !(t->object_is_literal &&
		 t->object.literal == state->literal_cursor) )
	    continue;
	}

	if ( match_triples(t, p, state->flags) )
	{ set_next_triple(tw, t);

	  return TRUE;			/* non-deterministic */
	}
      }

      if ( (state->flags & MATCH_INVERSE) && inverse_partial_triple(p) )
      { init_triple_walker(tw, state->db, p, p->indexed);
	goto inv_alt;
      }

      return TRUE;			/* deterministic */
    }
  }

  if ( (state->flags & MATCH_INVERSE) && inverse_partial_triple(p) )
  { init_triple_walker(tw, state->db, p, p->indexed);
    goto retry;
  }

  if ( state->has_literal_state )
  { literal **litp;

    if ( (litp = skiplist_find_next(&state->literal_state)) )
    { literal *lit = *litp;

      DEBUG(2, Sdprintf("next: ");
	       print_literal(lit);
	       Sdprintf("\n"));

      switch(state->pattern.match)
      { case STR_MATCH_PREFIX:
	{ if ( !match_atoms(STR_MATCH_PREFIX, state->prefix, lit->value.string) )
	  { DEBUG(1,
		  Sdprintf("PREFIX: terminated literal iteration from ");
		  print_literal(lit);
		  Sdprintf("\n"));
	    return FALSE;			/* no longer a prefix */
	  }

	  break;
	}
	case STR_MATCH_LE:
	case STR_MATCH_BETWEEN:
	{ if ( compare_literals(&state->lit_ex, lit) < 0 )
	  { DEBUG(1,
		  Sdprintf("LE/BETWEEN(");
		  print_literal(state->lit_ex.literal);
		  Sdprintf("): terminated literal iteration from ");
		  print_literal(lit);
		  Sdprintf("\n"));
	    return FALSE;			/* no longer a prefix */
	  }

	  break;
	}
      }

      init_cursor_from_literal(state, lit);

      goto retry;
    }
  }

  return FALSE;
}



static foreign_t
rdf(term_t subject, term_t predicate, term_t object,
    term_t src, term_t realpred, control_t h, unsigned flags)
{ rdf_db *db = DB;
  search_state *state;

  switch(PL_foreign_control(h))
  { case PL_FIRST_CALL:
    { query *q = open_query(db);

      state = &q->search_state;
      memset(state, 0, sizeof(*state));
      state->db	       = db;
      state->subject   = subject;
      state->object    = object;
      state->predicate = predicate;
      state->src       = src;
      state->realpred  = realpred;
      state->flags     = flags;

      if ( !init_search_state(state, q) )
	return FALSE;

      goto search;
    }
    case PL_REDO:
    { int rc;

      state = PL_foreign_context_address(h);
      assert(state->subject == subject);

    search:
      if ( (rc=next_search_state(state)) )
      { if ( state->cursor.current || state->has_literal_state )
	  return allow_retry_state(state);
      }

      free_search_state(state);
      return rc;
    }
    case PL_PRUNED:
    { state = PL_foreign_context_address(h);

      free_search_state(state);
      return TRUE;
    }
    default:
      assert(0);
      return FALSE;
  }
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
rdf(Subject, Predicate, Object)

Search specifications:

	Predicate:

		subPropertyOf(X) = P

	Object:

		literal(substring(X), L)
		literal(word(X), L)
		literal(exact(X), L)
		literal(prefix(X), L)
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */


static foreign_t
rdf3(term_t subject, term_t predicate, term_t object, control_t h)
{ return rdf(subject, predicate, object, 0, 0, h,
	     MATCH_EXACT);
}


static foreign_t
rdf4(term_t subject, term_t predicate, term_t object,
     term_t src, control_t h)
{ return rdf(subject, predicate, object, src, 0, h,
	     MATCH_EXACT|MATCH_SRC);
}


static foreign_t
rdf_has(term_t subject, term_t predicate, term_t object,
	term_t realpred, control_t h)
{ return rdf(subject, predicate, object, 0, realpred, h,
	     MATCH_SUBPROPERTY|MATCH_INVERSE);
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
rdf_estimate_complexity(+S,+P,+O,-C)

- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static foreign_t
rdf_estimate_complexity(term_t subject, term_t predicate, term_t object,
		        term_t complexity)
{ triple t;
  size_t c;
  rdf_db *db = DB;
  int rc;

  memset(&t, 0, sizeof(t));
  if ( (rc=get_partial_triple(db, subject, predicate, object, 0, &t)) != TRUE )
  { if ( rc == -1 )
    { return FALSE;			/* error */
    } else
    { return PL_unify_integer(complexity, 0);	/* no predicate */
    }
  }

  if ( t.indexed == BY_NONE )
  { c = db->created - db->erased;		/* = totale triple count */
#if 0
  } else if ( t.indexed == BY_P )
  { c = t.predicate.r->triple_count;		/* must sum over children */
#endif
  } else
  { size_t key = triple_hash_key(&t, t.indexed);
    triple_hash *hash = &db->hash[ICOL(t.indexed)];
    size_t count;

    c = 0;
    for(count=hash->bucket_count_epoch; count <= hash->bucket_count; count *= 2)
    { int entry = key%count;
      triple_bucket *bucket = &hash->blocks[MSB(entry)][entry];

      c += bucket->count;		/* TBD: compensate for resize */
    }
  }

  rc = PL_unify_int64(complexity, c);
  free_triple(db, &t, FALSE);

  return rc;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
current_literal(?Literals)
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static foreign_t
rdf_current_literal(term_t t, control_t h)
{ rdf_db *db = DB;
  literal **data;
  skiplist_enum *state;
  int rc;

  switch(PL_foreign_control(h))
  { case PL_FIRST_CALL:
      if ( PL_is_variable(t) )
      { state = rdf_malloc(db, sizeof(*state));

	data = skiplist_find_first(&db->literals, NULL, state);
	goto next;
      } else
      { return FALSE;			/* TBD */
      }
    case PL_REDO:
      state = PL_foreign_context_address(h);
      data  = skiplist_find_next(state);
    next:
      for(; data; data=skiplist_find_next(state))
      { literal *lit = *data;

	if ( unify_literal(t, lit) )
	{ PL_retry_address(state);
	}
      }

      rc = FALSE;
      goto cleanup;
    case PL_PRUNED:
      rc = TRUE;

    cleanup:
      state = PL_foreign_context_address(h);
      rdf_free(db, state, sizeof(*state));

      return rc;
    default:
      assert(0);
      return FALSE;
  }
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
rdf_update(+Subject, +Predicate, +Object, +Action)

Update a triple. Please note this is actually erase+assert as the triple
needs to be updated in  the  linked   lists  while  erase simply flags a
triple as `erases' without deleting it   to support queries which active
choicepoints.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static int
update_triple(rdf_db *db, term_t action, triple *t, triple **updated)
{ term_t a = PL_new_term_ref();
  triple tmp, *new;
  int i;
					/* Create copy in local memory */
  tmp = *t;
  tmp.allocated = FALSE;
  tmp.atoms_locked = FALSE;
  if ( t->object_is_literal )
    tmp.object.literal = copy_literal(db, t->object.literal);

  if ( !PL_get_arg(1, action, a) )
    return PL_type_error("rdf_action", action);

  if ( PL_is_functor(action, FUNCTOR_subject1) )
  { atom_t s;

    if ( !PL_get_atom_ex(a, &s) )
      return FALSE;
    if ( tmp.subject == s )
      return TRUE;			/* no change */

    tmp.subject = s;
  } else if ( PL_is_functor(action, FUNCTOR_predicate1) )
  { predicate *p;

    if ( !get_predicate(db, a, &p) )
      return FALSE;
    if ( tmp.predicate.r == p )
      return TRUE;			/* no change */

    tmp.predicate.r = p;
  } else if ( PL_is_functor(action, FUNCTOR_object1) )
  { triple t2;

    memset(&t2, 0, sizeof(t2));

    if ( !get_object(db, a, &t2) )
    { free_triple(db, &t2, FALSE);
      return FALSE;
    }
    if ( match_object(&t2, &tmp, MATCH_QUAL) )
    { free_triple(db, &t2, FALSE);
      return TRUE;
    }

    if ( tmp.object_is_literal )
      free_literal(db, tmp.object.literal);
    if ( (tmp.object_is_literal = t2.object_is_literal) )
    { tmp.object.literal = t2.object.literal;
    } else
    { tmp.object.resource = t2.object.resource;
    }
  } else if ( PL_is_functor(action, FUNCTOR_graph1) )
  { triple t2;

    if ( !get_graph(a, &t2) )
      return FALSE;
    if ( t2.graph == t->graph && t2.line == t->line )
    { *updated = NULL;
      return TRUE;
    }

    tmp.graph = t2.graph;
    tmp.line = t2.line;
  } else
    return PL_domain_error("rdf_action", action);

  for(i=0; i<INDEX_TABLES; i++)
    tmp.tp.next[i] = NULL;

  new = new_triple(db);
  new->subject		 = tmp.subject;
  new->predicate.r	 = tmp.predicate.r;
  if ( (new->object_is_literal = tmp.object_is_literal) )
  { new->object.literal = copy_literal(db, tmp.object.literal);
  } else
  { new->object.resource = tmp.object.resource;
  }
  new->graph		 = tmp.graph;
  new->line		 = tmp.line;

  free_triple(db, &tmp, FALSE);
  lock_atoms(db, new);

  *updated = new;

  return TRUE;
}


static foreign_t
rdf_update5(term_t subject, term_t predicate, term_t object, term_t src,
	    term_t action)
{ triple t, *p;
  int indexed = BY_SPO;
  rdf_db *db = DB;
  int rc = TRUE;
  size_t count;
  triple_walker tw;
  triple_buffer matches;
  query *q;

  memset(&t, 0, sizeof(t));

  if ( !get_src(src, &t) ||
       !get_triple(db, subject, predicate, object, &t) )
    return FALSE;

  q = open_query(db);
  init_triple_buffer(&matches);
  init_triple_walker(&tw, db, &t, indexed);
  while((p=next_triple(&tw)))
  { if ( !alive_triple(q, p) )
      continue;

    if ( match_triples(p, &t, MATCH_EXACT) )
      buffer_triple(&matches, p);
  }

  if ( !is_empty_buffer(&matches) )
  { triple_buffer replacements;
    triple *new, **tp;
    size_t updated = 0;

    count = matches.top-matches.base;
    init_triple_buffer(&replacements);
    for(tp=matches.base; tp<matches.top; tp++)
    { new = NULL;
      if ( !update_triple(db, action, *tp, &new) )
      { rc = FALSE;
	free_triple_buffer(&replacements);
	goto out;
      }

      updated++;
      buffer_triple(&replacements, new);
    }

    if ( updated )
      update_triples(q, matches.base, replacements.base, count);
    free_triple_buffer(&replacements);
  } else
  { count = 0;
  }

out:
  close_query(q);
  free_triple_buffer(&matches);
  free_triple(db, &t, FALSE);

  return (rc && count > 0) ? TRUE : FALSE;
}


static foreign_t
rdf_update(term_t subject, term_t predicate, term_t object, term_t action)
{ return rdf_update5(subject, predicate, object, 0, action);
}


static foreign_t
rdf_retractall4(term_t subject, term_t predicate, term_t object, term_t src)
{ triple t, *p;
  rdf_db *db = DB;
  triple_walker tw;
  triple_buffer buf;
  query *q;

  memset(&t, 0, sizeof(t));
  switch( get_partial_triple(db, subject, predicate, object, src, &t) )
  { case 0:				/* no such predicate */
      return TRUE;
    case -1:				/* error */
      return FALSE;
  }

  if ( t.graph	)		/* speedup for rdf_retractall(_,_,_,DB) */
  { graph *gr = existing_graph(db, t.graph);

    if ( !gr || gr->triple_count == 0 )
      return TRUE;
  }

  init_triple_buffer(&buf);
  q = open_query(db);
  init_triple_walker(&tw, db, &t, t.indexed);
  while((p=next_triple(&tw)))
  { if ( match_triples(p, &t, MATCH_EXACT|MATCH_SRC) )
    { if ( t.object_is_literal && t.object.literal->objtype == OBJ_TERM )
      { fid_t fid = PL_open_foreign_frame();
	int rc = unify_object(object, p);
	PL_discard_foreign_frame(fid);
	if ( !rc )
	  continue;
      }

      buffer_triple(&buf, p);
    }
  }
  free_triple(db, &t, FALSE);
  del_triples(q, buf.base, buf.top-buf.base);
  close_query(q);
  free_triple_buffer(&buf);


  return TRUE;
}


static foreign_t
rdf_retractall3(term_t subject, term_t predicate, term_t object)
{ return rdf_retractall4(subject, predicate, object, 0);
}


		 /*******************************
		 *	     MONITOR		*
		 *******************************/

typedef struct broadcast_callback
{ struct broadcast_callback *next;
  predicate_t		     pred;
  long			     mask;
} broadcast_callback;

static long joined_mask = 0L;
static broadcast_callback *callback_list;
static broadcast_callback *callback_tail;

static int
do_broadcast(term_t term, long mask)
{ if ( callback_list )
  { broadcast_callback *cb;

    for(cb = callback_list; cb; cb = cb->next)
    { qid_t qid;
      term_t ex;

      if ( !(cb->mask & mask) )
	continue;

      if ( !(qid = PL_open_query(NULL, PL_Q_CATCH_EXCEPTION, cb->pred, term)) )
	return FALSE;
      if ( !PL_next_solution(qid) && (ex = PL_exception(qid)) )
      { term_t av = PL_new_term_refs(2);

	PL_cut_query(qid);

	PL_put_atom(av+0, ATOM_error);
	PL_put_term(av+1, ex);

	PL_call_predicate(NULL, PL_Q_NORMAL,
			  PL_predicate("print_message", 2, "user"),
			  av);
	return FALSE;
      } else
      { PL_close_query(qid);
      }
    }
  }

  return TRUE;
}


static int
broadcast(broadcast_id id, void *a1, void *a2)
{ int rc = TRUE;

  if ( (joined_mask & id) )
  { fid_t fid;
    term_t term;
    functor_t funct;

    if ( !(fid = PL_open_foreign_frame()) ||
	 !(term = PL_new_term_ref()) )
      return FALSE;

    switch(id)
    { case EV_ASSERT:
      case EV_ASSERT_LOAD:
	funct = FUNCTOR_assert4;
        goto assert_retract;
      case EV_RETRACT:
	funct = FUNCTOR_retract4;
      assert_retract:
      { triple *t = a1;
	term_t tmp;

	if ( !(tmp = PL_new_term_refs(4)) ||
	     !PL_put_atom(tmp+0, t->subject) ||
	     !PL_put_atom(tmp+1, t->predicate.r->name) ||
	     !unify_object(tmp+2, t) ||
	     !unify_graph(tmp+3, t) ||
	     !PL_cons_functor_v(term, funct, tmp) )
	  return FALSE;
	break;
      }
      case EV_UPDATE:
      { triple *t = a1;
	triple *new = a2;
	term_t tmp, a;
	functor_t action;
	int rc;

	if ( !(tmp = PL_new_term_refs(5)) ||
	     !(a = PL_new_term_ref()) ||
	     !PL_put_atom(tmp+0, t->subject) ||
	     !PL_put_atom(tmp+1, t->predicate.r->name) ||
	     !unify_object(tmp+2, t) ||
	     !unify_graph(tmp+3, t) )
	  return FALSE;

	if ( t->subject != new->subject )
	{ action = FUNCTOR_subject1;
	  rc = PL_put_atom(a, new->subject);
	} else if ( t->predicate.r != new->predicate.r )
	{ action = FUNCTOR_predicate1;
	  rc = PL_put_atom(a, new->predicate.r->name);
	} else if ( !match_object(t, new, MATCH_QUAL) )
	{ action = FUNCTOR_object1;
	  rc = unify_object(a, new);
	} else if ( !same_graph(t, new) )
	{ action = FUNCTOR_graph1;
	  rc = unify_graph(a, new);
	} else
	{ return TRUE;			/* no change */
	}

        if ( !rc ||
	     !PL_cons_functor_v(tmp+4, action, a) ||
	     !PL_cons_functor_v(term, FUNCTOR_update5, tmp) )
	  return FALSE;
	break;
      }
      case EV_NEW_LITERAL:
      { literal *lit = a1;
	term_t tmp;

	if ( !(tmp = PL_new_term_refs(1)) ||
	     !unify_literal(tmp, lit) ||
	     !PL_cons_functor_v(term, FUNCTOR_new_literal1, tmp) )
	  return FALSE;
	break;
      }
      case EV_OLD_LITERAL:
      { literal *lit = a1;
	term_t tmp;

	if ( !(tmp = PL_new_term_refs(1)) ||
	     !unify_literal(tmp, lit) ||
	     !PL_cons_functor_v(term, FUNCTOR_old_literal1, tmp) )
	  return FALSE;
	break;
      }
      case EV_LOAD:
      { term_t ctx = (term_t)a1;
	atom_t be  = (atom_t)a2;
	term_t tmp;

	if ( !(tmp = PL_new_term_refs(2)) ||
	     !PL_put_atom(tmp+0, be) ||		/* begin/end */
	     !PL_put_term(tmp+1, ctx) ||
	     !PL_cons_functor_v(term, FUNCTOR_load2, tmp) )
	  return FALSE;
	break;
      }
      case EV_TRANSACTION:
      { term_t ctx = (term_t)a1;
	term_t be  = (term_t)a2;
	term_t tmp;

	if ( !(tmp = PL_new_term_refs(2)) ||
	     !PL_put_term(tmp+0, be) ||		/* begin/end */
	     !PL_put_term(tmp+1, ctx) ||
	     !PL_cons_functor_v(term, FUNCTOR_transaction2, tmp) )
	  return FALSE;
	break;
      }
      case EV_REHASH:
      { atom_t be = (atom_t)a1;
	term_t tmp = PL_new_term_refs(1);

	if ( !(tmp = PL_new_term_refs(1)) ||
	     !PL_put_atom(tmp+0, be) ||
	     !PL_cons_functor_v(term, FUNCTOR_rehash1, tmp) )
	  return FALSE;
	break;
      }
      default:
	assert(0);
    }

    rc = do_broadcast(term, id);

    PL_discard_foreign_frame(fid);
  }

  return rc;
}


static foreign_t
rdf_monitor(term_t goal, term_t mask)
{ atom_t name;
  broadcast_callback *cb;
  predicate_t p;
  long msk;
  module_t m = NULL;

  PL_strip_module(goal, &m, goal);

  if ( !PL_get_atom_ex(goal, &name) ||
       !PL_get_long_ex(mask, &msk) )
    return FALSE;

  p = PL_pred(PL_new_functor(name, 1), m);

  for(cb=callback_list; cb; cb = cb->next)
  { if ( cb->pred == p )
    { broadcast_callback *cb2;
      cb->mask = msk;

      joined_mask = 0L;
      for(cb2=callback_list; cb2; cb2 = cb2->next)
	joined_mask |= cb2->mask;
      DEBUG(2, Sdprintf("Set mask to 0x%x\n", joined_mask));

      return TRUE;
    }
  }

  cb = PL_malloc(sizeof(*cb));
  cb->next = NULL;
  cb->mask = msk;
  cb->pred = p;
  if ( callback_list )
  { callback_tail->next = cb;
    callback_tail = cb;
  } else
  { callback_list = callback_tail = cb;
  }
  joined_mask |= msk;

  return TRUE;
}



static foreign_t
rdf_set_predicate(term_t pred, term_t option)
{ predicate *p;
  rdf_db *db = DB;

  if ( !get_predicate(db, pred, &p) )
    return FALSE;

  if ( PL_is_functor(option, FUNCTOR_symmetric1) )
  { int val;

    if ( !get_bool_arg_ex(1, option, &val) )
      return FALSE;

    if ( val )
      p->inverse_of = p;
    else
      p->inverse_of = NULL;
    return TRUE;
  } else if ( PL_is_functor(option, FUNCTOR_inverse_of1) )
  { term_t a = PL_new_term_ref();
    predicate *i;

    _PL_get_arg(1, option, a);
    if ( PL_get_nil(a) )
    { if ( p->inverse_of )
      { p->inverse_of->inverse_of = NULL;
	p->inverse_of = NULL;
      }
    } else
    { if ( !get_predicate(db, a, &i) )
	return FALSE;

      p->inverse_of = i;
      i->inverse_of = p;
    }
    return TRUE;
  } else if ( PL_is_functor(option, FUNCTOR_transitive1) )
  { int val;

    if ( !get_bool_arg_ex(1, option, &val) )
      return FALSE;

    p->transitive = val;

    return TRUE;
  } else
    return PL_type_error("predicate_option", option);
}


#define PRED_PROPERTY_COUNT 9
static functor_t predicate_key[PRED_PROPERTY_COUNT];

static int
unify_predicate_property(rdf_db *db, predicate *p, term_t option, functor_t f)
{ if ( f == FUNCTOR_symmetric1 )
    return PL_unify_term(option, PL_FUNCTOR, f,
			 PL_BOOL, p->inverse_of == p ? TRUE : FALSE);
  else if ( f == FUNCTOR_inverse_of1 )
  { if ( p->inverse_of )
      return PL_unify_term(option, PL_FUNCTOR, f,
			   PL_ATOM, p->inverse_of->name);
    else
      return FALSE;
  } else if ( f == FUNCTOR_transitive1 )
  { return PL_unify_term(option, PL_FUNCTOR, f,
			 PL_BOOL, p->transitive);
  } else if ( f == FUNCTOR_triples1 )
  { return PL_unify_term(option, PL_FUNCTOR, f,
			 PL_LONG, p->triple_count);
  } else if ( f == FUNCTOR_rdf_subject_branch_factor1 )
  { return PL_unify_term(option, PL_FUNCTOR, f,
		 PL_FLOAT, subject_branch_factor(db, p, DISTINCT_DIRECT));
  } else if ( f == FUNCTOR_rdf_object_branch_factor1 )
  { return PL_unify_term(option, PL_FUNCTOR, f,
		 PL_FLOAT, object_branch_factor(db, p, DISTINCT_DIRECT));
  } else if ( f == FUNCTOR_rdfs_subject_branch_factor1 )
  { return PL_unify_term(option, PL_FUNCTOR, f,
		 PL_FLOAT, subject_branch_factor(db, p, DISTINCT_SUB));
  } else if ( f == FUNCTOR_rdfs_object_branch_factor1 )
  { return PL_unify_term(option, PL_FUNCTOR, f,
		 PL_FLOAT, object_branch_factor(db, p, DISTINCT_SUB));
  } else
  { assert(0);
    return FALSE;
  }
}


typedef struct enum_pred
{ predicate *p;
  int i;
} enum_pred;


static foreign_t
rdf_current_predicate(term_t name, control_t h)
{ rdf_db *db = DB;
  predicate *p;
  enum_pred *ep;
  atom_t a;

  switch( PL_foreign_control(h) )
  { case PL_FIRST_CALL:
      if ( PL_is_variable(name) )
      { ep = rdf_malloc(db, sizeof(*ep));
	ep->i  = 0;
	ep->p  = NULL;
	goto next;
      } else if ( PL_get_atom_ex(name, &a) )
      { predicate *p;

	if ( (p=existing_predicate(db, a)) &&
	     p->triple_count > 0 )
	  return TRUE;
      }
      return FALSE;
    case PL_REDO:
      ep = PL_foreign_context_address(h);
      goto next;
    case PL_PRUNED:
      ep = PL_foreign_context_address(h);
      rdf_free(db, ep, sizeof(*ep));
      return TRUE;
    default:
      assert(0);
      return FALSE;
  }

next:
  if ( !(p=ep->p) )
  { while (!(p = db->predicates.blocks[MSB(ep->i)][ep->i]) )
    { if ( ++ep->i >= db->predicates.bucket_count )
	goto fail;
    }
  }

  if ( !PL_unify_atom(name, p->name) )
  { fail:
    rdf_free(db, ep, sizeof(*ep));
    return FALSE;
  }

  if ( !(ep->p = p->next) )
  { if ( ++ep->i >= db->predicates.bucket_count )
      goto fail;
  }
  PL_retry_address(ep);
}


static foreign_t
rdf_predicate_property(term_t pred, term_t option, control_t h)
{ int n;
  predicate *p;
  rdf_db *db = DB;

  if ( !predicate_key[0] )
  { int i = 0;

    predicate_key[i++] = FUNCTOR_symmetric1;
    predicate_key[i++] = FUNCTOR_inverse_of1;
    predicate_key[i++] = FUNCTOR_transitive1;
    predicate_key[i++] = FUNCTOR_triples1;
    predicate_key[i++] = FUNCTOR_rdf_subject_branch_factor1;
    predicate_key[i++] = FUNCTOR_rdf_object_branch_factor1;
    predicate_key[i++] = FUNCTOR_rdfs_subject_branch_factor1;
    predicate_key[i++] = FUNCTOR_rdfs_object_branch_factor1;
    assert(i < PRED_PROPERTY_COUNT);
  }

  switch(PL_foreign_control(h))
  { case PL_FIRST_CALL:
    { functor_t f;

      if ( PL_is_variable(option) )
      { n = 0;
	goto redo;
      } else if ( PL_get_functor(option, &f) )
      { for(n=0; predicate_key[n]; n++)
	{ if ( predicate_key[n] == f )
	  { if ( !get_predicate(db, pred, &p) )
	      return FALSE;
	    return unify_predicate_property(db, p, option, f);
	  }
	}
	return PL_domain_error("rdf_predicate_property", option);
      } else
	return PL_type_error("rdf_predicate_property", option);
    }
    case PL_REDO:
      n = (int)PL_foreign_context(h);
    redo:
      if ( !get_predicate(db, pred, &p) )
	return FALSE;
      for( ; predicate_key[n]; n++ )
      { if ( unify_predicate_property(db, p, option, predicate_key[n]) )
	{ n++;
	  if ( predicate_key[n] )
	    PL_retry(n);
	  return TRUE;
	}
      }
      return FALSE;
    case PL_PRUNED:
      return TRUE;
    default:
      assert(0);
      return TRUE;
  }
}


		 /*******************************
		 *     TRANSITIVE RELATIONS	*
		 *******************************/

typedef struct visited
{ struct visited *next;			/* next in list */
  struct visited *hash_link;		/* next in hashed link */
  atom_t resource;			/* visited resource */
  uintptr_t distance;			/* Distance */
} visited;


#define AGENDA_LOCAL_MAGIC 742736360
#define AGENDA_SAVED_MAGIC 742736362

typedef struct agenda
{ query   *query;			/* associated query */
  visited *head;			/* visited list */
  visited *tail;			/* tail of visited list */
  visited *to_expand;			/* next to expand */
  visited *to_return;			/* next to return */
  visited **hash;			/* hash-table for cycle detection */
  int	  magic;			/* AGENDA_*_MAGIC */
  int	  hash_size;
  int     size;				/* size of the agenda */
  uintptr_t max_d;			/* max distance */
  triple  pattern;			/* partial triple used as pattern */
  atom_t  target;			/* resource we are seaching for */
  struct chunk  *chunk;			/* node-allocation chunks */
} agenda;

#ifndef offsetof
#define offsetof(structure, field) ((size_t) &(((structure *)NULL)->field))
#endif
#define CHUNK_SIZE(n) offsetof(chunk, nodes[n])

typedef struct chunk
{ struct chunk *next;
  int	 used;				/* # used elements */
  int	 size;				/* size of the chunk */
  struct visited nodes[1];		/* nodes in the chunk */
} chunk;


static visited *
alloc_node_agenda(rdf_db *db, agenda *a)
{ chunk *c;
  int size;

  if ( (c=a->chunk) )
  { if ( c->used < c->size )
    { visited *v = &c->nodes[c->used++];

      return v;
    }
  }

  size = (a->size == 0 ? 8 : 1024);
  c = rdf_malloc(db, CHUNK_SIZE(size));
  c->size = size;
  c->used = 1;
  c->next = a->chunk;
  a->chunk = c;

  return &c->nodes[0];
}


static void
empty_agenda(rdf_db *db, agenda *a)
{ chunk *c, *n;

  if ( a->query )
    close_query(a->query);

  for(c=a->chunk; c; c = n)
  { n = c->next;
    rdf_free(db, c, CHUNK_SIZE(c->size));
  }
  if ( a->hash )
    rdf_free(db, a->hash, sizeof(visited*)*a->hash_size);

  if ( a->magic == AGENDA_SAVED_MAGIC )
  {  a->magic = 0;
     rdf_free(db, a, sizeof(*a));
  } else
  { a->magic = 0;
  }
}


static agenda *
save_agenda(rdf_db *db, agenda *a)
{ agenda *r = rdf_malloc(db, sizeof(*r));

  assert(a->magic == AGENDA_LOCAL_MAGIC);
  *r = *a;
  r->magic = AGENDA_SAVED_MAGIC;

  return r;
}


static void
hash_agenda(rdf_db *db, agenda *a, int size)
{ if ( a->hash )
    rdf_free(db, a->hash, sizeof(*a->hash));
  if ( size > 0 )
  { visited *v;

    a->hash = rdf_malloc(db, sizeof(visited*)*size);
    memset(a->hash, 0, sizeof(visited*)*size);
    a->hash_size = size;

    for(v=a->head; v; v = v->next)
    { int key = atom_hash(v->resource)&(size-1);

      v->hash_link = a->hash[key];
      a->hash[key] = v;
    }
  }
}


static int
in_agenda(agenda *a, atom_t resource)
{ visited *v;

  if ( a->hash )
  { int key = atom_hash(resource)&(a->hash_size-1);
    v = a->hash[key];

    for( ; v; v = v->hash_link )
    { if ( v->resource == resource )
	return TRUE;
    }
  } else
  { v = a->head;

    for( ; v; v = v->next )
    { if ( v->resource == resource )
	return TRUE;
    }
  }

  return FALSE;
}


static visited *
append_agenda(rdf_db *db, agenda *a, atom_t res, uintptr_t d)
{ visited *v = a->head;

  if ( in_agenda(a, res) )
    return NULL;

  db->agenda_created++;			/* statistics */

  a->size++;
  if ( !a->hash_size && a->size > 32 )
    hash_agenda(db, a, 64);
  else if ( a->size > a->hash_size * 4 )
    hash_agenda(db, a, a->hash_size * 4);

  v = alloc_node_agenda(db, a);
  v->resource = res;
  v->distance = d;
  v->next = NULL;
  if ( a->tail )
  { a->tail->next = v;
    a->tail = v;
  } else
  { a->head = a->tail = v;
  }

  if ( a->hash_size )
  { int key = atom_hash(res)&(a->hash_size-1);

    v->hash_link = a->hash[key];
    a->hash[key] = v;
  }

  return v;
}


static int
can_reach_target(rdf_db *db, agenda *a)
{ triple_walker tw;
  int indexed = a->pattern.indexed;
  int rc = FALSE;
  triple *p;

  if ( indexed & BY_S )			/* subj ---> */
  { a->pattern.object.resource = a->target;
    indexed |= BY_O;
  } else
  { a->pattern.subject = a->target;
    indexed |= BY_S;
  }

  init_triple_walker(&tw, db, &a->pattern, indexed);
  while((p=next_triple(&tw)))
  { if ( match_triples(p, &a->pattern, MATCH_SUBPROPERTY) )
    { rc = TRUE;
      break;
    }
  }

  if ( a->pattern.indexed & BY_S )
  { a->pattern.object.resource = 0;
  } else
  { a->pattern.subject = 0;
  }

  return rc;
}



static visited *
bf_expand(rdf_db *db, agenda *a, atom_t resource, uintptr_t d)
{ triple pattern = a->pattern;
  visited *rc = NULL;

  if ( pattern.indexed & BY_S )		/* subj ---> */
  { pattern.subject = resource;
  } else
  { pattern.object.resource = resource;
  }

  if ( a->target && can_reach_target(db, a) )
    return append_agenda(db, a, a->target, d);

  for(;;)
  { int indexed = pattern.indexed;
    triple_walker tw;
    triple *p;

    init_triple_walker(&tw, db, &pattern, indexed);
    while((p=next_triple(&tw)))
    { if ( !alive_triple(a->query, p) )
	continue;

      if ( match_triples(p, &pattern, MATCH_SUBPROPERTY) )
      { atom_t found;
	visited *v;

	if ( indexed & BY_S )
	{ if ( p->object_is_literal )
	    continue;
	  found = p->object.resource;
	} else
	{ found = p->subject;
	}

	v = append_agenda(db, a, found, d);
	if ( !rc )
	  rc = v;
	if ( found == a->target )
	  return rc;
      }
    }
    if ( inverse_partial_triple(&pattern) )
      continue;
    break;
  }
					/* TBD: handle owl:sameAs */
  return rc;
}


static int
peek_agenda(rdf_db *db, agenda *a)
{ if ( a->to_return )
    return TRUE;

  while( a->to_expand )
  { uintptr_t next_d = a->to_expand->distance+1;

    if ( next_d >= a->max_d )
      return FALSE;

    a->to_return = bf_expand(db, a,
			     a->to_expand->resource,
			     next_d);
    a->to_expand = a->to_expand->next;

    if ( a->to_return )
      return TRUE;
  }

  return FALSE;
}


static visited *
next_agenda(rdf_db *db, agenda *a)
{ if ( peek_agenda(db, a) )
  { visited *v = a->to_return;

    a->to_return = a->to_return->next;

    return v;
  }

  return NULL;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
rdf_reachable(+Subject, +Predicate, -Object)
rdf_reachable(-Subject, +Predicate, ?Object)
    Examine transitive relations, reporting all `Object' that can be
    reached from `Subject' using Predicate without going into a loop
    if the relation is cyclic.

directly_attached() deals with the posibility that  the predicate is not
defined and Subject and Object are  the   same.  Should  use clean error
handling, but that means a lot of changes. For now this will do.

TBD:	Implement bi-directional search if both Subject and Object are
	given.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static int
directly_attached(term_t pred, term_t from, term_t to)
{ if ( PL_is_atom(pred) && PL_is_atom(from) )
    return PL_unify(to, from);

  return FALSE;
}


static int
unify_distance(term_t d, uintptr_t dist)
{ if ( d )
    return PL_unify_integer(d, dist);

  return TRUE;
}


static foreign_t
rdf_reachable(term_t subj, term_t pred, term_t obj,
	      term_t max_d, term_t d,
	      control_t h)
{ rdf_db *db = DB;

  switch(PL_foreign_control(h))
  { case PL_FIRST_CALL:
    { agenda a;
      visited *v;
      term_t target_term;
      int is_det = FALSE;

      if ( PL_is_variable(pred) )
	return PL_instantiation_error(pred);

      memset(&a, 0, sizeof(a));
      a.magic = AGENDA_LOCAL_MAGIC;
      if ( max_d )
      { long md;
	atom_t inf;

	if ( PL_get_atom(max_d, &inf) && inf == ATOM_infinite )
	{ a.max_d = (uintptr_t)-1;
	} else
	{ if ( !PL_get_long_ex(max_d, &md) || md < 0 )
	    return FALSE;
	  a.max_d = md;
	}
      } else
      { a.max_d = (uintptr_t)-1;
      }

      if ( !PL_is_variable(subj) )		/* subj .... obj */
      { switch(get_partial_triple(db, subj, pred, 0, 0, &a.pattern))
	{ case 0:
	    return directly_attached(pred, subj, obj) &&
		   unify_distance(d, 0);
	  case -1:
	    return FALSE;
	}
	is_det = PL_is_ground(obj);
	if ( a.pattern.object_is_literal )
	  return FALSE;			/* rdf_reachable(literal(...),?,?) */
	target_term = obj;
      } else if ( !PL_is_variable(obj) )	/* obj .... subj */
      {	switch(get_partial_triple(db, 0, pred, obj, 0, &a.pattern))
	{ case 0:
	    return directly_attached(pred, obj, subj);
	  case -1:
	    return FALSE;
	}
	if ( a.pattern.object_is_literal )
	  return FALSE;			/* rdf_reachable(-,+,literal(...)) */
	target_term = subj;
      } else
	return PL_instantiation_error(subj);

      a.query = open_query(db);
      if ( (a.pattern.indexed & BY_S) )		/* subj ... */
	append_agenda(db, &a, a.pattern.subject, 0);
      else
	append_agenda(db, &a, a.pattern.object.resource, 0);
      a.to_return = a.head;
      a.to_expand = a.head;

      while( (v=next_agenda(db, &a)) )
      { if ( PL_unify_atom(target_term, v->resource) )
	{ if ( is_det )		/* mode(+, +, +) */
	  { int rc = unify_distance(d, v->distance);
	    empty_agenda(db, &a);
	    return rc;
	  } else if ( unify_distance(d, v->distance) )
	  {				/* mode(+, +, -) or mode(-, +, +) */
	    if ( peek_agenda(db, &a) )
	    { agenda *ra = save_agenda(db, &a);
	      DEBUG(9, Sdprintf("Saved agenta to %p\n", ra));
	      PL_retry_address(ra);
	    }

	    empty_agenda(db, &a);
	    return TRUE;
	  }
	}
      }
      empty_agenda(db, &a);
      return FALSE;
    }
    case PL_REDO:
    { agenda *a = PL_foreign_context_address(h);
      term_t target_term;
      visited *v;

      assert(a->magic == AGENDA_SAVED_MAGIC);

      if ( !PL_is_variable(subj) )	/* +, +, - */
	target_term = obj;
      else
	target_term = subj;		/* -, +, + */

      while( (v=next_agenda(db, a)) )
      { if ( PL_unify_atom(target_term, v->resource) &&
	     unify_distance(d, v->distance) )
	{ assert(a->magic == AGENDA_SAVED_MAGIC);
	  if ( peek_agenda(db, a) )
	  { PL_retry_address(a);
	  } else
	  { empty_agenda(db, a);
	    return TRUE;
	  }
	}
      }

      empty_agenda(db, a);
      return FALSE;
    }
    case PL_PRUNED:
    { agenda *a = PL_foreign_context_address(h);

      DEBUG(9, Sdprintf("Cutted; agenda = %p\n", a));

      assert(a->magic == AGENDA_SAVED_MAGIC);

      empty_agenda(db, a);
      return TRUE;
    }
    default:
      assert(0);
      return FALSE;
  }
}

static foreign_t
rdf_reachable3(term_t subj, term_t pred, term_t obj, control_t h)
{ return rdf_reachable(subj, pred, obj, 0, 0, h);
}

static foreign_t
rdf_reachable5(term_t subj, term_t pred, term_t obj, term_t max_d, term_t d,
	       control_t h)
{ return rdf_reachable(subj, pred, obj, max_d, d, h);
}


		 /*******************************
		 *	     STATISTICS		*
		 *******************************/

static functor_t keys[16];		/* initialised in install_rdf_db() */

static int
unify_statistics(rdf_db *db, term_t key, functor_t f)
{ int64_t v;

  if ( f == FUNCTOR_triples1 )
  { v = db->created - db->erased;
  } else if ( f == FUNCTOR_resources1 )
  { v = db->resources.hash.count;
  } else if ( f == FUNCTOR_predicates1 )
  { v = db->predicates.count;
  } else if ( f == FUNCTOR_core1 )
  { v = db->core;
  } else if ( f == FUNCTOR_indexed16 )
  { int i;
    term_t a = PL_new_term_ref();

    if ( !PL_unify_functor(key, FUNCTOR_indexed16) )
      return FALSE;
    for(i=0; i<16; i++)
    { if ( !PL_get_arg(i+1, key, a) ||
	   !PL_unify_integer(a, db->indexed[i]) )
	return FALSE;
    }

    return TRUE;
  } else if ( f == FUNCTOR_hash_quality1 )
  { term_t tail, list = PL_new_term_ref();
    term_t head = PL_new_term_ref();
    term_t tmp = PL_new_term_ref();
    term_t av = PL_new_term_refs(3);
    int i;

    if ( !PL_unify_functor(key, FUNCTOR_hash_quality1) )
      return FALSE;
    _PL_get_arg(1, key, list);
    tail = PL_copy_term_ref(list);

    for(i=0; i<INDEX_TABLES; i++)
    { if ( !PL_unify_list(tail, head, tail) ||
	   !PL_put_integer(av+0, col_index[i]) ||
	   !PL_put_integer(av+1, db->hash[i].bucket_count) ||
	   !PL_put_float(av+2, triple_hash_quality(db, i)) ||
	   !PL_cons_functor_v(tmp, FUNCTOR_hash3, av) ||
	   !PL_unify(head, tmp) )
	return FALSE;
    }

    return PL_unify_nil(tail);
  } else if ( f == FUNCTOR_searched_nodes1 )
  { v = db->agenda_created;
  } else if ( f == FUNCTOR_duplicates1 )
  { v = db->duplicates;
  } else if ( f == FUNCTOR_literals1 )
  { v = db->literals.count;
  } else if ( f == FUNCTOR_triples2 && PL_is_functor(key, f) )
  { graph *src;
    term_t a = PL_new_term_ref();
    atom_t name;

    _PL_get_arg(1, key, a);
    if ( !PL_get_atom_ex(a, &name) )
      return FALSE;
    if ( (src = existing_graph(db, name)) )
      v = src->triple_count;
    else
      v = 0;

    _PL_get_arg(2, key, a);
    return PL_unify_int64(a, v);
  } else if ( f == FUNCTOR_gc2 )
  { return PL_unify_term(key,
			 PL_FUNCTOR, f,
			   PL_INT, db->gc.count,
			   PL_FLOAT, db->gc.time);	/* time spent */
  } else
    assert(0);

  return PL_unify_term(key, PL_FUNCTOR, f, PL_INT64, v);
}

static foreign_t
rdf_statistics(term_t key, control_t h)
{ int n;
  rdf_db *db = DB;

  switch(PL_foreign_control(h))
  { case PL_FIRST_CALL:
    { functor_t f;

      if ( PL_is_variable(key) )
      { n = 0;
	goto redo;
      } else if ( PL_get_functor(key, &f) )
      { for(n=0; keys[n]; n++)
	{ if ( keys[n] == f )
	    return unify_statistics(db, key, f);
	}
	return PL_domain_error("rdf_statistics", key);
      } else
	return PL_type_error("rdf_statistics", key);
    }
    case PL_REDO:
      n = (int)PL_foreign_context(h);
    redo:
      unify_statistics(db, key, keys[n]);
      n++;
      if ( keys[n] )
	PL_retry(n);
    case PL_PRUNED:
      return TRUE;
    default:
      assert(0);
      return TRUE;
  }
}


static foreign_t
rdf_generation(term_t t)
{ rdf_db *db = DB;

  return PL_unify_integer(t, db->generation);
}


		 /*******************************
		 *	       RESET		*
		 *******************************/

static void
erase_triples(rdf_db *db)
{ triple *t, *n;
  int i;

  for(t=db->by_none.head; t; t=n)
  { n = t->tp.next[ICOL(BY_NONE)];

    free_triple(db, t, FALSE);		/* ? */
    db->freed++;
  }
  db->by_none.head = db->by_none.tail = NULL;

  for(i=BY_S; i<INDEX_TABLES; i++)
  { triple_hash *hash = &db->hash[i];

    reset_triple_hash(db, hash);
  }

  db->created = 0;
  db->erased = 0;
  db->freed = 0;
  db->erased = 0;
  db->rehash_count = 0;
  memset(db->indexed, 0, sizeof(db->indexed));
  db->duplicates = 0;
  db->generation = 0;
}


static void
erase_predicates(rdf_db *db)
{ int i;

  for(i=0; i<db->predicates.bucket_count; i++)
  { predicate *n, *p = db->predicates.blocks[MSB(i)][i];

    db->predicates.blocks[MSB(i)][i] = NULL;

    for( ; p; p = n )
    { n = p->next;

      free_list(db, &p->subPropertyOf);
      free_list(db, &p->siblings);
      if ( ++p->cloud->deleted == p->cloud->size )
	free_predicate_cloud(db, p->cloud);

      rdf_free(db, p, sizeof(*p));
    }
  }

  db->predicates.count = 0;
}


static int
reset_db(rdf_db *db)
{ int rc;

  db->resetting = TRUE;

  erase_triples(db);
  erase_predicates(db);
  erase_resources(&db->resources);
  erase_graphs(db);
  db->need_update = FALSE;
  db->agenda_created = 0;
  skiplist_destroy(&db->literals);

  rc = (init_resource_db(db, &db->resources) &&
	init_literal_table(db));

  db->resetting = FALSE;

  return rc;
}


/** rdf_reset_db

    Reset the RDF database to its initial state.  Only allowed if there
    are no active queries. This means that if the calling thread has
    open queries this must be considered a permission error.  Otherwise
    we wait until all queries have died.

    TBD: Check queries in other threads!
*/

static foreign_t
rdf_reset_db()
{ rdf_db *db = DB;
  query *q = open_query(db);
  int rc;

  if ( q->depth > 0 )
  { close_query(q);
    return permission_error("reset", "rdf_db", "default",
			    "Active queries");
  }

  rc = reset_db(db);
  close_query(q);

  return rc;
}


		 /*******************************
		 *	       MATCH		*
		 *******************************/


static foreign_t
match_label(term_t how, term_t search, term_t label)
{ atom_t h, f, l;
  int type;

  if ( !PL_get_atom_ex(how, &h) ||
       !PL_get_atom_ex(search, &f) ||
       !PL_get_atom_ex(label, &l) )
    return FALSE;

  if ( h == ATOM_exact )
    type = STR_MATCH_EXACT;
  else if ( h == ATOM_substring )
    type = STR_MATCH_SUBSTRING;
  else if ( h == ATOM_word )
    type = STR_MATCH_WORD;
  else if ( h == ATOM_prefix )
    type = STR_MATCH_PREFIX;
  else if ( h == ATOM_like )
    type = STR_MATCH_LIKE;
  else
    return PL_domain_error("search_method", how);

  return match_atoms(type, f, l);
}


static foreign_t
lang_matches(term_t lang, term_t pattern)
{ atom_t l, p;

  if ( !PL_get_atom_ex(lang, &l) ||
       !PL_get_atom_ex(pattern, &p) )
    return FALSE;

  return atom_lang_matches(l, p);
}




		 /*******************************
		 *	       VERSION		*
		 *******************************/

static foreign_t
rdf_version(term_t v)
{ return PL_unify_integer(v, RDF_VERSION);
}


		 /*******************************
		 *	     REGISTER		*
		 *******************************/

#define MKFUNCTOR(n, a) \
	FUNCTOR_ ## n ## a = PL_new_functor(PL_new_atom(#n), a)
#define NDET PL_FA_NONDETERMINISTIC
#define META PL_FA_TRANSPARENT

install_t
install_rdf_db()
{ int i=0;
  extern install_t install_atom_map(void);

  init_errors();
  register_resource_predicates();

  MKFUNCTOR(literal, 1);
  MKFUNCTOR(triples, 1);
  MKFUNCTOR(triples, 2);
  MKFUNCTOR(resources, 1);
  MKFUNCTOR(predicates, 1);
  MKFUNCTOR(subject, 1);
  MKFUNCTOR(predicate, 1);
  MKFUNCTOR(object, 1);
  MKFUNCTOR(graph, 1);
  MKFUNCTOR(indexed, 16);
  MKFUNCTOR(exact, 1);
  MKFUNCTOR(plain, 1);
  MKFUNCTOR(substring, 1);
  MKFUNCTOR(word, 1);
  MKFUNCTOR(prefix, 1);
  MKFUNCTOR(like, 1);
  MKFUNCTOR(le, 1);
  MKFUNCTOR(between, 2);
  MKFUNCTOR(ge, 1);
  MKFUNCTOR(literal, 2);
  MKFUNCTOR(searched_nodes, 1);
  MKFUNCTOR(duplicates, 1);
  MKFUNCTOR(literals, 1);
  MKFUNCTOR(symmetric, 1);
  MKFUNCTOR(transitive, 1);
  MKFUNCTOR(inverse_of, 1);
  MKFUNCTOR(lang, 2);
  MKFUNCTOR(type, 2);
  MKFUNCTOR(rdf_subject_branch_factor, 1);
  MKFUNCTOR(rdf_object_branch_factor, 1);
  MKFUNCTOR(rdfs_subject_branch_factor, 1);
  MKFUNCTOR(rdfs_object_branch_factor, 1);
  MKFUNCTOR(gc, 2);
  MKFUNCTOR(core, 1);
  MKFUNCTOR(assert, 4);
  MKFUNCTOR(retract, 4);
  MKFUNCTOR(update, 5);
  MKFUNCTOR(new_literal, 1);
  MKFUNCTOR(old_literal, 1);
  MKFUNCTOR(transaction, 2);
  MKFUNCTOR(load, 2);
  MKFUNCTOR(rehash, 1);
  MKFUNCTOR(begin, 1);
  MKFUNCTOR(end, 1);
  MKFUNCTOR(hash_quality, 1);
  MKFUNCTOR(hash, 3);

  FUNCTOR_colon2 = PL_new_functor(PL_new_atom(":"), 2);

  ATOM_user	     = PL_new_atom("user");
  ATOM_exact	     = PL_new_atom("exact");
  ATOM_plain	     = PL_new_atom("plain");
  ATOM_prefix	     = PL_new_atom("prefix");
  ATOM_like	     = PL_new_atom("like");
  ATOM_substring     = PL_new_atom("substring");
  ATOM_word	     = PL_new_atom("word");
  ATOM_subPropertyOf = PL_new_atom(URL_subPropertyOf);
  ATOM_error	     = PL_new_atom("error");
  ATOM_begin	     = PL_new_atom("begin");
  ATOM_end	     = PL_new_atom("end");
  ATOM_infinite	     = PL_new_atom("infinite");

  PRED_call1         = PL_predicate("call", 1, "user");

					/* statistics */
  keys[i++] = FUNCTOR_triples1;
  keys[i++] = FUNCTOR_resources1;
  keys[i++] = FUNCTOR_indexed16;
  keys[i++] = FUNCTOR_hash_quality1;
  keys[i++] = FUNCTOR_predicates1;
  keys[i++] = FUNCTOR_searched_nodes1;
  keys[i++] = FUNCTOR_duplicates1;
  keys[i++] = FUNCTOR_literals1;
  keys[i++] = FUNCTOR_triples2;
  keys[i++] = FUNCTOR_gc2;
  keys[i++] = FUNCTOR_core1;
  keys[i++] = 0;
  assert(i<=16);

  check_index_tables();
					/* see struct triple */
  assert(sizeof(literal) <= sizeof(triple*)*INDEX_TABLES);

					/* setup the database */
  DB = new_db();

  PL_register_foreign("rdf_version",    1, rdf_version,     0);
  PL_register_foreign("rdf_assert",	3, rdf_assert3,	    0);
  PL_register_foreign("rdf_assert",	4, rdf_assert4,	    0);
  PL_register_foreign("rdf_update",	4, rdf_update,      0);
  PL_register_foreign("rdf_update",	5, rdf_update5,     0);
  PL_register_foreign("rdf_retractall",	3, rdf_retractall3, 0);
  PL_register_foreign("rdf_retractall",	4, rdf_retractall4, 0);
  PL_register_foreign("rdf",		3, rdf3,	    NDET);
  PL_register_foreign("rdf",		4, rdf4,	    NDET);
  PL_register_foreign("rdf_has",	4, rdf_has,	    NDET);
  PL_register_foreign("rdf_gc",		0, rdf_gc,	    0);
  PL_register_foreign("rdf_statistics_",1, rdf_statistics,  NDET);
  PL_register_foreign("rdf_generation", 1, rdf_generation,  0);
  PL_register_foreign("rdf_match_label",3, match_label,     0);
  PL_register_foreign("rdf_save_db_",   2, rdf_save_db,     0);
  PL_register_foreign("rdf_load_db_",   3, rdf_load_db,     0);
  PL_register_foreign("rdf_reachable",  3, rdf_reachable3,  NDET);
  PL_register_foreign("rdf_reachable",  5, rdf_reachable5,  NDET);
  PL_register_foreign("rdf_reset_db_",  0, rdf_reset_db,    0);
  PL_register_foreign("rdf_set_predicate",
					2, rdf_set_predicate, 0);
  PL_register_foreign("rdf_predicate_property_",
					2, rdf_predicate_property, NDET);
  PL_register_foreign("rdf_current_predicate",
					1, rdf_current_predicate, NDET);
  PL_register_foreign("rdf_current_literal",
					1, rdf_current_literal, NDET);
  PL_register_foreign("rdf_graph",      1, rdf_graph,       NDET);
  PL_register_foreign("rdf_set_graph_source", 3, rdf_set_graph_source, 0);
  PL_register_foreign("rdf_unset_graph_source", 1, rdf_unset_graph_source, 0);
  PL_register_foreign("rdf_graph_source_", 3, rdf_graph_source, 0);
  PL_register_foreign("rdf_estimate_complexity",
					4, rdf_estimate_complexity, 0);
  PL_register_foreign("rdf_transaction_",2, rdf_transaction, META);
  PL_register_foreign("rdf_active_transactions_",
					1, rdf_active_transactions, 0);
  PL_register_foreign("rdf_monitor_",   2, rdf_monitor,     META);
/*PL_register_foreign("rdf_broadcast_", 2, rdf_broadcast,   0);*/
#ifdef WITH_MD5
  PL_register_foreign("rdf_md5",	2, rdf_md5,	    0);
  PL_register_foreign("rdf_atom_md5",	3, rdf_atom_md5,    0);
#endif

#ifdef O_DEBUG
  PL_register_foreign("rdf_debug",      1, rdf_debug,       0);
  PL_register_foreign("rdf_print_predicate_cloud", 1, rdf_print_predicate_cloud, 0);
#endif
#ifdef O_SECURE
  PL_register_foreign("rdf_dump_literals", 0, dump_literals, 0);
  PL_register_foreign("rdf_check_literals", 0, check_transitivity, 0);
#endif
  PL_register_foreign("lang_matches", 2, lang_matches, 0);

  install_atom_map();
}
