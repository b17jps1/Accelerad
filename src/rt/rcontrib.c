#ifndef lint
static const char RCSid[] = "$Id: rcontrib.c,v 2.30 2017/04/11 17:52:40 greg Exp $";
#endif
/*
 * Accumulate ray contributions for a set of materials
 * Initialization and calculation routines
 */

#include "copyright.h"

#include "rcontrib.h"
#include "otypes.h"
#include "source.h"

char	*shm_boundary = NULL;		/* boundary of shared memory */

CUBE	thescene;			/* our scene */
OBJECT	nsceneobjs;			/* number of objects in our scene */

int	dimlist[MAXDIM];		/* sampling dimensions */
int	ndims = 0;			/* number of sampling dimensions */
int	samplendx = 0;			/* index for this sample */

static void	trace_contrib(RAY *r);	/* our trace callback */
void	(*trace)() = trace_contrib;

int	do_irrad = 0;			/* compute irradiance? */

int	rand_samp = 1;			/* pure Monte Carlo sampling? */

double	dstrsrc = 0.9;			/* square source distribution */
double	shadthresh = .03;		/* shadow threshold */
double	shadcert = .75;			/* shadow certainty */
int	directrelay = 3;		/* number of source relays */
int	vspretest = 512;		/* virtual source pretest density */
int	directvis = 1;			/* sources visible? */
double	srcsizerat = .2;		/* maximum ratio source size/dist. */

COLOR	cextinction = BLKCOLOR;		/* global extinction coefficient */
COLOR	salbedo = BLKCOLOR;		/* global scattering albedo */
double	seccg = 0.;			/* global scattering eccentricity */
double	ssampdist = 0.;			/* scatter sampling distance */

double	specthresh = .02;		/* specular sampling threshold */
double	specjitter = 1.;		/* specular sampling jitter */

int	backvis = 1;			/* back face visibility */

int	maxdepth = -10;			/* maximum recursion depth */
double	minweight = 2e-3;		/* minimum ray weight */

char	*ambfile = NULL;		/* ambient file name */
COLOR	ambval = BLKCOLOR;		/* ambient value */
int	ambvwt = 0;			/* initial weight for ambient value */
double	ambacc = 0;			/* ambient accuracy */
int	ambres = 256;			/* ambient resolution */
int	ambdiv = 350;			/* ambient divisions */
int	ambssamp = 0;			/* ambient super-samples */
int	ambounce = 1;			/* ambient bounces */
char	*amblist[AMBLLEN+1];		/* ambient include/exclude list */
int	ambincl = -1;			/* include == 1, exclude == 0 */

int	account;			/* current accumulation count */
RNUMBER	raysleft;			/* number of rays left to trace */
long	waitflush;			/* how long until next flush */

RNUMBER	lastray = 0;			/* last ray number sent */
RNUMBER	lastdone = 0;			/* last ray output */

static void mcfree(void *p) { epfree((*(MODCONT *)p).binv); free(p); }

LUTAB	modconttab = LU_SINIT(NULL,mcfree);	/* modifier lookup table */

#ifdef ACCELERAD
#define EXPECTED_RAY_COUNT	32

/* from optix_radiance.c */
extern void contribOptix(const size_t width, const size_t height, const unsigned int imm_irrad, const unsigned int lim_dist, const unsigned int contrib, const unsigned int bins, const double alarm, double* rays, LUTAB *modifiers);

static unsigned int total_bins = 0;	/* total number of contribution bins */

double  ralrm = 0.0;				/* seconds between reports */

/* TODO This shouldn't be necessary, but the variable must exist in optix_util.c */
double	pctdone = 0.0;			/* percentage done */
void report(int dummy) {}		/* report progress */
#endif

/************************** INITIALIZATION ROUTINES ***********************/

char *
formstr(				/* return format identifier */
	int  f
)
{
	switch (f) {
	case 'a': return("ascii");
	case 'f': return("float");
	case 'd': return("double");
	case 'c': return(COLRFMT);
	}
	return("unknown");
}


/* Add modifier to our list to track */
MODCONT *
addmodifier(char *modn, char *outf, char *prms, char *binv, int bincnt)
{
	LUENT	*lep = lu_find(&modconttab,modn);
	MODCONT	*mp;
	EPNODE	*ebinv;
	int	i;
	
	if (lep->data != NULL) {
		sprintf(errmsg, "duplicate modifier '%s'", modn);
		error(USER, errmsg);
	}
	if (nmods >= MAXMODLIST) {
		sprintf(errmsg, "too many modifiers (%d limit)", MAXMODLIST);
		error(INTERNAL, errmsg);
	}
	if (!strcmp(modn, VOIDID)) {
		sprintf(errmsg, "cannot track '%s' modifier", VOIDID);
		error(USER, errmsg);
	}
	modname[nmods++] = modn;	/* XXX assumes static string */
	lep->key = modn;		/* XXX assumes static string */
	if (binv == NULL)
		binv = "0";		/* use single bin if unspecified */
	ebinv = eparse(binv);
	if (ebinv->type == NUM) {	/* check value if constant */
		bincnt = (int)(evalue(ebinv) + 1.5);
		if (bincnt != 1) {
			sprintf(errmsg, "illegal non-zero constant for bin (%s)",
					binv);
			error(USER, errmsg);
		}
	} else if (bincnt <= 0) {
		sprintf(errmsg,
			"unspecified or illegal bin count for modifier '%s'",
				modn);
		error(USER, errmsg);
	}
					/* initialize results holder */
	mp = (MODCONT *)malloc(sizeof(MODCONT)+sizeof(DCOLOR)*(bincnt-1));
	if (mp == NULL)
		error(SYSTEM, "out of memory in addmodifier");
	mp->outspec = outf;		/* XXX assumes static string */
	mp->modname = modn;		/* XXX assumes static string */
	mp->params = prms;		/* XXX assumes static string */
	mp->binv = ebinv;
	mp->bin0 = 0;
	mp->nbins = bincnt;
#ifdef ACCELERAD
	mp->start_bin = total_bins;
	total_bins += bincnt;
#endif
	memset(mp->cbin, 0, sizeof(DCOLOR)*bincnt);
					/* figure out starting bin */
	while (!getostream(mp->outspec, mp->modname, mp->bin0, 1))
		mp->bin0++;
					/* allocate other output streams */
	for (i = 0; ++i < mp->nbins; )
		getostream(mp->outspec, mp->modname, mp->bin0+i, 1);
	lep->data = (char *)mp;
	return(mp);
}


/* Add modifiers from a file list */
void
addmodfile(char *fname, char *outf, char *prms, char *binv, int bincnt)
{
	char	*mname[MAXMODLIST];
	int	i;
					/* find the file & store strings */
	i = wordfile(mname, MAXMODLIST, getpath(fname, getrlibpath(), R_OK));
	if (i < 0) {
		sprintf(errmsg, "cannot find modifier file '%s'", fname);
		error(SYSTEM, errmsg);
	}
	if (i >= MAXMODLIST-1) {
		sprintf(errmsg, "too many modifiers (%d limit) in file '%s'",
				MAXMODLIST-1, fname);
		error(INTERNAL, errmsg);
	}
	for (i = 0; mname[i]; i++)	/* add each one */
		addmodifier(mname[i], outf, prms, binv, bincnt);
}


void
quit(			/* quit program */
	int  code
)
{
	if (nchild > 0)		/* close children if any */
		end_children(code != 0);
	exit(code);
}


/* Initialize our process(es) */
static void
rcinit(void)
{
	int	i;

	if (nproc > MAXPROCESS)
		sprintf(errmsg, "too many processes requested -- reducing to %d",
				nproc = MAXPROCESS);
	if (nproc > 1) {
#ifdef ACCELERAD
		if (use_optix) /* Don't allow multiple processes to access the graphics card. */
			error(USER, "multiprocessing incompatible with GPU implementation");
#endif
		preload_objs();		/* preload auxiliary data */
					/* set shared memory boundary */
		shm_boundary = strcpy((char *)malloc(16), "SHM_BOUNDARY");
	}
	for (i = 0; i < nsources; i++)	/* tracing to sources as well */
		source[i].sflags |= SFOLLOW;
	if (yres > 0) {			/* set up flushing & ray counts */
		if (xres > 0)
			raysleft = (RNUMBER)xres*yres;
		else
			raysleft = yres;
	} else
		raysleft = 0;
	if ((account = accumulate) > 1)
		raysleft *= accumulate;
	waitflush = (yres > 0) & (xres > 1) ? 0 : xres;

	if (nproc > 1 && in_rchild())	/* forked child? */
		return;			/* return to main processing loop */

#ifdef ACCELERAD
	if (!use_optix) /* Recovery shouldn't be useful for parallel computation. */
#endif
	if (recover) {			/* recover previous output? */
		if (accumulate <= 0)
			reload_output();
		else
			recover_output();
	}
	if (nproc == 1)			/* single process? */
		return;
					/* else run appropriate controller */
	if (accumulate <= 0)
		feeder_loop();
	else
		parental_loop();
	quit(0);			/* parent musn't return! */
}

/************************** MAIN CALCULATION PROCESS ***********************/

/* Our trace call to sum contributions */
static void
trace_contrib(RAY *r)
{
	MODCONT	*mp;
	double	bval;
	int	bn;
	RREAL	contr[3];

	if (r->ro == NULL || r->ro->omod == OVOID)
		return;
						/* shadow ray not on source? */
	if (r->rsrc >= 0 && source[r->rsrc].so != r->ro)
		return;

	mp = (MODCONT *)lu_find(&modconttab,objptr(r->ro->omod)->oname)->data;

	if (mp == NULL)				/* not in our list? */
		return;

	worldfunc(RCCONTEXT, r);		/* else set context */
	set_eparams((char *)mp->params);
	if ((bval = evalue(mp->binv)) <= -.5)	/* and get bin number */
		return;				/* silently ignore negatives */
	if ((bn = (int)(bval + .5)) >= mp->nbins) {
		sprintf(errmsg, "bad bin number (%d ignored)", bn);
		error(WARNING, errmsg);
		return;
	}
	raycontrib(contr, r, PRIMARY);		/* compute coefficient */
	if (contrib)
		multcolor(contr, r->rcol);	/* -> contribution */
	addcolor(mp->cbin[bn], contr);
}


/* Evaluate irradiance contributions */
static void
eval_irrad(FVECT org, FVECT dir)
{
	RAY	thisray;

	VSUM(thisray.rorg, org, dir, 1.1e-4);
	thisray.rdir[0] = -dir[0];
	thisray.rdir[1] = -dir[1];
	thisray.rdir[2] = -dir[2];
	thisray.rmax = 0.0;
	rayorigin(&thisray, PRIMARY, NULL, NULL);
					/* pretend we hit surface */
	thisray.rt = thisray.rot = 1e-5;
	thisray.rod = 1.0;
	VCOPY(thisray.ron, dir);
	VSUM(thisray.rop, org, dir, 1e-4);
	samplendx++;			/* compute result */
	(*ofun[Lamb.otype].funp)(&Lamb, &thisray);
}


/* Evaluate radiance contributions */
static void
eval_rad(FVECT org, FVECT dir, double dmax)
{
	RAY	thisray;
					/* set up ray */
	VCOPY(thisray.rorg, org);
	VCOPY(thisray.rdir, dir);
	thisray.rmax = dmax;
	rayorigin(&thisray, PRIMARY, NULL, NULL);
	samplendx++;			/* call ray evaluation */
	rayvalue(&thisray);
}


/* Accumulate and/or output ray contributions (child or only process) */
#ifdef ACCELERAD
void
#else
static void
#endif
done_contrib(void)
{
	MODCONT	*mp;
	int	i;

	if (account <= 0 || --account)
		return;			/* not time yet */

	for (i = 0; i < nmods; i++) {	/* output records & clear */
		mp = (MODCONT *)lu_find(&modconttab,modname[i])->data;
		mod_output(mp);
		memset(mp->cbin, 0, sizeof(DCOLOR)*mp->nbins);
	}
	end_record();			/* end lines & flush if time */

	account = accumulate;		/* reset accumulation counter */
}


/* Principal calculation loop (called by main) */
void
rcontrib(void)
{
	static int	ignore_warning_given = 0;
	FVECT		orig, direc;
	double		d;
#ifdef ACCELERAD
	size_t width, current_ray, total_rays;
	RREAL *ray_cache;
#endif
					/* initialize (& fork more of us) */
	rcinit();
					/* load rays from stdin & process */
#ifdef getc_unlocked
	flockfile(stdin);		/* avoid mutex overhead */
#endif
#ifdef ACCELERAD
	if (use_optix) {
		/* Populate the set of rays to trace */
		total_rays = raysleft ? raysleft : EXPECTED_RAY_COUNT;
		ray_cache = (RREAL *)malloc(6 * sizeof(RREAL) * total_rays);
		if (ray_cache == NULL)
			error(SYSTEM, "out of memory in rcontrib");
		current_ray = 0u;

		/* load rays from stdin & process */
		while (getvec(orig) == 0 && getvec(direc) == 0) {
			/* resize array if necessary (should only happen when vcount == 0) */
			if (current_ray == total_rays) {
				total_rays *= 2;
				ray_cache = (RREAL *)realloc(ray_cache, 6 * sizeof(RREAL) * total_rays);
				if (ray_cache == NULL)
					error(SYSTEM, "out of memory in rcontrib");
			}
			VCOPY(ray_cache + (6 * current_ray), orig);
			VCOPY(ray_cache + (6 * current_ray + 3), direc);
			current_ray++;
		}

		total_rays = current_ray;
		if (raysleft)
			raysleft -= (RNUMBER)total_rays;
		width = (yres > 0 && xres > 0) ? xres : 1;
		if (yres > 0 && accumulate > 1)
			width *= accumulate;
		contribOptix(width, yres > 0 ? yres : total_rays, imm_irrad, lim_dist, contrib, total_bins, ralrm, ray_cache, &modconttab);
		free(ray_cache);
	}
	else
#endif
	while (getvec(orig) == 0 && getvec(direc) == 0) {
		d = normalize(direc);
		if (nchild != -1 && (d == 0.0) & (accumulate == 0)) {
			if (!ignore_warning_given++)
				error(WARNING,
				"dummy ray(s) ignored during accumulation\n");
			continue;
		}
		if (lastray+1 < lastray)
			lastray = lastdone = 0;
		++lastray;
		if (d == 0.0) {				/* zero ==> flush */
			if ((yres <= 0) | (xres <= 1))
				waitflush = 1;		/* flush after */
			if (nchild == -1)
				account = 1;
		} else if (imm_irrad) {			/* else compute */
			eval_irrad(orig, direc);
		} else {
			eval_rad(orig, direc, lim_dist ? d : 0.0);
		}
		done_contrib();		/* accumulate/output */
		++lastdone;
		if (raysleft && !--raysleft)
			break;		/* preemptive EOI */
	}
	if (nchild != -1 && (accumulate <= 0) | (account < accumulate)) {
		if (account < accumulate) {
			error(WARNING, "partial accumulation in final record");
			accumulate -= account;
		}
		account = 1;		/* output accumulated totals */
		done_contrib();
	}
	lu_done(&ofiletab);		/* close output files */
	if (raysleft)
		error(USER, "unexpected EOF on input");
}
