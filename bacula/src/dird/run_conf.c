/*
 *
 *  Configuration parser for Director Run Configuration
 *   directives, which are part of the Schedule Resource
 *
 *     Kern Sibbald, May MM
 *
 *     Version $Id$
 */
/*
   Copyright (C) 2000, 2001, 2002 Kern Sibbald and John Walker

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of
   the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public
   License along with this program; if not, write to the Free
   Software Foundation, Inc., 59 Temple Place - Suite 330, Boston,
   MA 02111-1307, USA.

 */

#include "bacula.h"
#include "dird.h"

extern URES res_all;
extern struct s_jl joblevels[];

/* Forward referenced subroutines */

enum e_state {
   s_none = 0,
   s_range,
   s_mday,
   s_month,
   s_time,
   s_at,
   s_wday,
   s_daily,
   s_weekly,
   s_monthly,
   s_hourly,
   s_wpos,			      /* 1st, 2nd, ...*/
};  

struct s_keyw {
  char *name;			      /* keyword */
  enum e_state state;		      /* parser state */
  int code;			      /* state value */
};

/* Keywords understood by parser */
static struct s_keyw keyw[] = {
  {N_("on"),         s_none,    0},
  {N_("at"),         s_at,      0},

  {N_("sun"),        s_wday,    0},
  {N_("mon"),        s_wday,    1},
  {N_("tue"),        s_wday,    2},
  {N_("wed"),        s_wday,    3},
  {N_("thu"),        s_wday,    4},
  {N_("fri"),        s_wday,    5},
  {N_("sat"),        s_wday,    6},
  {N_("jan"),        s_month,   0},
  {N_("feb"),        s_month,   1},
  {N_("mar"),        s_month,   2},
  {N_("apr"),        s_month,   3},
  {N_("may"),        s_month,   4},
  {N_("jun"),        s_month,   5},
  {N_("jul"),        s_month,   6},
  {N_("aug"),        s_month,   7},
  {N_("sep"),        s_month,   8},
  {N_("oct"),        s_month,   9},
  {N_("nov"),        s_month,  10},
  {N_("dec"),        s_month,  11},

  {N_("sunday"),     s_wday,    0},
  {N_("monday"),     s_wday,    1},
  {N_("tuesday"),    s_wday,    2},
  {N_("wednesday"),  s_wday,    3},
  {N_("thursday"),   s_wday,    4},
  {N_("friday"),     s_wday,    5},
  {N_("saturday"),   s_wday,    6},
  {N_("january"),    s_month,   0},
  {N_("february"),   s_month,   1},
  {N_("march"),      s_month,   2},
  {N_("april"),      s_month,   3},
  {N_("june"),       s_month,   5},
  {N_("july"),       s_month,   6},
  {N_("august"),     s_month,   7},
  {N_("september"),  s_month,   8},
  {N_("october"),    s_month,   9},
  {N_("november"),   s_month,  10},
  {N_("december"),   s_month,  11},

  {N_("daily"),      s_daily,   0},
  {N_("weekly"),     s_weekly,  0},
  {N_("monthly"),    s_monthly, 0},
  {N_("hourly"),     s_hourly,  0},

  {N_("1st"),        s_wpos,    0},
  {N_("2nd"),        s_wpos,    1},
  {N_("3rd"),        s_wpos,    2},
  {N_("4th"),        s_wpos,    3},
  {N_("5th"),        s_wpos,    4},

  {N_("first"),      s_wpos,    0},
  {N_("second"),     s_wpos,    1},
  {N_("third"),      s_wpos,    2},
  {N_("fourth"),     s_wpos,    3},
  {N_("fifth"),      s_wpos,    4},
  {NULL,	 s_none,    0}
};

static int have_hour, have_mday, have_wday, have_month, have_wpos;
static int have_at;
static RUN lrun;

static void clear_defaults()
{
   have_hour = have_mday = have_wday = have_month = have_wpos = TRUE;
   clear_bit(0,lrun.hour);
   clear_bits(0, 30, lrun.mday);
   clear_bits(0, 6, lrun.wday);
   clear_bits(0, 11, lrun.month);
   clear_bits(0, 4, lrun.wpos);
}

static void set_defaults()
{
   have_hour = have_mday = have_wday = have_month = have_wpos = FALSE;
   have_at = FALSE;
   set_bit(0,lrun.hour);
   set_bits(0, 30, lrun.mday);
   set_bits(0, 6, lrun.wday);
   set_bits(0, 11, lrun.month);
   set_bits(0, 4, lrun.wpos);
}


/* Keywords (RHS) permitted in Run records */
static struct s_kw RunFields[] = {
   {"pool",     'P'},
   {"level",    'L'},
   {"storage",  'S'},
   {"messages", 'M'},
   {"priority", 'p'},
   {NULL,	 0}
};

/* 
 * Store Schedule Run information   
 * 
 * Parse Run statement:
 *
 *  Run <keyword=value ...> [on] 2 january at 23:45
 *
 *   Default Run time is daily at 0:0
 *  
 *   There can be multiple run statements, they are simply chained
 *   together.
 *
 */
void store_run(LEX *lc, struct res_items *item, int index, int pass)
{
   int i, j, found;
   int token, state, state2 = 0, code = 0, code2 = 0;
   int options = lc->options;
   RUN **run = (RUN **)(item->value);	
   RUN *trun;
   char *p;
   RES *res;


   lc->options |= LOPT_NO_IDENT;      /* want only "strings" */

   /* clear local copy of run record */
   memset(&lrun, 0, sizeof(RUN));

   /* scan for Job level "full", "incremental", ... */
   for (found=TRUE; found; ) {
      found = FALSE;
      token = lex_get_token(lc, T_NAME);
      for (i=0; RunFields[i].name; i++) {
	 if (strcasecmp(lc->str, RunFields[i].name) == 0) {
	    found = TRUE;
	    if (lex_get_token(lc, T_ALL) != T_EQUALS) {
               scan_err1(lc, "Expected an equals, got: %s", lc->str);
	       /* NOT REACHED */ 
	    }
	    switch (RunFields[i].token) {
            case 'L':                 /* level */
	       token = lex_get_token(lc, T_NAME);
	       for (j=0; joblevels[j].level_name; j++) {
		  if (strcasecmp(lc->str, joblevels[j].level_name) == 0) {
		     lrun.level = joblevels[j].level;
		     lrun.job_type = joblevels[j].job_type;
		     j = 0;
		     break;
		  }
	       }
	       if (j != 0) {
                  scan_err1(lc, _("Job level field: %s not found in run record"), lc->str);
		  /* NOT REACHED */
	       }
	       break;
            case 'p':                 /* Priority */
	       token = lex_get_token(lc, T_PINT32);
	       if (pass == 2) {
		  lrun.Priority = lc->pint32_val;
	       }
	       break;
            case 'P':                 /* Pool */
	       token = lex_get_token(lc, T_NAME);
	       if (pass == 2) {
		  res = GetResWithName(R_POOL, lc->str);
		  if (res == NULL) {
                     scan_err1(lc, "Could not find specified Pool Resource: %s",
				lc->str);
		     /* NOT REACHED */
		  }
		  lrun.pool = (POOL *)res;
	       }
	       break;
            case 'S':                 /* storage */
	       token = lex_get_token(lc, T_NAME);
	       if (pass == 2) {
		  res = GetResWithName(R_STORAGE, lc->str);
		  if (res == NULL) {
                     scan_err1(lc, "Could not find specified Storage Resource: %s",
				lc->str);
		     /* NOT REACHED */
		  }
		  lrun.storage = (STORE *)res;
	       }
	       break;
            case 'M':                 /* messages */
	       token = lex_get_token(lc, T_NAME);
	       if (pass == 2) {
		  res = GetResWithName(R_MSGS, lc->str);
		  if (res == NULL) {
                     scan_err1(lc, "Could not find specified Messages Resource: %s",
				lc->str);
		     /* NOT REACHED */
		  }
		  lrun.msgs = (MSGS *)res;
	       }
	       break;
	    default:
               scan_err1(lc, "Expected a keyword name, got: %s", lc->str);
	       /* NOT REACHED */
	       break;
	    } /* end switch */	   
	 } /* end if strcasecmp */
      } /* end for RunFields */

      /* At this point, it is not a keyword. Check for old syle
       * Job Levels without keyword. This form is depreciated!!!
       */
      for (j=0; joblevels[j].level_name; j++) {
	 if (strcasecmp(lc->str, joblevels[j].level_name) == 0) {
	    lrun.level = joblevels[j].level;
	    lrun.job_type = joblevels[j].job_type;
	    found = TRUE;
	    break;
	 }
      }
   } /* end for found */


   /*
    * Scan schedule times.
    * Default is: daily at 0:0
    */
   state = s_none;
   set_defaults();

   for ( ; token != T_EOL; (token = lex_get_token(lc, T_ALL))) {
      int len, pm = 0;
      switch (token) {
      case T_NUMBER:
	 state = s_mday;
	 code = atoi(lc->str) - 1;
	 if (code < 0 || code > 30) {
            scan_err0(lc, _("Day number out of range (1-31)"));
	 }
	 break;
      case T_NAME:		   /* this handles drop through from keyword */
      case T_UNQUOTED_STRING:
         if (strchr(lc->str, (int)'-')) {
	    state = s_range;
	    break;
	 }
         if (strchr(lc->str, (int)':')) {
	    state = s_time;
	    break;
	 }
	 /* everything else must be a keyword */
	 for (i=0; keyw[i].name; i++) {
	    if (strcasecmp(lc->str, keyw[i].name) == 0) {
	       state = keyw[i].state;
	       code   = keyw[i].code;
	       i = 0;
	       break;
	    }
	 }
	 if (i != 0) {
            scan_err1(lc, _("Job type field: %s in run record not found"), lc->str);
	    /* NOT REACHED */
	 }
	 break;
      case T_COMMA:
	 continue;
      default:
         scan_err2(lc, _("Unexpected token: %d:%s"), token, lc->str);
	 /* NOT REACHED */
	 break;
      }
      switch (state) {
      case s_none:
	 continue;
      case s_mday:		   /* day of month */
	 if (!have_mday) {
	    clear_bits(0, 30, lrun.mday);
	    clear_bits(0, 6, lrun.wday);
	    have_mday = TRUE;
	 }
	 set_bit(code, lrun.mday);
	 break;
      case s_month:		   /* month of year */
	 if (!have_month) {
	    clear_bits(0, 11, lrun.month);
	    have_month = TRUE;
	 }
	 set_bit(code, lrun.month);
	 break;
      case s_wday:		   /* week day */
	 if (!have_wday) {
	    clear_bits(0, 6, lrun.wday);
	    clear_bits(0, 30, lrun.mday);
	    have_wday = TRUE;
	 }
	 set_bit(code, lrun.wday);
	 break;
      case s_wpos:		   /* Week position 1st, ... */
	 if (!have_wpos) {
	    clear_bits(0, 4, lrun.wpos);
	    have_wpos = TRUE;
	 }
	 set_bit(code, lrun.wpos);
	 break;
      case s_time:		   /* time */
	 if (!have_at) {
            scan_err0(lc, _("Time must be preceded by keyword AT."));
	    /* NOT REACHED */
	 }
	 if (!have_hour) {
	    clear_bit(0, lrun.hour);
	 }
         p = strchr(lc->str, ':');
	 if (!p)  {
            scan_err0(lc, _("Time logic error.\n"));
	    /* NOT REACHED */
	 }
	 *p++ = 0;		   /* separate two halves */
	 code = atoi(lc->str);
	 len = strlen(p);
         if (len > 2 && p[len-1] == 'm') {
            if (p[len-2] == 'a') {
	       pm = 0;
            } else if (p[len-2] == 'p') {
	       pm = 1;
	    } else {
               scan_err0(lc, _("Bad time specification."));
	       /* NOT REACHED */
	    }
	 } else {
	    pm = 0;
	 }
	 code2 = atoi(p);
	 if (pm) {
	    code += 12;
	 }
	 if (code < 0 || code > 23 || code2 < 0 || code2 > 59) {
            scan_err0(lc, _("Bad time specification."));
	    /* NOT REACHED */
	 }
	 set_bit(code, lrun.hour);
	 lrun.minute = code2;
	 have_hour = TRUE;
	 break;
      case s_at:
	 have_at = TRUE;
	 break;
      case s_range:
         p = strchr(lc->str, '-');
	 if (!p) {
            scan_err0(lc, _("Range logic error.\n"));
	 }
	 *p++ = 0;		   /* separate two halves */

	 /* Check for day range */
	 if (is_an_integer(lc->str) && is_an_integer(p)) {
	    code = atoi(lc->str) - 1;
	    code2 = atoi(p) - 1;
	    if (code < 0 || code > 30 || code2 < 0 || code2 > 30) {
               scan_err0(lc, _("Bad day range specification."));
	    }
	    if (!have_mday) {
	       clear_bits(0, 30, lrun.mday);
	       clear_bits(0, 6, lrun.wday);
	       have_mday = TRUE;
	    }
	    if (code < code2) {
	       set_bits(code, code2, lrun.mday);
	    } else {
	       set_bits(code, 30, lrun.mday);
	       set_bits(0, code2, lrun.mday);
	    }
	    break;
	 }

	 /* lookup first half of keyword range (week days or months) */
	 lcase(lc->str);
	 for (i=0; keyw[i].name; i++) {
	    if (strcmp(lc->str, keyw[i].name) == 0) {
	       state = keyw[i].state;
	       code   = keyw[i].code;
	       i = 0;
	       break;
	    }
	 }
	 if (i != 0 || (state != s_month && state != s_wday && state != s_wpos)) {
            scan_err0(lc, _("Invalid month, week or position day range"));
	    /* NOT REACHED */
	 }

	 /* Lookup end of range */
	 lcase(p);
	 for (i=0; keyw[i].name; i++) {
	    if (strcmp(p, keyw[i].name) == 0) {
	       state2  = keyw[i].state;
	       code2   = keyw[i].code;
	       i = 0;
	       break;
	    }
	 }
	 if (i != 0 || state != state2 || code == code2) {
            scan_err0(lc, _("Invalid month, weekday or position range"));
	    /* NOT REACHED */
	 }
	 if (state == s_wday) {
	    if (!have_wday) {
	       clear_bits(0, 6, lrun.wday);
	       clear_bits(0, 30, lrun.mday);
	       have_wday = TRUE;
	    }
	    if (code < code2) {
	       set_bits(code, code2, lrun.wday);
	    } else {
	       set_bits(code, 6, lrun.wday);
	       set_bits(0, code2, lrun.wday);
	    }
	 } else if (state == s_month) {
	    if (!have_month) {
	       clear_bits(0, 30, lrun.month);
	       have_month = TRUE;
	    }
	    if (code < code2) {
	       set_bits(code, code2, lrun.month);
	    } else {
	       /* this is a bit odd, but we accept it anyway */
	       set_bits(code, 30, lrun.month);
	       set_bits(0, code2, lrun.month);
	    }
	 } else {
	    /* Must be position */
	    if (!have_wpos) {
	       clear_bits(0, 4, lrun.wpos);
	       have_wpos = TRUE;
	    }
	    if (code < code2) {
	       set_bits(code, code2, lrun.wpos);
	    } else {
	       set_bits(code, 4, lrun.wpos);
	       set_bits(0, code2, lrun.wpos);
	    }
	 }			
	 break;
      case s_hourly:
	 clear_defaults();
	 set_bits(0, 23, lrun.hour);
	 set_bits(0, 30, lrun.mday);
	 set_bits(0, 11, lrun.month);
	 set_bits(0, 4, lrun.wpos);
	 break;
      case s_weekly:
	 clear_defaults();
	 set_bit(0, lrun.wday);
	 set_bits(0, 11, lrun.month);
	 set_bits(0, 4, lrun.wpos);
	 break;
      case s_daily:
	 clear_defaults();
	 set_bits(0, 30, lrun.mday);
	 set_bits(0, 11, lrun.month);
	 set_bits(0, 4,  lrun.wpos);
	 break;
      case s_monthly:
	 clear_defaults();
	 set_bits(0, 11, lrun.month);
	 set_bits(0, 4,  lrun.wpos);
	 break;
      default:
         scan_err0(lc, _("Unexpected run state\n"));
	 /* NOT REACHED */
	 break;
      }
   }

   /* Allocate run record, copy new stuff into it,
    * and link it into the list of run records 
    * in the schedule resource.
    */
   if (pass == 2) {
      trun = (RUN *)malloc(sizeof(RUN));
      memcpy(trun, &lrun, sizeof(RUN));
      if (*run) {
	 trun->next = *run;
      }
      *run = trun;
   }

   lc->options = options;	      /* restore scanner options */
   set_bit(index, res_all.res_sch.hdr.item_present);
}
