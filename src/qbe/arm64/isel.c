#include "all.h"

enum Imm {
	Iother,
	Iplo12,
	Iphi12,
	Iplo24,
	Inlo12,
	Inhi12,
	Inlo24
};

static enum Imm
imm(Con *c, int k, int64_t *pn)
{
	int64_t n;
	int i;

	if (c->type != CBits)
		return Iother;
	n = c->bits.i;
	if (k == Kw)
		n = (int32_t)n;
	i = Iplo12;
	if (n < 0) {
		i = Inlo12;
		n = -(uint64_t)n;
	}
	*pn = n;
	if ((n & 0x000fff) == n)
		return i;
	if ((n & 0xfff000) == n)
		return i + 1;
	if ((n & 0xffffff) == n)
		return i + 2;
	return Iother;
}

int
arm64_logimm(uint64_t x, int k)
{
	uint64_t n;

	if (k == Kw)
		x = (x & 0xffffffff) | x << 32;
	if (x & 1)
		x = ~x;
	if (x == 0)
		return 0;
	if (x == 0xaaaaaaaaaaaaaaaa)
		return 1;
	n = x & 0xf;
	if (0x1111111111111111 * n == x)
		goto Check;
	n = x & 0xff;
	if (0x0101010101010101 * n == x)
		goto Check;
	n = x & 0xffff;
	if (0x0001000100010001 * n == x)
		goto Check;
	n = x & 0xffffffff;
	if (0x0000000100000001 * n == x)
		goto Check;
	n = x;
Check:
	return (n & (n + (n & -n))) == 0;
}

/* per-block cache of stack-slot addresses (see fixarg) */
static Ref *slotaddr;
static int nslotaddr;

static void
fixarg(Ref *pr, int k, int phi, Fn *fn)
{
	char buf[32];
	Con *c, cc;
	Ref r0, r1, r2, r3;
	int s, n;

	r0 = *pr;
	switch (rtype(r0)) {
	case RCon:
		c = &fn->con[r0.val];
		if (T.apple
		&& c->type == CAddr
		&& (c->sym.type & SThr)) {
			r1 = newtmp("isel", Kl, fn);
			*pr = r1;
			if (c->bits.i) {
				r2 = newtmp("isel", Kl, fn);
				cc = (Con){.type = CBits};
				cc.bits.i = c->bits.i;
				r3 = newcon(&cc, fn);
				emit(Oadd, Kl, r1, r2, r3);
				r1 = r2;
			}
			emit(Ocopy, Kl, r1, TMP(R0), R);
			r1 = newtmp("isel", Kl, fn);
			r2 = newtmp("isel", Kl, fn);
			emit(Ocall, 0, R, r1, CALL(33));
			emit(Ocopy, Kl, TMP(R0), r2, R);
			emit(Oload, Kl, r1, r2, R);
			cc = *c;
			cc.bits.i = 0;
			r3 = newcon(&cc, fn);
			emit(Ocopy, Kl, r2, r3, R);
			break;
		}
		if (KBASE(k) == 0 && phi)
			return;
		r1 = newtmp("isel", k, fn);
		if (KBASE(k) == 0) {
			emit(Ocopy, k, r1, r0, R);
		} else {
			n = stashbits(c->bits.i, KWIDE(k) ? 8 : 4);
			vgrow(&fn->con, ++fn->ncon);
			c = &fn->con[fn->ncon-1];
			sprintf(buf, "\"%sfp%d\"", T.asloc, n);
			*c = (Con){.type = CAddr};
			c->sym.id = intern(buf);
			r2 = newtmp("isel", Kl, fn);
			emit(Oload, k, r1, r2, R);
			emit(Ocopy, Kl, r2, CON(c-fn->con), R);
		}
		*pr = r1;
		break;
	case RTmp:
		s = fn->tmp[r0.val].slot;
		if (s == -1)
			break;
		/* simple(v0.6): one address per stack slot per block. Stock QBE
		 * emitted a fresh `add xN, x29, #off` at every single use, so a
		 * loop touching a[j] and a[j+1] recomputed the array's base
		 * twice. The definitions are flushed at the top of the block
		 * (see arm64_isel), where they dominate every use. */
		if (slotaddr && s < nslotaddr) {
			if (req(slotaddr[s], R))
				slotaddr[s] = newtmp("isel", Kl, fn);
			*pr = slotaddr[s];
			break;
		}
		r1 = newtmp("isel", Kl, fn);
		emit(Oaddr, Kl, r1, SLOT(s), R);
		*pr = r1;
		break;
	}
}

static int
selcmp(Ref arg[2], int k, Fn *fn)
{
	Ref r, *iarg;
	Con *c;
	int swap, cmp, fix;
	int64_t n;

	if (KBASE(k) == 1) {
		emit(Oafcmp, k, R, arg[0], arg[1]);
		iarg = curi->arg;
		fixarg(&iarg[0], k, 0, fn);
		fixarg(&iarg[1], k, 0, fn);
		return 0;
	}
	swap = rtype(arg[0]) == RCon;
	if (swap) {
		r = arg[1];
		arg[1] = arg[0];
		arg[0] = r;
	}
	fix = 1;
	cmp = Oacmp;
	r = arg[1];
	if (rtype(r) == RCon) {
		c = &fn->con[r.val];
		switch (imm(c, k, &n)) {
		default:
			break;
		case Iplo12:
		case Iphi12:
			fix = 0;
			break;
		case Inlo12:
		case Inhi12:
			cmp = Oacmn;
			r = getcon(n, fn);
			fix = 0;
			break;
		}
	}
	emit(cmp, k, R, arg[0], r);
	iarg = curi->arg;
	fixarg(&iarg[0], k, 0, fn);
	if (fix)
		fixarg(&iarg[1], k, 0, fn);
	return swap;
}

/* NOTE (v0.6, retried v0.95): ARM64 register-offset addressing
 * (`ldr d, [base, idx]`) folds `base + index` into the load/store. The
 * amd64 address-numbering machinery (anumber/amatch) was ported here and
 * *works* for register-based indices, but stack-array bases need the
 * slot's frame displacement materialised into a register (arm64 has no
 * base+index+disp form), and doing that in fixarg breaks the chuse()
 * use-count accounting seladdr set up — regalloc then coalesces base and
 * index into one register (`str d0, [x2, x2]`, a miscompile). Getting the
 * slot-materialisation liveness right is the remaining work; reverted for
 * now. amd64 sidesteps it because x86 folds base+index+disp directly, so
 * a slot base never needs a separate register. The payoff is real
 * (matmul 0.71x C on x86_64 vs 2.50x here), so this stays a live TODO. */

/* simple(v0.55): can this constant serve directly as an ARM64 immediate
 * operand, saving the `mov` that stock QBE emitted for every constant?
 * Conservative: only forms the emitter is guaranteed to print correctly.
 * Logical ops are restricted to 12-bit values because the emitter's
 * "lsl #12" path has no logical-immediate equivalent. */
static int
immarg(int op, int k, Ref r, Fn *fn)
{
	Con *c;
	int64_t n;

	if (rtype(r) != RCon || KBASE(k) != 0)
		return 0;
	c = &fn->con[r.val];
	if (c->type != CBits)
		return 0;
	n = c->bits.i;
	if (k == Kw)
		n = (int32_t)n;
	switch (op) {
	case Oadd:
	case Osub:
		return n >= 0 && ((n & 0xfff) == n || (n & 0xfff000) == n);
	case Oand:
	case Oor:
	case Oxor:
	case Oatst:
		return n > 0 && (n & 0xfff) == n && arm64_logimm(n, k);
	case Oshl:
	case Oshr:
	case Osar:
		return n >= 0 && n < (KWIDE(k) ? 64 : 32);
	}
	return 0;
}

static int
callable(Ref r, Fn *fn)
{
	Con *c;

	if (rtype(r) == RTmp)
		return 1;
	if (rtype(r) == RCon) {
		c = &fn->con[r.val];
		if (c->type == CAddr)
		if (c->bits.i == 0)
			return 1;
	}
	return 0;
}

static void
sel(Ins i, Fn *fn)
{
	Ref *iarg;
	Ins *i0;
	int ck, cc;

	if (INRANGE(i.op, Oalloc, Oalloc1)) {
		i0 = curi - 1;
		salloc(i.to, i.arg[0], fn);
		fixarg(&i0->arg[0], Kl, 0, fn);
		return;
	}
	if (iscmp(i.op, &ck, &cc)) {
		emit(Oflag, i.cls, i.to, R, R);
		i0 = curi;
		if (selcmp(i.arg, ck, fn))
			i0->op += cmpop(cc);
		else
			i0->op += cc;
		return;
	}
	if (i.op == Ocall)
	if (callable(i.arg[0], fn)) {
		emiti(i);
		return;
	}
	if (i.op != Onop) {
		emiti(i);
		iarg = curi->arg; /* fixarg() can change curi */
		fixarg(&iarg[0], argcls(&i, 0), 0, fn);
		/* simple(v0.55): keep eligible constants as immediates */
		if (!immarg(i.op, i.cls, iarg[1], fn))
			fixarg(&iarg[1], argcls(&i, 1), 0, fn);
	}
}

/* simple(v0.55): find the instruction defining r, scanning back only
 * while the flags stay untouched (on ARM64 only comparisons and calls
 * disturb them). Returns 0 if r is not reachable that way. */
static Ins *
findflagdef(Blk *b, Ins *from, Ref r, Fn *fn)
{
	Ins *i;
	int ck, cc;

	(void)fn;
	for (i=from; b->ins<i;) {
		--i;
		if (i->op == Onop)
			continue;
		if (req(i->to, r))
			return i;
		if (iscmp(i->op, &ck, &cc) || i->op == Ocall)
			return 0;
	}
	return 0;
}

/* simple(v0.55): fold `(x & imm) == 0` into ARM64's `tst`, which sets
 * flags without producing a value — saving both the `and`'s destination
 * register and the separate compare. The `and` is rewritten in place;
 * the backwards walk selects it later. */
static int
seltst(Blk *b, Ins *ir, int ck, int cc, Fn *fn)
{
	Ins *ai;
	Ref r, t;

	if (KBASE(ck) != 0)
		return 0;
	if (cc != Cieq && cc != Cine)
		return 0;
	if (!req(ir->arg[1], CON_Z))
		return 0;
	r = ir->arg[0];
	if (rtype(r) != RTmp || fn->tmp[r.val].nuse != 1)
		return 0;
	ai = findflagdef(b, ir, r, fn);
	if (!ai || ai->op != Oand)
		return 0;
	if (rtype(ai->arg[0]) != RTmp) {
		if (rtype(ai->arg[1]) != RTmp)
			return 0;
		t = ai->arg[0];
		ai->arg[0] = ai->arg[1];
		ai->arg[1] = t;
	}
	ai->op = Oatst;
	ai->to = R;
	return 1;
}

/* simple(v0.55): lower an Osel0/Osel1 group to csel.
 * ARM64's csel is 3-operand, so unlike amd64's cmov there is no
 * constraint tying the destination to a source register: we compare the
 * condition against zero and emit one csel per selected value. Emission
 * runs backwards, so the compare below lands before the csels. */
static Ins *
selsel(Fn *fn, Blk *b, Ins *i)
{
	Ref r;
	Ins *isel0, *isel1, *ir;
	int ck, cc;

	assert(i->op == Osel1);
	for (isel0=i; b->ins<isel0; isel0--) {
		if (isel0->op == Osel0)
			break;
		assert(isel0->op == Osel1);
	}
	assert(isel0->op == Osel0);
	r = isel0->arg[0];
	assert(rtype(r) == RTmp);
	/* If the condition is produced by a comparison sitting immediately
	 * before the group (its only use), fuse: reuse that comparison's
	 * flags for the csel instead of materializing a 0/1 with cset and
	 * comparing it against zero again. Adjacency keeps this sound —
	 * nothing in between can clobber the flags. */
	ir = 0;
	for (isel1=isel0; b->ins<isel1;) {
		--isel1;
		if (isel1->op == Onop)
			continue;
		if (req(isel1->to, r)) {
			ir = isel1;
			break;
		}
		/* Unlike amd64, ARM64 ALU instructions do not write flags —
		 * only comparisons (via cmp/cmn) and calls disturb them, so
		 * the scan may pass over everything else. */
		if (iscmp(isel1->op, &ck, &cc) || isel1->op == Ocall)
			break;
	}
	if (ir && fn->tmp[r.val].nuse == 1 && iscmp(ir->op, &ck, &cc)) {
		int tst = seltst(b, ir, ck, cc, fn);
		/* selcmp swaps when the first operand is a constant; predict
		 * that here so the selects get the right condition before
		 * they are emitted (emission runs backwards) */
		if (!tst && KBASE(ck) == 0 && rtype(ir->arg[0]) == RCon)
			cc = cmpop(cc);
		for (isel1=i; isel0<isel1; --isel1) {
			isel1->op = Oxsel + cc;
			sel(*isel1, fn);
		}
		if (!tst)
			selcmp(ir->arg, ck, fn);
		*ir = (Ins){.op = Onop};
	} else {
		for (isel1=i; isel0<isel1; --isel1) {
			isel1->op = Oxsel + Cine;
			sel(*isel1, fn);
		}
		selcmp((Ref[]){r, CON_Z}, Kw, fn);
	}
	*isel0 = (Ins){.op = Onop};
	return isel0;
}

static void
seljmp(Blk *b, Fn *fn)
{
	Ref r;
	Ins *i, *ir;
	int ck, cc, use;

	if (b->jmp.type == Jret0
	|| b->jmp.type == Jjmp
	|| b->jmp.type == Jhlt)
		return;
	assert(b->jmp.type == Jjnz);
	r = b->jmp.arg;
	use = -1;
	b->jmp.arg = R;
	ir = 0;
	i = &b->ins[b->nins];
	while (i > b->ins)
		if (req((--i)->to, r)) {
			use = fn->tmp[r.val].nuse;
			ir = i;
			break;
		}
	if (ir && use == 1
	&& iscmp(ir->op, &ck, &cc)) {
		if (seltst(b, ir, ck, cc, fn))
			; /* flags come from tst */
		else if (selcmp(ir->arg, ck, fn))
			cc = cmpop(cc);
		b->jmp.type = Jjf + cc;
		*ir = (Ins){.op = Onop};
	}
	else {
		selcmp((Ref[]){r, CON_Z}, Kw, fn);
		b->jmp.type = Jjfine;
	}
}

void
arm64_isel(Fn *fn)
{
	Blk *b, **sb;
	Ins *i;
	Phi *p;
	uint n, al;
	int64_t sz;

	/* assign slots to fast allocs */
	b = fn->start;
	/* specific to NAlign == 3 */ /* or change n=4 and sz /= 4 below */
	for (al=Oalloc, n=4; al<=Oalloc1; al++, n*=2)
		for (i=b->ins; i<&b->ins[b->nins]; i++)
			if (i->op == al) {
				if (rtype(i->arg[0]) != RCon)
					break;
				sz = fn->con[i->arg[0].val].bits.i;
				if (sz < 0 || sz >= INT_MAX-15)
					err("invalid alloc size %"PRId64, sz);
				sz = (sz + n-1) & -n;
				sz /= 4;
				fn->tmp[i->to.val].slot = fn->slot;
				fn->slot += sz;
				*i = (Ins){.op = Onop};
			}

	nslotaddr = fn->slot;
	slotaddr = nslotaddr ? emalloc(nslotaddr * sizeof slotaddr[0]) : 0;

	for (b=fn->start; b; b=b->link) {
		curi = &insb[NIns];
		for (n=0; n<(uint)nslotaddr; n++)
			slotaddr[n] = R;
		for (sb=(Blk*[3]){b->s1, b->s2, 0}; *sb; sb++)
			for (p=(*sb)->phi; p; p=p->link) {
				for (n=0; p->blk[n] != b; n++)
					assert(n+1 < p->narg);
				fixarg(&p->arg[n], p->cls, 1, fn);
			}
		seljmp(b, fn);
		for (i=&b->ins[b->nins]; i!=b->ins;) {
			--i;
			assert(i->op != Osel0);
			if (i->op == Osel1)
				i = selsel(fn, b, i);
			else
				sel(*i, fn);
		}
		/* emission runs backwards, so flushing the slot addresses last
		 * places them first in the block — before every use */
		for (n=0; n<(uint)nslotaddr; n++)
			if (!req(slotaddr[n], R))
				emit(Oaddr, Kl, slotaddr[n], SLOT(n), R);
		idup(b, curi, &insb[NIns]-curi);
	}
	free(slotaddr);
	slotaddr = 0;

	if (debug['I']) {
		fprintf(stderr, "\n> After instruction selection:\n");
		printfn(fn, stderr);
	}
}
