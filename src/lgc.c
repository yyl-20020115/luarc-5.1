/*
** $Id: lgc.c,v 2.38.1.1 2007/12/27 13:02:25 roberto Exp $
** Garbage Collector
** See Copyright Notice in lua.h
*/

#include <string.h>

#define lgc_c
#define LUA_CORE

#include "lua.h"

#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lgc.h"
#include "lmem.h"
#include "lobject.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "ltm.h"


#define GCSTEPSIZE	1024u
#define GCSWEEPMAX	40
#define GCSWEEPCOST	10
#define GCFINALIZECOST	100


#define maskmarks	cast_byte(~(bitmask(BLACKBIT)|WHITEBITS))

#define makewhite(g,x)	\
   ((x)->gch.marked = cast_byte(((x)->gch.marked & maskmarks) | luaC_white(g)))

#define white2gray(x)	reset2bits((x)->gch.marked, WHITE0BIT, WHITE1BIT)
#define black2gray(x)	resetbit((x)->gch.marked, BLACKBIT)

#define stringmark(s)	reset2bits((s)->tsv.marked, WHITE0BIT, WHITE1BIT)


#define isfinalized(u)		testbit((u)->marked, FINALIZEDBIT)
#define markfinalized(u)	l_setbit((u)->marked, FINALIZEDBIT)


#define KEYWEAK         bitmask(KEYWEAKBIT)
#define VALUEWEAK       bitmask(VALUEWEAKBIT)



#define markvalue(g,o) { checkconsistency(o); \
  if (iscollectable(o) && iswhite(gcvalue(o))) reallymarkobject(g,gcvalue(o)); }

#define markobject(g,t) { if (iswhite(obj2gco(t))) \
		reallymarkobject(g, obj2gco(t)); }


#define setthreshold(g)  (g->GCthreshold = (g->estimate/100) * g->gcpause)


static void removeentry (Node *n) {
  lua_assert(ttisnil(gval(n)));
  if (iscollectable(gkey(n)))
    setttype(gkey(n), LUA_TDEADKEY);  /* dead key; remove it */
}


static void reallymarkobject (global_State *g, GCObject *o) {
  lua_assert(iswhite(o) && !isdead(g, o));
  white2gray(o);
  switch (o->gch.tt) {
    case LUA_TSTRING: {
      return;
    }
    case LUA_TUSERDATA: { /* udata has no gc ptr, can't add to gc list */
      Table *mt = gco2u(o)->metatable;
      gray2black(o);  /* udata are never gray */
      if (mt) markobject(g, mt);
      markobject(g, gco2u(o)->env);
      return;
    }
    case LUA_TUPVAL: {
      /* open upval is in g->uvhead list, closed in rootgc list */
      UpVal *uv = gco2uv(o);
      markvalue(g, uv->v);
      if (uv->v == &uv->u.value)  /* closed? */
        gray2black(o);  /* open upvalues are never black */
      return;
    }
#if LUA_REFCOUNT
    case LUA_TFUNCTION:
    case LUA_TTABLE:
    case LUA_TTHREAD:
    case LUA_TPROTO: {
      o->tgch.gcnext = g->gray;
      o->tgch.gcprev = NULL;
      if (g->gray) g->gray->tgch.gcprev = o;
      g->gray = o;
    }
#else
    case LUA_TFUNCTION: {
      gco2cl(o)->c.gclist = g->gray;
      g->gray = o;
      break;
    }
    case LUA_TTABLE: {
      gco2h(o)->gclist = g->gray;
      g->gray = o;
      break;
    }
    case LUA_TTHREAD: {
      gco2th(o)->gclist = g->gray;
      g->gray = o;
      break;
    }
    case LUA_TPROTO: {
      gco2p(o)->gclist = g->gray;
      g->gray = o;
      break;
    }
#endif
    default: lua_assert(0);
  }
}


static void marktmu (global_State *g) {
  GCObject *u = g->tmudata;
  if (u) {
    do {
      u = u->gch.next;
      makewhite(g, u);  /* may be marked, if left from previous GC */
      reallymarkobject(g, u);
    } while (u != g->tmudata);
  }
}


/* move `dead' udata that need finalization to list `tmudata' */
size_t luaC_separateudata (lua_State *L, int all) {
  global_State *g = G(L);
  size_t deadmem = 0;
  GCObject **p = &g->mainthread->next;
  GCObject *curr;
  while ((curr = *p) != NULL) {
    if (!(iswhite(curr) || all) || isfinalized(gco2u(curr)))
      p = &curr->gch.next;  /* don't bother with them */
    else if (fasttm(L, gco2u(curr)->metatable, TM_GC) == NULL) {
      markfinalized(gco2u(curr));  /* don't need finalization */
      p = &curr->gch.next;
    }
    else {  /* must call its gc method */
      deadmem += sizeudata(gco2u(curr));
      markfinalized(gco2u(curr));
#if LUA_REFCOUNT
      if (curr->tgch.next)
	curr->tgch.next->tgch.prev = curr->tgch.prev;
      /* udata with FINALIZEDBIT set will not be freed by refcount, so
      ** no need to assign curr->tgch.prev here.
      */
#endif
      *p = curr->gch.next;
      /* link `curr' at the end of `tmudata' list */
      if (g->tmudata == NULL)  /* list is empty? */
        g->tmudata = curr->gch.next = curr;  /* creates a circular list */
      else {
        curr->gch.next = g->tmudata->gch.next;
        g->tmudata->gch.next = curr;
        g->tmudata = curr;
      }
    }
  }
  return deadmem;
}


static int traversetable (global_State *g, Table *h) {
  int i;
  int weakkey = 0;
  int weakvalue = 0;
  const TValue *mode;
  if (h->metatable)
    markobject(g, h->metatable);
  mode = gfasttm(g, h->metatable, TM_MODE);
  if (mode && ttisstring(mode)) {  /* is there a weak mode? */
    weakkey = (strchr(svalue(mode), 'k') != NULL);
    weakvalue = (strchr(svalue(mode), 'v') != NULL);
    if (weakkey || weakvalue) {  /* is really weak? */
      h->marked &= ~(KEYWEAK | VALUEWEAK);  /* clear bits */
      h->marked |= cast_byte((weakkey << KEYWEAKBIT) |
                             (weakvalue << VALUEWEAKBIT));
#if LUA_REFCOUNT
      h->gcnext = g->weak;
      lua_assert(h->gcprev == NULL); /* h is former gray list head */
      if (g->weak) g->weak->tgch.gcprev = obj2gco(h);
#else
      h->gclist = g->weak;  /* must be cleared after GC, ... */
#endif
      g->weak = obj2gco(h);  /* ... so put in the appropriate list */
    }
  }
  if (weakkey && weakvalue) return 1;
  if (!weakvalue) {
    i = h->sizearray;
    while (i--)
      markvalue(g, &h->array[i]);
  }
  i = sizenode(h);
  while (i--) {
    Node *n = gnode(h, i);
    lua_assert(ttype(gkey(n)) != LUA_TDEADKEY || ttisnil(gval(n)));
    if (ttisnil(gval(n)))
      removeentry(n);  /* remove empty entries */
    else {
      lua_assert(!ttisnil(gkey(n)));
      if (!weakkey) markvalue(g, gkey(n));
      if (!weakvalue) markvalue(g, gval(n));
    }
  }
  return weakkey || weakvalue;
}


/*
** All marks are conditional because a GC may happen while the
** prototype is still being created
*/
static void traverseproto (global_State *g, Proto *f) {
  int i;
  if (f->source) stringmark(f->source);
  for (i=0; i<f->sizek; i++)  /* mark literals */
    markvalue(g, &f->k[i]);
  for (i=0; i<f->sizeupvalues; i++) {  /* mark upvalue names */
    if (f->upvalues[i])
      stringmark(f->upvalues[i]);
  }
  for (i=0; i<f->sizep; i++) {  /* mark nested protos */
    if (f->p[i])
      markobject(g, f->p[i]);
  }
  for (i=0; i<f->sizelocvars; i++) {  /* mark local-variable names */
    if (f->locvars[i].varname)
      stringmark(f->locvars[i].varname);
  }
}



static void traverseclosure (global_State *g, Closure *cl) {
  markobject(g, cl->c.env);
  if (cl->c.isC) {
    int i;
    for (i=0; i<cl->c.nupvalues; i++)  /* mark its upvalues */
      markvalue(g, &cl->c.upvalue[i]);
  }
  else {
    int i;
    lua_assert(cl->l.nupvalues == cl->l.p->nups);
    markobject(g, cl->l.p);
    for (i=0; i<cl->l.nupvalues; i++)  /* mark its upvalues */
      markobject(g, cl->l.upvals[i]);
  }
}


static void checkstacksizes (lua_State *L, StkId max) {
  int ci_used = cast_int(L->ci - L->base_ci);  /* number of `ci' in use */
  int s_used = cast_int(max - L->stack);  /* part of stack in use */
  if (L->size_ci > LUAI_MAXCALLS)  /* handling overflow? */
    return;  /* do not touch the stacks */
  if (4*ci_used < L->size_ci && 2*BASIC_CI_SIZE < L->size_ci)
    luaD_reallocCI(L, L->size_ci/2);  /* still big enough... */
  condhardstacktests(luaD_reallocCI(L, ci_used + 1));
  if (4*s_used < L->stacksize &&
      2*(BASIC_STACK_SIZE+EXTRA_STACK) < L->stacksize)
    luaD_reallocstack(L, L->stacksize/2);  /* still big enough... */
  condhardstacktests(luaD_reallocstack(L, s_used));
}


static void traversestack (global_State *g, lua_State *l) {
  StkId o, lim;
  CallInfo *ci;
  markvalue(g, gt(l));
  lim = l->top;
  for (ci = l->base_ci; ci <= l->ci; ci++) {
    lua_assert(ci->top <= l->stack_last);
    if (lim < ci->top) lim = ci->top;
  }
  for (o = l->stack; o < l->top; o++)
    markvalue(g, o);
  for (; o <= lim; o++) {
#if LUA_REFCOUNT
    setnilvalue(l, o);
#else /* !LUA_REFCOUNT */
    setnilvalue(o);
#endif
  }
  checkstacksizes(l, lim);
}


/*
** traverse one gray object, turning it to black.
** Returns `quantity' traversed.
*/
static l_mem propagatemark (global_State *g) {
  GCObject *o = g->gray;
  lua_assert(isgray(o));
  gray2black(o);
  switch (o->gch.tt) {
    case LUA_TTABLE: {
      Table *h = gco2h(o);
#if LUA_REFCOUNT
      if (h->gcnext) h->gcnext->tgch.gcprev = NULL;
      g->gray = h->gcnext;
#else
      g->gray = h->gclist;
#endif
      if (traversetable(g, h))  /* table is weak? */
        black2gray(o);  /* keep it gray */
      return sizeof(Table) + sizeof(TValue) * h->sizearray +
                             sizeof(Node) * sizenode(h);
    }
    case LUA_TFUNCTION: {
      Closure *cl = gco2cl(o);
#if LUA_REFCOUNT
      if (cl->c.gcnext) cl->c.gcnext->tgch.gcprev = NULL;
      g->gray = cl->c.gcnext;
#else
      g->gray = cl->c.gclist;
#endif
      traverseclosure(g, cl);
      return (cl->c.isC) ? sizeCclosure(cl->c.nupvalues) :
                           sizeLclosure(cl->l.nupvalues);
    }
    case LUA_TTHREAD: {
      lua_State *th = gco2th(o);
#if LUA_REFCOUNT
      if (th->gcnext) th->gcnext->tgch.gcprev = NULL;
      g->gray = th->gcnext;
      if (g->grayagain) g->grayagain->tgch.gcprev = o;
      th->gcnext = g->grayagain;
      g->grayagain = o;
#else
      g->gray = th->gclist;
      th->gclist = g->grayagain;
      g->grayagain = o;
#endif
      black2gray(o);
      traversestack(g, th);
      return sizeof(lua_State) + sizeof(TValue) * th->stacksize +
                                 sizeof(CallInfo) * th->size_ci;
    }
    case LUA_TPROTO: {
      Proto *p = gco2p(o);
#if LUA_REFCOUNT
      if (p->gcnext) p->gcnext->tgch.gcprev = NULL;
      g->gray = p->gcnext;
#else
      g->gray = p->gclist;
#endif
      traverseproto(g, p);
      return sizeof(Proto) + sizeof(Instruction) * p->sizecode +
                             sizeof(Proto *) * p->sizep +
                             sizeof(TValue) * p->sizek + 
                             sizeof(int) * p->sizelineinfo +
                             sizeof(LocVar) * p->sizelocvars +
                             sizeof(TString *) * p->sizeupvalues;
    }
    default: lua_assert(0); return 0;
  }
}


static size_t propagateall (global_State *g) {
  size_t m = 0;
  while (g->gray) m += propagatemark(g);
  return m;
}


/*
** The next function tells whether a key or value can be cleared from
** a weak table. Non-collectable objects are never removed from weak
** tables. Strings behave as `values', so are never removed too. for
** other objects: if really collected, cannot keep them; for userdata
** being finalized, keep them in keys, but not in values
*/
static int iscleared (const TValue *o, int iskey) {
  if (!iscollectable(o)) return 0;
  if (ttisstring(o)) {
    stringmark(rawtsvalue(o));  /* strings are `values', so are never weak */
    return 0;
  }
  return iswhite(gcvalue(o)) ||
    (ttisuserdata(o) && (!iskey && isfinalized(uvalue(o))));
}


/*
** clear collected entries from weaktables
*/
#if LUA_REFCOUNT
static void cleartable (lua_State *L, GCObject *l) {
#else
static void cleartable (GCObject *l) {
#endif
  while (l) {
    Table *h = gco2h(l);
    int i = h->sizearray;
    lua_assert(testbit(h->marked, VALUEWEAKBIT) ||
               testbit(h->marked, KEYWEAKBIT));
    if (testbit(h->marked, VALUEWEAKBIT)) {
      while (i--) {
        TValue *o = &h->array[i];
        if (iscleared(o, 0)) { /* value was collected? */
#if LUA_REFCOUNT
          setnilvalue(L, o);  /* remove value */
#else /* !LUA_REFCOUNT */
          setnilvalue(o);  /* remove value */
#endif
	}
      }
    }
    i = sizenode(h);
    while (i--) {
      Node *n = gnode(h, i);
      if (!ttisnil(gval(n)) &&  /* non-empty entry? */
          (iscleared(key2tval(n), 1) || iscleared(gval(n), 0))) {
#if LUA_REFCOUNT
        setnilvalue(L, gval(n));  /* remove value ... */
#else
        setnilvalue(gval(n));  /* remove value ... */
#endif
        removeentry(n);  /* remove entry from table */
      }
    }
#if LUA_REFCOUNT
    l = h->gcnext;
#else
    l = h->gclist;
#endif
  }
}


static void freeobj (lua_State *L, GCObject *o) {
  switch (o->gch.tt) {
    case LUA_TPROTO: luaF_freeproto(L, gco2p(o)); break;
    case LUA_TFUNCTION: luaF_freeclosure(L, gco2cl(o)); break;
    case LUA_TUPVAL: luaF_freeupval(L, gco2uv(o)); break;
    case LUA_TTABLE: luaH_free(L, gco2h(o)); break;
    case LUA_TTHREAD: {
      lua_assert(gco2th(o) != L && gco2th(o) != G(L)->mainthread);
      luaE_freethread(L, gco2th(o));
      break;
    }
    case LUA_TSTRING: {
      G(L)->strt.nuse--;
      luaM_freemem(L, o, sizestring(gco2ts(o)));
      break;
    }
    case LUA_TUSERDATA: {
      luaM_freemem(L, o, sizeudata(gco2u(o)));
      break;
    }
    default: lua_assert(0);
  }
}



#define sweepwholelist(L,p)	sweeplist(L,p,MAX_LUMEM)


static GCObject **sweeplist (lua_State *L, GCObject **p, lu_mem count) {
  GCObject *curr;
  global_State *g = G(L);
  int deadmask = otherwhite(g);
  while ((curr = *p) != NULL && count-- > 0) {
    if (curr->gch.tt == LUA_TTHREAD)  /* sweep open upvalues of each thread */
      sweepwholelist(L, &gco2th(curr)->openupval);
    if ((curr->gch.marked ^ WHITEBITS) & deadmask) {  /* not dead? */
      lua_assert(!isdead(g, curr) || testbit(curr->gch.marked, FIXEDBIT));
      makewhite(g, curr);  /* make it white (for next cycle) */
      p = &curr->gch.next;
    }
    else {  /* must erase `curr' */
      lua_assert(isdead(g, curr) || deadmask == bitmask(SFIXEDBIT));
#if LUA_REFCOUNT
      if (curr->tgch.next)
       	curr->tgch.next->tgch.prev = curr->tgch.prev;
#endif
      *p = curr->gch.next;
      if (curr == g->rootgc)  /* is the first element of the list? */
        g->rootgc = curr->gch.next;  /* adjust first */
      freeobj(L, curr);
    }
  }
  return p;
}


static void checkSizes (lua_State *L) {
  global_State *g = G(L);
  /* check size of string hash */
  if (g->strt.nuse < cast(lu_int32, g->strt.size/4) &&
      g->strt.size > MINSTRTABSIZE*2)
    luaS_resize(L, g->strt.size/2);  /* table is too big */
  /* check size of buffer */
  if (luaZ_sizebuffer(&g->buff) > LUA_MINBUFFER*2) {  /* buffer too big? */
    size_t newsize = luaZ_sizebuffer(&g->buff) / 2;
    luaZ_resizebuffer(L, &g->buff, newsize);
  }
}


static void GCTM (lua_State *L) {
  global_State *g = G(L);
  GCObject *o = g->tmudata->gch.next;  /* get first element */
  Udata *udata = rawgco2u(o);
  const TValue *tm;
  /* remove udata from `tmudata' */
  if (o == g->tmudata)  /* last element? */
    g->tmudata = NULL;
  else
    g->tmudata->gch.next = udata->uv.next;
  udata->uv.next = g->mainthread->next;  /* return it to `root' list */
#if LUA_REFCOUNT
  udata->uv.prev = obj2gco(g->mainthread);
  if (g->mainthread->next)
    g->mainthread->next->gch.prev = o;
#endif
  g->mainthread->next = o;
  makewhite(g, o);
  tm = fasttm(L, udata->uv.metatable, TM_GC);
  if (tm != NULL) {
    lu_byte oldah = L->allowhook;
    lu_mem oldt = g->GCthreshold;
    L->allowhook = 0;  /* stop debug hooks during GC tag method */
    g->GCthreshold = 2*g->totalbytes;  /* avoid GC steps */
    setobj2s(L, L->top, tm);
    setuvalue(L, L->top+1, udata);
    L->top += 2;
    luaD_call(L, L->top - 2, 0);
    L->allowhook = oldah;  /* restore hooks */
    g->GCthreshold = oldt;  /* restore threshold */
  }
}


/*
** Call all GC tag methods
*/
void luaC_callGCTM (lua_State *L) {
  while (G(L)->tmudata)
    GCTM(L);
}


void luaC_freeall (lua_State *L) {
  global_State *g = G(L);
  int i;
  g->currentwhite = WHITEBITS | bitmask(SFIXEDBIT);  /* mask to collect all elements */
  sweepwholelist(L, &g->rootgc);
  for (i = 0; i < g->strt.size; i++)  /* free all string lists */
    sweepwholelist(L, &g->strt.hash[i]);
}


static void markmt (global_State *g) {
  int i;
  for (i=0; i<NUM_TAGS; i++)
    if (g->mt[i]) markobject(g, g->mt[i]);
}


/* mark root set */
static void markroot (lua_State *L) {
  global_State *g = G(L);
  g->gray = NULL;
  g->grayagain = NULL;
  g->weak = NULL;
  markobject(g, g->mainthread);
  /* make global table be traversed before main stack */
  markvalue(g, gt(g->mainthread));
  markvalue(g, registry(L));
  markmt(g);
  g->gcstate = GCSpropagate;
}


static void remarkupvals (global_State *g) {
  UpVal *uv;
  for (uv = g->uvhead.u.l.next; uv != &g->uvhead; uv = uv->u.l.next) {
    lua_assert(uv->u.l.next->u.l.prev == uv && uv->u.l.prev->u.l.next == uv);
    if (isgray(obj2gco(uv)))
      markvalue(g, uv->v);
  }
}


static void atomic (lua_State *L) {
  global_State *g = G(L);
  size_t udsize;  /* total size of userdata to be finalized */
  /* remark occasional upvalues of (maybe) dead threads */
  remarkupvals(g);
  /* traverse objects cautch by write barrier and by 'remarkupvals' */
  propagateall(g);
  /* remark weak tables */
  g->gray = g->weak;
  g->weak = NULL;
  lua_assert(!iswhite(obj2gco(g->mainthread)));
  markobject(g, L);  /* mark running thread */
  markmt(g);  /* mark basic metatables (again) */
  propagateall(g);
  /* remark gray again */
  g->gray = g->grayagain;
  g->grayagain = NULL;
  propagateall(g);
  udsize = luaC_separateudata(L, 0);  /* separate userdata to be finalized */
  marktmu(g);  /* mark `preserved' userdata */
  udsize += propagateall(g);  /* remark, to propagate `preserveness' */
#if LUA_REFCOUNT
  cleartable(L, g->weak);  /* remove collected objects from weak tables */
#else
  cleartable(g->weak);  /* remove collected objects from weak tables */
#endif
  /* flip current white */
  g->currentwhite = cast_byte(otherwhite(g));
  g->sweepstrgc = 0;
  g->sweepgc = &g->rootgc;
  g->gcstate = GCSsweepstring;
  g->estimate = g->totalbytes - udsize;  /* first estimate */
}


static l_mem singlestep (lua_State *L) {
  global_State *g = G(L);
  /*lua_checkmemory(L);*/
  switch (g->gcstate) {
    case GCSpause: {
      markroot(L);  /* start a new collection */
      return 0;
    }
    case GCSpropagate: {
      if (g->gray)
        return propagatemark(g);
      else {  /* no more `gray' objects */
        atomic(L);  /* finish mark phase */
        return 0;
      }
    }
    case GCSsweepstring: {
      lu_mem old = g->totalbytes;
      sweepwholelist(L, &g->strt.hash[g->sweepstrgc++]);
      if (g->sweepstrgc >= g->strt.size)  /* nothing more to sweep? */
        g->gcstate = GCSsweep;  /* end sweep-string phase */
      lua_assert(old >= g->totalbytes);
      g->estimate -= old - g->totalbytes;
      return GCSWEEPCOST;
    }
    case GCSsweep: {
      lu_mem old = g->totalbytes;
      g->sweepgc = sweeplist(L, g->sweepgc, GCSWEEPMAX);
      if (*g->sweepgc == NULL) {  /* nothing more to sweep? */
        checkSizes(L);
        g->gcstate = GCSfinalize;  /* end sweep phase */
      }
      lua_assert(old >= g->totalbytes);
      g->estimate -= old - g->totalbytes;
      return GCSWEEPMAX*GCSWEEPCOST;
    }
    case GCSfinalize: {
      if (g->tmudata) {
        GCTM(L);
        if (g->estimate > GCFINALIZECOST)
          g->estimate -= GCFINALIZECOST;
        return GCFINALIZECOST;
      }
      else {
        g->gcstate = GCSpause;  /* end collection */
        g->gcdept = 0;
        return 0;
      }
    }
    default: lua_assert(0); return 0;
  }
}


void luaC_step (lua_State *L) {
  global_State *g = G(L);
  l_mem lim = (GCSTEPSIZE/100) * g->gcstepmul;
  if (lim == 0)
    lim = (MAX_LUMEM-1)/2;  /* no limit */
  g->gcdept += g->totalbytes - g->GCthreshold;
  do {
    lim -= singlestep(L);
    if (g->gcstate == GCSpause)
      break;
  } while (lim > 0);
  if (g->gcstate != GCSpause) {
    if (g->gcdept < GCSTEPSIZE)
      g->GCthreshold = g->totalbytes + GCSTEPSIZE;  /* - lim/g->gcstepmul;*/
    else {
      g->gcdept -= GCSTEPSIZE;
      g->GCthreshold = g->totalbytes;
    }
  }
  else {
    lua_assert(g->totalbytes >= g->estimate);
    setthreshold(g);
  }
}


void luaC_fullgc (lua_State *L) {
  global_State *g = G(L);
  if (g->gcstate <= GCSpropagate) {
    /* reset sweep marks to sweep all elements (returning them to white) */
    g->sweepstrgc = 0;
    g->sweepgc = &g->rootgc;
    /* reset other collector lists */
    g->gray = NULL;
    g->grayagain = NULL;
    g->weak = NULL;
    g->gcstate = GCSsweepstring;
  }
  lua_assert(g->gcstate != GCSpause && g->gcstate != GCSpropagate);
  /* finish any pending sweep phase */
  while (g->gcstate != GCSfinalize) {
    lua_assert(g->gcstate == GCSsweepstring || g->gcstate == GCSsweep);
    singlestep(L);
  }
  markroot(L);
  while (g->gcstate != GCSpause) {
    singlestep(L);
  }
  setthreshold(g);
}


void luaC_barrierf (lua_State *L, GCObject *o, GCObject *v) {
  global_State *g = G(L);
  lua_assert(isblack(o) && iswhite(v) && !isdead(g, v) && !isdead(g, o));
  lua_assert(g->gcstate != GCSfinalize && g->gcstate != GCSpause);
  lua_assert(ttype(&o->gch) != LUA_TTABLE);
  /* must keep invariant? */
  if (g->gcstate == GCSpropagate)
    reallymarkobject(g, v);  /* restore invariant */
  else  /* don't mind */
    makewhite(g, o);  /* mark as white just to avoid other barriers */
}


void luaC_barrierback (lua_State *L, Table *t) {
  global_State *g = G(L);
  GCObject *o = obj2gco(t);
  lua_assert(isblack(o) && !isdead(g, o));
  lua_assert(g->gcstate != GCSfinalize && g->gcstate != GCSpause);
  black2gray(o);  /* make table gray (again) */
#if LUA_REFCOUNT
  t->gcnext = g->grayagain;
  t->gcprev = NULL;
  if (g->grayagain)
    g->grayagain->tgch.gcprev = o;
  g->grayagain = o;
#else
  t->gclist = g->grayagain;
  g->grayagain = o;
#endif
}


void luaC_link (lua_State *L, GCObject *o, lu_byte tt) {
  global_State *g = G(L);
#if LUA_REFCOUNT
  o->gch.next = g->rootgc;
  o->gch.prev = NULL;
  if (g->rootgc)
    g->rootgc->gch.prev = o;
  g->rootgc = o;
#else
  o->gch.next = g->rootgc;
  g->rootgc = o;
#endif
  o->gch.marked = luaC_white(g);
  o->gch.tt = tt;
}


void luaC_linkupval (lua_State *L, UpVal *uv) {
  global_State *g = G(L);
  GCObject *o = obj2gco(uv);
#if LUA_REFCOUNT
  o->gch.next = g->rootgc;
  o->gch.prev = NULL;
  if (g->rootgc)
    g->rootgc->gch.prev = o;
  g->rootgc = o;
#else
  o->gch.next = g->rootgc;  /* link upvalue into `rootgc' list */
  g->rootgc = o;
#endif
  if (isgray(o)) { 
    if (g->gcstate == GCSpropagate) {
      gray2black(o);  /* closed upvalues need barrier */
      luaC_barrier(L, uv, uv->v);
    }
    else {  /* sweep phase: sweep it (turning it into white) */
      makewhite(g, o);
      lua_assert(g->gcstate != GCSfinalize && g->gcstate != GCSpause);
    }
  }
}


#if LUA_REFCOUNT
static void luarc_freestring (lua_State *L, TString *s) {
  GCObject *obj = cast(GCObject *, s);
  stringtable *st = &G(L)->strt;
  if (obj->gch.prev != NULL)
    obj->gch.prev->gch.next = obj->gch.next;
  else {
    lua_assert(st->hash[lmod(s->hash, st->size)] == obj);
    st->hash[lmod(s->tsv.hash, st->size)] = obj->gch.next;
  }
  if (obj->gch.next) obj->gch.next->gch.prev = obj->gch.prev;
  st->nuse --;
  luaM_freemem(L, obj, sizestring(gco2ts(obj)));
}


static void luarc_freeudata (lua_State *L, Udata *ud) {
  GCObject *obj = cast(GCObject *, ud);
  if (isfinalized(gco2u(obj))) return; /* do not free finalized udata */

  /* FINALIZEDBIT is not set, udata must be in rootgc list */

  markfinalized(gco2u(obj));

  /* Unlink userdata from rootgc */
  if (obj->gch.next) obj->gch.next->gch.prev = obj->gch.prev;
  lua_assert(obj->gch.prev);
  obj->gch.prev->gch.next = obj->gch.next;

  /* Execute GC tagmethod */
  if (fasttm(L, ud->uv.metatable, TM_GC) != NULL) {
    /* put userdata to the head(not tail) of g->tmudata */
    if (G(L)->tmudata == NULL) /* list is empty? */
      G(L)->tmudata = obj->gch.next = obj;
    else {
      obj->gch.next = G(L)->tmudata->gch.next;
      G(L)->tmudata->gch.next = obj;
      /* do not change g->tmudata to keep obj as the list head */
    }

    GCTM(L); /* execute the gc tagmethod */

    /* obj is put back to rootgc by GCTM, unlink it */
    if (obj->gch.next) obj->gch.next->gch.prev = obj->gch.prev;
    lua_assert(obj->gch.prev == g->mainthread);
    obj->gch.prev->gch.next = obj->gch.next;
  }

  /* Adjust g->sweepgc in sweep state */
  if (G(L)->gcstate == GCSsweep && G(L)->sweepgc == &ud->uv.next) {
    if (ud->uv.prev) G(L)->sweepgc = &ud->uv.prev->gch.next;
    else G(L)->sweepgc = &G(L)->rootgc;
  }

  if (ud->uv.metatable)
    luarc_subtableref(L, ud->uv.metatable);
  luarc_subtableref(L, ud->uv.env);

  luaM_freemem(L, obj, sizeudata(&ud->uv));
}


static void luarc_freeproto (lua_State *L, Proto *f) {
  int i;
  GCObject *obj = cast(GCObject *, f);

  /* Proto is in rootgc list, possibly in gray list */

  /* Unlink from rootgc list */
  if (f->next != NULL) f->next->gch.prev = f->prev;

  if (f->prev != NULL) f->prev->gch.next = f->next;
  else {
    lua_assert(G(L)->rootgc == obj);
    G(L)->rootgc = f->next;
  }

  /* Adjust g->gray list in mark state */
  if (G(L)->gcstate == GCSpropagate && isgray(obj)) {
    if (f->gcnext) f->gcnext->tgch.gcprev = f->gcprev;

    if (f->gcprev) f->gcprev->tgch.gcnext = f->gcnext;
    else {
      lua_assert(G(L)->gray == obj);
      G(L)->gray = f->gcnext;
    }
  }

  /* Adjust g->sweepgc in GC sweep state */
  if (G(L)->gcstate == GCSsweep && G(L)->sweepgc == &f->next) {
    if (f->prev != NULL) G(L)->sweepgc = &f->prev->gch.next;
    else G(L)->sweepgc = &G(L)->rootgc;
  }

  /* Reduce refcount of members */
  if (f->source) luarc_substringref(L, f->source);
  for (i=0; i<f->sizek; i++)
    luarc_subref(L, &f->k[i]);
  for (i=0; i<f->sizeupvalues; i++) {
    if (f->upvalues[i])
      luarc_substringref(L, f->upvalues[i]);
  }
  for (i=0; i<f->sizep; i++) {
    if (f->p[i])
      luarc_subprotoref(L, f->p[i]);
  }
  for (i=0; i<f->sizelocvars; i++) {
    if (f->locvars[i].varname)
      luarc_substringref(L, f->locvars[i].varname);
  }

  luaF_freeproto(L, f);
}


static void luarc_freeclosure (lua_State *L, Closure *cl) {
  int i;
  GCObject *obj = cast(GCObject *, cl);

  /* Closure is in rootgc list, possibly in gray list */

  /* Unlink from rootgc list */
  if (cl->c.next != NULL) cl->c.next->gch.prev = cl->c.prev;
  
  if (cl->c.prev != NULL) cl->c.prev->gch.next = cl->c.next;
  else {
    lua_assert(G(L)->rootgc == obj);
    G(L)->rootgc = cl->c.next;
  }

  /* Unlink from g->gray list in mark state */
  if (G(L)->gcstate == GCSpropagate && isgray(obj)) {
    if (cl->c.gcnext) cl->c.gcnext->tgch.gcprev = cl->c.gcprev;

    if (cl->c.gcprev) cl->c.gcprev->tgch.gcnext = cl->c.gcnext;
    else {
      lua_assert(G(L)->gray == obj);
      G(L)->gray = cl->c.gcnext;
    }
  }

  /* Adjust g->sweepgc in GC sweep state */
  if (G(L)->gcstate == GCSsweep && G(L)->sweepgc == &cl->c.next) {
    if (cl->c.prev != NULL) G(L)->sweepgc = &cl->c.prev->gch.next;
    else G(L)->sweepgc = &G(L)->rootgc;
  }

  /* Reduce refcount of members */
  luarc_subtableref(L, cl->c.env);
  if (cl->c.isC) {
    for (i=0; i<cl->c.nupvalues; i++)
      luarc_subref(L, &cl->c.upvalue[i]);
  }
  else {
    luarc_subprotoref(L, cl->l.p);
    for (i=0; i<cl->l.nupvalues; i++)
      luarc_subupvalref(L, cl->l.upvals[i]);
  }

  luaF_freeclosure(L, cl);
}


static void luarc_freeupval (lua_State *L, UpVal *uv) {

  /* Open upval is in L->openupval list, closed upval is in g->rootgc list
  ** open upval may be used immediately, even when its refcount is 0.
  ** So only closed upval is freed.
  */
  if (uv->v != &uv->u.value) return; /* Do not free open upval */

  /* Closed upval is in the rootgc list only */

  /* Unlink uv from rootgc */
  if (uv->next) uv->next->gch.prev = uv->prev;

  if (uv->prev) uv->prev->gch.next = uv->next;
  else {
    lua_assert(G(L)->rootgc == obj2gco(uv));
    G(L)->rootgc = uv->next;
  }

  /* Adjust g->sweepgc in GC sweep state */
  if (G(L)->gcstate == GCSsweep && G(L)->sweepgc == &uv->next) {
    if (uv->prev != NULL) G(L)->sweepgc = &uv->prev->gch.next;
    else G(L)->sweepgc = &G(L)->rootgc;
  }

  /* Reduce refcount of member */
  luarc_subref(L, uv->v);

  luaF_freeupval(L, uv);
}


static void luarc_freetable (lua_State *L, Table *t) {
  int i;
  GCObject *obj = cast(GCObject *, t);

  /* Table is in g->rootgc list, and possibly in
  ** g->gray/g->grayagain/g->weak list
  */

  /* Unlink t from rootgc list */
  if (t->next) t->next->gch.prev = t->prev;

  if (t->prev) t->prev->gch.next = t->next;
  else {
    lua_assert(G(L)->rootgc == obj);
    G(L)->rootgc = t->next;
  }

  /* Unlink t from gray/grayagain/weak list in mark state */
  if (G(L)->gcstate == GCSpropagate && isgray(obj)) {
    if (t->gcnext) t->gcnext->tgch.gcprev = t->gcprev;

    if (t->gcprev) t->gcprev->tgch.gcnext = t->gcnext;
    else {
      if (G(L)->gray == obj) G(L)->gray = t->gcnext;
      else if (G(L)->grayagain == obj) G(L)->grayagain = t->gcnext;
      else if (G(L)->weak == obj) G(L)->weak = t->gcnext;
      else lua_assert(0);
    }
  }

  /* Adjust g->sweepgc in GC sweep state */
  if (G(L)->gcstate == GCSsweep && G(L)->sweepgc == &t->next) {
    if (t->prev) G(L)->sweepgc = &t->prev->gch.next;
    else G(L)->sweepgc = &G(L)->rootgc;
  }

  /* Reduce refcount of keys/values, even this is a weak table */
  if (t->metatable) luarc_subtableref(L, t->metatable);
  i = t->sizearray;
  while (i--) luarc_subref(L, &t->array[i]);
  i = sizenode(t);
  while (i--) {
    Node *n = gnode(t, i);
    lua_assert(ttype(gkey(n)) != LUA_TDEADKEY || ttisnil(gval(n)));
    if (ttisnil(gval(n))) removeentry(n);
    else {
      lua_assert(!ttisnil(gkey(n)));
      luarc_subref(L, key2tval(n));
      luarc_subref(L, gval(n));
    }
  }
  
  luaH_free(L, t);
}


static void luarc_freethread (lua_State *L, lua_State *th) {
  StkId o, lim;
  CallInfo *ci;
  GCObject *obj = cast(GCObject *, th);

  /* Thread is in rootgc list, and possibly gray/grayagain list */

  /* Unlink th from rootgc list */
  if (th->next) th->next->gch.prev = th->prev;

  if (th->prev) th->prev->gch.next = th->next;
  else {
    lua_assert(G(L)->rootgc == obj);
    G(L)->rootgc = th->next;
  }

  /* Unlink th from gray/grayagain list in mark state */
  if (G(L)->gcstate == GCSpropagate && isgray(obj)) {
    if (th->gcnext) th->gcnext->tgch.gcprev = th->gcprev;

    if (th->gcprev) th->gcprev->tgch.gcnext = th->gcnext;
    else {
      if (G(L)->gray == obj) G(L)->gray = th->gcnext;
      else if (G(L)->grayagain == obj) G(L)->grayagain = th->gcnext;
      else lua_assert(0);
    }
  }

  /* Adjust g->sweepgc in GC sweep state */
  if (G(L)->gcstate == GCSsweep && G(L)->sweepgc == &th->next) {
    if (th->prev) G(L)->sweepgc = &th->prev->gch.next;
    else G(L)->sweepgc = &G(L)->rootgc;
  }

  /* Reduce refcount of members */
  luarc_subvalref(L, gt(th));
  lim = th->top;
  for (ci = th->base_ci; ci <= th->ci; ci++) {
    lua_assert(ci->top <= th->stack_last);
    if (lim < ci->top) lim = ci->top;
  }
  for (o = th->stack; o <= lim; o++)
    luarc_subref(L, o);

  lua_assert(th != L && th != G(L)->mainthread);
  luaE_freethread(L, th);
}


void luaC_freeobj (lua_State *L, GCObject *obj) {
  switch(obj->gch.tt) {
    case LUA_TSTRING:
      luarc_freestring(L, rawgco2ts(obj));
      break;
    case LUA_TUSERDATA:
      luarc_freeudata(L, rawgco2u(obj));
      break;
    case LUA_TPROTO:
      luarc_freeproto(L, gco2p(obj));
      break;
    case LUA_TFUNCTION:
      luarc_freeclosure(L, gco2cl(obj));
      break;
    case LUA_TUPVAL:
      luarc_freeupval(L, gco2uv(obj));
      break;
    case LUA_TTABLE:
      luarc_freetable(L, gco2h(obj));
      break;
    case LUA_TTHREAD:
      luarc_freethread(L, gco2th(obj));
      break;
    default: lua_assert(0);
  }
}

#endif /* LUA_REFCOUNT */
