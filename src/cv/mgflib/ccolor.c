#ifndef lint
static const char RCSid[] = "$Id$";
#endif
/*
 * Spectral color handling routines
 */

#include <stdio.h>
#include "ccolor.h"


C_COLOR		c_dfcolor = C_DEFCOLOR;

				/* CIE 1931 Standard Observer curves */
static C_COLOR	cie_xf = { 1, NULL, C_CDSPEC|C_CSSPEC|C_CSXY|C_CSEFF,
			{14,42,143,435,1344,2839,3483,3362,2908,1954,956,
			320,49,93,633,1655,2904,4334,5945,7621,9163,10263,
			10622,10026,8544,6424,4479,2835,1649,874,468,227,
			114,58,29,14,7,3,2,1,0}, 106836L, .467, .368, 362.230
			};
static C_COLOR	cie_yf = { 1, NULL, C_CDSPEC|C_CSSPEC|C_CSXY|C_CSEFF,
			{0,1,4,12,40,116,230,380,600,910,1390,2080,3230,
			5030,7100,8620,9540,9950,9950,9520,8700,7570,6310,
			5030,3810,2650,1750,1070,610,320,170,82,41,21,10,
			5,2,1,1,0,0}, 106856L, .398, .542, 493.525
			};
static C_COLOR	cie_zf = { 1, NULL, C_CDSPEC|C_CSSPEC|C_CSXY|C_CSEFF,
			{65,201,679,2074,6456,13856,17471,17721,16692,
			12876,8130,4652,2720,1582,782,422,203,87,39,21,17,
			11,8,3,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
			106770L, .147, .077, 54.363
			};
				/* Derived CIE 1931 Primaries (imaginary) */
static C_COLOR	cie_xp = { 1, NULL, C_CDSPEC|C_CSSPEC|C_CSXY,
			{-174,-198,-195,-197,-202,-213,-235,-272,-333,
			-444,-688,-1232,-2393,-4497,-6876,-6758,-5256,
			-3100,-815,1320,3200,4782,5998,6861,7408,7754,
			7980,8120,8199,8240,8271,8292,8309,8283,8469,
			8336,8336,8336,8336,8336,8336},
			127424L, 1., .0,
			};
static C_COLOR	cie_yp = { 1, NULL, C_CDSPEC|C_CSSPEC|C_CSXY,
			{-451,-431,-431,-430,-427,-417,-399,-366,-312,
			-204,57,691,2142,4990,8810,9871,9122,7321,5145,
			3023,1123,-473,-1704,-2572,-3127,-3474,-3704,
			-3846,-3927,-3968,-3999,-4021,-4038,-4012,-4201,
			-4066,-4066,-4066,-4066,-4066,-4066},
			-23035L, .0, 1.,
			};
static C_COLOR	cie_zp = { 1, NULL, C_CDSPEC|C_CSSPEC|C_CSXY,
			{4051,4054,4052,4053,4054,4056,4059,4064,4071,
			4074,4056,3967,3677,2933,1492,313,-440,-795,
			-904,-918,-898,-884,-869,-863,-855,-855,-851,
			-848,-847,-846,-846,-846,-845,-846,-843,-845,
			-845,-845,-845,-845,-845},
			36057L, .0, .0,
			};


int
c_isgrey(clr)			/* check if color is grey */
register C_COLOR	*clr;
{
	if (!(clr->flags & (C_CSXY|C_CSSPEC)))
		return(1);		/* no settings == grey */
	c_ccvt(clr, C_CSXY);
	return(clr->cx >= .323 && clr->cx <= .343 &&
			clr->cy >= .323 && clr->cy <= .343);
}


void
c_ccvt(clr, fl)			/* convert color representations */
register C_COLOR	*clr;
int	fl;
{
	double	x, y, z;
	register int	i;

	fl &= ~clr->flags;			/* ignore what's done */
	if (!fl)				/* everything's done! */
		return;
	if (!(clr->flags & (C_CSXY|C_CSSPEC)))	/* nothing set! */
		*clr = c_dfcolor;
	if (fl & C_CSXY) {		/* cspec -> cxy */
		x = y = z = 0.;
		for (i = 0; i < C_CNSS; i++) {
			x += cie_xf.ssamp[i] * clr->ssamp[i];
			y += cie_yf.ssamp[i] * clr->ssamp[i];
			z += cie_zf.ssamp[i] * clr->ssamp[i];
		}
		x /= (double)cie_xf.ssum;
		y /= (double)cie_yf.ssum;
		z /= (double)cie_zf.ssum;
		z += x + y;
		clr->cx = x / z;
		clr->cy = y / z;
		clr->flags |= C_CSXY;
	} else if (fl & C_CSSPEC) {	/* cxy -> cspec */
		x = clr->cx;
		y = clr->cy;
		z = 1. - x - y;
		clr->ssum = 0;
		for (i = 0; i < C_CNSS; i++) {
			clr->ssamp[i] = x*cie_xp.ssamp[i] + y*cie_yp.ssamp[i]
					+ z*cie_zp.ssamp[i] + .5;
			if (clr->ssamp[i] < 0)		/* out of gamut! */
				clr->ssamp[i] = 0;
			else
				clr->ssum += clr->ssamp[i];
		}
		clr->flags |= C_CSSPEC;
	}
	if (fl & C_CSEFF) {		/* compute efficacy */
		if (clr->flags & C_CSSPEC) {		/* from spectrum */
			y = 0.;
			for (i = 0; i < C_CNSS; i++)
				y += cie_yf.ssamp[i] * clr->ssamp[i];
			clr->eff = C_CLPWM * y / clr->ssum;
		} else /* clr->flags & C_CSXY */ {	/* from (x,y) */
			clr->eff = clr->cx*cie_xf.eff + clr->cy*cie_yf.eff +
					(1. - clr->cx - clr->cy)*cie_zf.eff;
		}
		clr->flags |= C_CSEFF;
	}
}
