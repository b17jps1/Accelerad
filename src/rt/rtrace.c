#ifndef lint
static const char	RCSid[] = "$Id: rtrace.c,v 2.88 2020/03/12 17:19:18 greg Exp $";
#endif
/*
 *  rtrace.c - program and variables for individual ray tracing.
 */

#include "copyright.h"

/*
 *  Input is in the form:
 *
 *	xorg	yorg	zorg	xdir	ydir	zdir
 *
 *  The direction need not be normalized.  Output is flexible.
 *  If the direction vector is (0,0,0), then the output is flushed.
 *  All values default to ascii representation of real
 *  numbers.  Binary representations can be selected
 *  with '-ff' for float or '-fd' for double.  By default,
 *  radiance is computed.  The '-i' or '-I' options indicate that
 *  irradiance values are desired.
 */

#include  <time.h>

#include  "platform.h"
#include  "ray.h"
#include  "ambient.h"
#include  "source.h"
#include  "otypes.h"
#include  "otspecial.h"
#include  "resolu.h"
#include  "random.h"

extern int  inform;			/* input format */
extern int  outform;			/* output format */
extern char  *outvals;			/* output values */

extern int  imm_irrad;			/* compute immediate irradiance? */
extern int  lim_dist;			/* limit distance? */

extern char  *tralist[];		/* list of modifers to trace (or no) */
extern int  traincl;			/* include == 1, exclude == 0 */

extern int  hresolu;			/* horizontal resolution */
extern int  vresolu;			/* vertical resolution */

static int  castonly = 0;

#ifndef  MAXTSET
#define	 MAXTSET	8191		/* maximum number in trace set */
#endif
OBJECT	traset[MAXTSET+1]={0};		/* trace include/exclude set */

#ifdef DAYSIM
static void daysimOutput(RAY* r);
#endif

static RAY  thisray;			/* for our convenience */

typedef void putf_t(RREAL *v, int n);
static putf_t puta, putd, putf, putrgbe;

typedef void oputf_t(RAY *r);
static oputf_t  oputo, oputd, oputv, oputV, oputl, oputL, oputc, oputp,
		oputr, oputR, oputx, oputX, oputn, oputN, oputs,
		oputw, oputW, oputm, oputM, oputtilde;

static void setoutput(char *vs);
extern void tranotify(OBJECT obj);
static void bogusray(void);
static void raycast(RAY *r);
static void rayirrad(RAY *r);
static void rtcompute(FVECT org, FVECT dir, double dmax);
static int printvals(RAY *r);
static int getvec(FVECT vec, int fmt, FILE *fp);
static void tabin(RAY *r);
static void ourtrace(RAY *r);

static oputf_t *ray_out[32], *every_out[32];
static putf_t *putreal;

#ifdef ACCELERAD
#define EXPECTED_RAY_COUNT	32

/* from optix_rtrace.c */
extern void computeOptix(const size_t width, const size_t height, const unsigned int imm_irrad, RAY* rays);
#endif


void
quit(			/* quit program */
	int  code
)
{
	if (ray_pnprocs > 0)	/* close children if any */
		ray_pclose(0);		
#ifndef  NON_POSIX
	else if (!ray_pnprocs) {
		headclean();	/* delete header file */
		pfclean();	/* clean up persist files */
	}
#endif
	exit(code);
}


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


extern void
rtrace(				/* trace rays from file */
	char  *fname,
	int  nproc
)
{
	unsigned long  vcount = (hresolu > 1) ? (unsigned long)hresolu*vresolu
					      : (unsigned long)vresolu;
	long  nextflush = (!vresolu | (hresolu <= 1)) * hresolu;
	int  something2flush = 0;
	FILE  *fp;
	double	d;
#ifdef DAYSIM
	int j = 0;
#endif
	FVECT  orig, direc;
#ifdef ACCELERAD
	size_t current_ray, total_rays;
	RAY* ray_cache;
#endif
					/* set up input */
	if (fname == NULL)
		fp = stdin;
	else if ((fp = fopen(fname, "r")) == NULL) {
		sprintf(errmsg, "cannot open input file \"%s\"", fname);
		error(SYSTEM, errmsg);
	}
	if (inform != 'a')
		SET_FILE_BINARY(fp);
					/* set up output */
	setoutput(outvals);
	if (imm_irrad)
		castonly = 0;
	else if (castonly)
		nproc = 1;		/* don't bother multiprocessing */
	if ((nextflush > 0) & (nproc > nextflush)) {
		error(WARNING, "reducing number of processes to match flush interval");
		nproc = nextflush;
	}
	switch (outform) {
	case 'a': putreal = puta; break;
	case 'f': putreal = putf; break;
	case 'd': putreal = putd; break;
	case 'c': 
		if (outvals[0] && (outvals[1] || !strchr("vrx", outvals[0])))
			error(USER, "color format only with -ov, -or, -ox");
		putreal = putrgbe; break;
	default:
		error(CONSISTENCY, "botched output format");
	}
#ifdef ACCELERAD
	if (use_optix) {
		/* Populate the set of rays to trace */
		total_rays = vcount ? vcount : EXPECTED_RAY_COUNT;
		ray_cache = (RAY *)malloc(sizeof(RAY) * total_rays);
		if (ray_cache == NULL)
			error(SYSTEM, "out of memory in rtrace");
		current_ray = 0u;
	} else /* OptiX kernel can only be launched from a single thread. */
#endif
	if (nproc > 1) {		/* start multiprocessing */
		ray_popen(nproc);
		ray_fifo_out = printvals;
	}
	if (hresolu > 0) {
		if (vresolu > 0)
			fprtresolu(hresolu, vresolu, stdout);
		else
			fflush(stdout);
	}
					/* process file */
	while (getvec(orig, inform, fp) == 0 &&
			getvec(direc, inform, fp) == 0) {

		d = normalize(direc);
		if (d == 0.0) {				/* flush request? */
#ifdef ACCELERAD
			if (use_optix)
				bogusray();
			else
#endif
			if (something2flush) {
				if (ray_pnprocs > 1 && ray_fifo_flush() < 0)
					error(USER, "child(ren) died");
				bogusray();
				fflush(stdout);
				nextflush = (!vresolu | (hresolu <= 1)) * hresolu;
				something2flush = 0;
			} else
				bogusray();
		} else {				/* compute and print */
#ifdef DAYSIM
			if (NumberOfSensorsInDaysimFile > 0) {  /* sensor units are specified using option '-U' */
				if (j < NumberOfSensorsInDaysimFile) {
					if (DaysimSensorUnits[j] == 1)
						imm_irrad = 1;
					else
						imm_irrad = 0;

					rtcompute(orig, direc, lim_dist ? d : 0.0);
					j++;
				} else { /* not enough sensors specified under '-U' */
					error(WARNING, "Not enough sensor units given under \'-U\'");
				}
			} else {
				rtcompute(orig, direc, lim_dist ? d : 0.0);
			}
#else
			rtcompute(orig, direc, lim_dist ? d : 0.0);
#endif
							/* flush if time */
#ifdef ACCELERAD
			if (!use_optix)
#endif
			if (!--nextflush) {
				if (ray_pnprocs > 1 && ray_fifo_flush() < 0)
					error(USER, "child(ren) died");
				fflush(stdout);
				nextflush = hresolu;
			} else
				something2flush = 1;
		}
#ifdef ACCELERAD
		if (use_optix) {
			/* resize array if necessary (should only happen when vcount == 0) */
			if (current_ray == total_rays) {
				total_rays *= 2;
				ray_cache = (RAY *)realloc(ray_cache, sizeof(RAY) * total_rays);
				if (ray_cache == NULL)
					error(SYSTEM, "out of memory in rtrace");
			}
			ray_cache[current_ray++] = thisray;
		} else /* Nothing ready to write yet. */
#endif
		if (ferror(stdout))
			error(SYSTEM, "write error");
		if (vcount && !--vcount)		/* check for end */
			break;
	}
#ifdef ACCELERAD
	if (use_optix) {
		/* Run OptiX kernel. */
		total_rays = current_ray;
		computeOptix(hresolu ? hresolu : 1, vresolu ? vresolu : total_rays, imm_irrad, ray_cache);

		/* Write output */
		for ( current_ray = 0u; current_ray < total_rays; ) {
			printvals(&ray_cache[current_ray++]);
		}

		free(ray_cache);
	} else /* OptiX kernel can only be launched from a single thread. */
#endif
	if (ray_pnprocs > 1) {				/* clean up children */
		if (ray_fifo_flush() < 0)
			error(USER, "unable to complete processing");
		ray_pclose(0);
	}
	if (fflush(stdout) < 0)
		error(SYSTEM, "write error");
	if (vcount)
		error(USER, "unexpected EOF on input");
	if (fname != NULL)
		fclose(fp);
}


static void
trace_sources(void)			/* trace rays to light sources, also */
{
	int	sn;
	
	for (sn = 0; sn < nsources; sn++)
		source[sn].sflags |= SFOLLOW;
}


static void
setoutput(				/* set up output tables */
	char  *vs
)
{
	oputf_t **table = ray_out;

	castonly = 1;
	while (*vs)
		switch (*vs++) {
		case 'T':				/* trace sources */
			if (!*vs) break;
			trace_sources();
			/* fall through */
		case 't':				/* trace */
			if (!*vs) break;
			*table = NULL;
			table = every_out;
			trace = ourtrace;
			castonly = 0;
			break;
		case 'o':				/* origin */
			*table++ = oputo;
			break;
		case 'd':				/* direction */
			*table++ = oputd;
			break;
		case 'r':				/* reflected contrib. */
			*table++ = oputr;
			castonly = 0;
			break;
		case 'R':				/* reflected distance */
			*table++ = oputR;
			castonly = 0;
			break;
		case 'x':				/* xmit contrib. */
			*table++ = oputx;
			castonly = 0;
			break;
		case 'X':				/* xmit distance */
			*table++ = oputX;
			castonly = 0;
			break;
		case 'v':				/* value */
			*table++ = oputv;
			castonly = 0;
			break;
		case 'V':				/* contribution */
			*table++ = oputV;
			if (ambounce > 0 && (ambacc > FTINY || ambssamp > 0))
				error(WARNING,
					"-otV accuracy depends on -aa 0 -as 0");
			break;
		case 'l':				/* effective distance */
			*table++ = oputl;
			castonly = 0;
			break;
#ifdef DAYSIM
		case 'c':               /* daylight coefficients */
			*table++ = daysimOutput;
			castonly = 0;
			break;
#else
		case 'c':				/* local coordinates */
			*table++ = oputc;
			break;
#endif
		case 'L':				/* single ray length */
			*table++ = oputL;
			break;
		case 'p':				/* point */
			*table++ = oputp;
			break;
		case 'n':				/* perturbed normal */
			*table++ = oputn;
			castonly = 0;
			break;
		case 'N':				/* unperturbed normal */
			*table++ = oputN;
			break;
		case 's':				/* surface */
			*table++ = oputs;
			break;
		case 'w':				/* weight */
			*table++ = oputw;
			break;
		case 'W':				/* coefficient */
			*table++ = oputW;
			castonly = 0;
			if (ambounce > 0 && (ambacc > FTINY) | (ambssamp > 0))
				error(WARNING,
					"-otW accuracy depends on -aa 0 -as 0");
			break;
		case 'm':				/* modifier */
			*table++ = oputm;
			break;
		case 'M':				/* material */
			*table++ = oputM;
			break;
		case '~':				/* tilde */
			*table++ = oputtilde;
			break;
		}
	*table = NULL;
							/* compatibility */
	for (table = ray_out; *table != NULL; table++) {
		if ((*table == oputV) | (*table == oputW))
			error(WARNING, "-oVW options require trace mode");
		if ((do_irrad | imm_irrad) &&
				(*table == oputr) | (*table == oputR) |
				(*table == oputx) | (*table == oputX))
			error(WARNING, "-orRxX options incompatible with -I+ and -i+");
	}
}


static void
bogusray(void)			/* print out empty record */
{
#ifdef ACCELERAD
	if (use_optix)
		return; /* The rest will occur after the OptiX kernel runs. */
#endif
	rayorigin(&thisray, PRIMARY, NULL, NULL);
	printvals(&thisray);
}


static void
raycast(			/* compute first ray intersection only */
	RAY *r
)
{
	if (!localhit(r, &thescene)) {
		if (r->ro == &Aftplane) {	/* clipped */
			r->ro = NULL;
			r->rot = FHUGE;
		} else
			sourcehit(r);
	}
}


static void
rayirrad(			/* compute irradiance rather than radiance */
	RAY *r
)
{
	void	(*old_revf)(RAY *) = r->revf;
					/* pretend we hit surface */
	r->rxt = r->rot = 1e-5;
	VSUM(r->rop, r->rorg, r->rdir, r->rot);
	r->ron[0] = -r->rdir[0];
	r->ron[1] = -r->rdir[1];
	r->ron[2] = -r->rdir[2];
	r->rod = 1.0;
					/* compute result */
	r->revf = raytrace;
	(*ofun[Lamb.otype].funp)(&Lamb, r);
	r->revf = old_revf;
}


static void
rtcompute(			/* compute and print ray value(s) */
	FVECT  org,
	FVECT  dir,
	double	dmax
)
{
					/* set up ray */
	rayorigin(&thisray, PRIMARY, NULL, NULL);
	if (imm_irrad) {
		VSUM(thisray.rorg, org, dir, 1.1e-4);
		thisray.rdir[0] = -dir[0];
		thisray.rdir[1] = -dir[1];
		thisray.rdir[2] = -dir[2];
		thisray.rmax = 0.0;
		thisray.revf = rayirrad;
	} else {
		VCOPY(thisray.rorg, org);
		VCOPY(thisray.rdir, dir);
		thisray.rmax = dmax;
		if (castonly)
			thisray.revf = raycast;
	}
#ifdef ACCELERAD
	if (use_optix)
		return; /* The rest will occur after the OptiX kernel runs. */
#endif
	if (ray_pnprocs > 1) {		/* multiprocessing FIFO? */
		if (ray_fifo_in(&thisray) < 0)
			error(USER, "lost children");
		return;
	}
	samplendx++;			/* else do it ourselves */
	rayvalue(&thisray);
	printvals(&thisray);
}


static int
printvals(			/* print requested ray values */
	RAY  *r
)
{
	oputf_t **tp;

	if (ray_out[0] == NULL)
		return(0);
	for (tp = ray_out; *tp != NULL; tp++)
		(**tp)(r);
	if (outform == 'a')
		putchar('\n');
	return(1);
}


static int
getvec(		/* get a vector from fp */
	FVECT  vec,
	int  fmt,
	FILE  *fp
)
{
	static float  vf[3];
	static double  vd[3];
	char  buf[32];
	int  i;

	switch (fmt) {
	case 'a':					/* ascii */
		for (i = 0; i < 3; i++) {
			if (fgetword(buf, sizeof(buf), fp) == NULL ||
					!isflt(buf))
				return(-1);
			vec[i] = atof(buf);
		}
		break;
	case 'f':					/* binary float */
		if (getbinary(vf, sizeof(float), 3, fp) != 3)
			return(-1);
		VCOPY(vec, vf);
		break;
	case 'd':					/* binary double */
		if (getbinary(vd, sizeof(double), 3, fp) != 3)
			return(-1);
		VCOPY(vec, vd);
		break;
	default:
		error(CONSISTENCY, "botched input format");
	}
	return(0);
}


void
tranotify(			/* record new modifier */
	OBJECT	obj
)
{
	static int  hitlimit = 0;
	OBJREC	 *o = objptr(obj);
	char  **tralp;

	if (obj == OVOID) {		/* starting over */
		traset[0] = 0;
		hitlimit = 0;
		return;
	}
	if (hitlimit || !ismodifier(o->otype))
		return;
	for (tralp = tralist; *tralp != NULL; tralp++)
		if (!strcmp(o->oname, *tralp)) {
			if (traset[0] >= MAXTSET) {
				error(WARNING, "too many modifiers in trace list");
				hitlimit++;
				return;		/* should this be fatal? */
			}
			insertelem(traset, obj);
			return;
		}
}


static void
ourtrace(				/* print ray values */
	RAY  *r
)
{
	oputf_t **tp;

	if (every_out[0] == NULL)
		return;
	if (r->ro == NULL) {
		if (traincl == 1)
			return;
	} else if (traincl != -1 && traincl != inset(traset, r->ro->omod))
		return;
	tabin(r);
	for (tp = every_out; *tp != NULL; tp++)
		(**tp)(r);
	if (outform == 'a')
		putchar('\n');
}


static void
tabin(				/* tab in appropriate amount */
	RAY  *r
)
{
	const RAY  *rp;

	for (rp = r->parent; rp != NULL; rp = rp->parent)
		putchar('\t');
}


static void
oputo(				/* print origin */
	RAY  *r
)
{
	(*putreal)(r->rorg, 3);
}


static void
oputd(				/* print direction */
	RAY  *r
)
{
	(*putreal)(r->rdir, 3);
}


static void
oputr(				/* print mirrored contribution */
	RAY  *r
)
{
	RREAL	cval[3];

	cval[0] = colval(r->mcol,RED);
	cval[1] = colval(r->mcol,GRN);
	cval[2] = colval(r->mcol,BLU);
	(*putreal)(cval, 3);
}



static void
oputR(				/* print mirrored distance */
	RAY  *r
)
{
	(*putreal)(&r->rmt, 1);
}


static void
oputx(				/* print unmirrored contribution */
	RAY  *r
)
{
	RREAL	cval[3];

	cval[0] = colval(r->rcol,RED) - colval(r->mcol,RED);
	cval[1] = colval(r->rcol,GRN) - colval(r->mcol,GRN);
	cval[2] = colval(r->rcol,BLU) - colval(r->mcol,BLU);
	(*putreal)(cval, 3);
}


static void
oputX(				/* print unmirrored distance */
	RAY  *r
)
{
	(*putreal)(&r->rxt, 1);
}


static void
oputv(				/* print value */
	RAY  *r
)
{
	RREAL	cval[3];

	cval[0] = colval(r->rcol,RED);
	cval[1] = colval(r->rcol,GRN);
	cval[2] = colval(r->rcol,BLU);
	(*putreal)(cval, 3);
}


static void
oputV(				/* print value contribution */
	RAY *r
)
{
	RREAL	contr[3];

	raycontrib(contr, r, PRIMARY);
	multcolor(contr, r->rcol);
	(*putreal)(contr, 3);
}


static void
oputl(				/* print effective distance */
	RAY  *r
)
{
	RREAL	d = raydistance(r);

	(*putreal)(&d, 1);
}


static void
oputL(				/* print single ray length */
	RAY  *r
)
{
	(*putreal)(&r->rot, 1);
}


static void
oputc(				/* print local coordinates */
	RAY  *r
)
{
	(*putreal)(r->uv, 2);
}


static RREAL	vdummy[3] = {0.0, 0.0, 0.0};


static void
oputp(				/* print point */
	RAY  *r
)
{
	if (r->rot < FHUGE*.99)
		(*putreal)(r->rop, 3);
	else
		(*putreal)(vdummy, 3);
}


static void
oputN(				/* print unperturbed normal */
	RAY  *r
)
{
	if (r->rot < FHUGE*.99) {
		if (r->rflips & 1) {	/* undo any flippin' flips */
			FVECT	unrm;
			unrm[0] = -r->ron[0];
			unrm[1] = -r->ron[1];
			unrm[2] = -r->ron[2];
			(*putreal)(unrm, 3);
		} else
			(*putreal)(r->ron, 3);
	} else
		(*putreal)(vdummy, 3);
}


static void
oputn(				/* print perturbed normal */
	RAY  *r
)
{
	FVECT  pnorm;

	if (r->rot >= FHUGE*.99) {
		(*putreal)(vdummy, 3);
		return;
	}
	raynormal(pnorm, r);
	(*putreal)(pnorm, 3);
}


static void
oputs(				/* print name */
	RAY  *r
)
{
	if (r->ro != NULL)
		fputs(r->ro->oname, stdout);
	else
		putchar('*');
	putchar('\t');
}


static void
oputw(				/* print weight */
	RAY  *r
)
{
	RREAL	rwt = r->rweight;
	
	(*putreal)(&rwt, 1);
}


static void
oputW(				/* print coefficient */
	RAY  *r
)
{
	RREAL	contr[3];
				/* shadow ray not on source? */
	if (r->rsrc >= 0 && source[r->rsrc].so != r->ro)
		setcolor(contr, 0.0, 0.0, 0.0);
	else
		raycontrib(contr, r, PRIMARY);

	(*putreal)(contr, 3);
}


static void
oputm(				/* print modifier */
	RAY  *r
)
{
	if (r->ro != NULL)
		if (r->ro->omod != OVOID)
			fputs(objptr(r->ro->omod)->oname, stdout);
		else
			fputs(VOIDID, stdout);
	else
		putchar('*');
	putchar('\t');
}


static void
oputM(				/* print material */
	RAY  *r
)
{
	OBJREC	*mat;

	if (r->ro != NULL) {
		if ((mat = findmaterial(r->ro)) != NULL)
			fputs(mat->oname, stdout);
		else
			fputs(VOIDID, stdout);
	} else
		putchar('*');
	putchar('\t');
}


static void
oputtilde(			/* output tilde (spacer) */
	RAY  *r
)
{
	fputs("~\t", stdout);
}


static void
puta(				/* print ascii value(s) */
	RREAL *v, int n
)
{
	if (n == 3) {
		printf("%e\t%e\t%e\t", v[0], v[1], v[2]);
		return;
	}
	while (n--)
		printf("%e\t", *v++);
}


static void
putd(RREAL *v, int n)		/* output binary double(s) */
{
#ifdef	SMLFLT
	double	da[3];
	int	i;

	if (n > 3)
		error(INTERNAL, "code error in putd()");
	for (i = n; i--; )
		da[i] = v[i];
	putbinary(da, sizeof(double), n, stdout);
#else
	putbinary(v, sizeof(RREAL), n, stdout);
#endif
}


static void
putf(RREAL *v, int n)		/* output binary float(s) */
{
#ifndef	SMLFLT
	float	fa[3];
	int	i;

	if (n > 3)
		error(INTERNAL, "code error in putf()");
	for (i = n; i--; )
		fa[i] = v[i];
	putbinary(fa, sizeof(float), n, stdout);
#else
	putbinary(v, sizeof(RREAL), n, stdout);
#endif
}


static void
putrgbe(RREAL *v, int n)	/* output RGBE color */
{
	COLR  cout;

	if (n != 3)
		error(INTERNAL, "putrgbe() not called with 3 components");
	setcolr(cout, v[0], v[1], v[2]);
	putbinary(cout, sizeof(cout), 1, stdout);
}

#ifdef DAYSIM
static void daysimOutput(RAY *r)				/* new function to print daylight_coef static int daysimOutput( RAY *r )*/
{
	int    k;
	double ratio, sum;

	if (outform == 'c') {
		float daylightCoef[DAYSIM_MAX_COEFS + 1];

		daylightCoef[0] = 0.0;
		for (k = 0; k < daysimGetCoefficients(); k++) {
			daylightCoef[0] += r->daylightCoef[k];
			daylightCoef[k + 1] = r->daylightCoef[k];
		}

		fwrite((char*)daylightCoef, sizeof(daylightCoef), 1, stdout);
		return;
	}

	if (daysimGetCoefficients() >= 2) {
		sum = 0.0;

		for (k = 0; k < daysimGetCoefficients(); k++) {
			RREAL dc = r->daylightCoef[k] / daysimLuminousSkySegments;
			sum += r->daylightCoef[k];

			(*putreal)(&dc, 1); // TODO one at a time may not be the most efficient way to do this
		}

		if (sum >= colval(r->rcol, RED)) { /* test whether the sum of daylight
										   coefficients corresponds to value for red */
			if (sum == 0)
				ratio = 1.0;
			else
				ratio = colval(r->rcol, RED) / sum;
		} else {
			if (colval(r->rcol, RED) == 0)
				ratio = 1.0;
			else
				ratio = sum / colval(r->rcol, RED);
		}
		if (ratio < 0.9999) {
			sprintf(errmsg,
				"The sum of the daylight cofficients is %e and does not equal the total red illuminance %e",
				sum, colval(r->rcol, RED));
			error(WARNING, errmsg);
		} else {
			//printf( "\n# ratio %12.9g\t[min( sum(DC)/ray-value, ray-value/sum(DC) )]", ratio );
		}
	}
}
#endif
