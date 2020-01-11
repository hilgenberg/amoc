/*
 * MOC - music on console
 * Copyright (C) 2005,2011 Damian Pietras <daper@daper.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

#include <librcc.h>
#include "rcc.h"

char *rcc_reencode (char *str)
{
	char *result = str;

	assert (str != NULL);

	rcc_string rccstring;

	rccstring = rccFrom (NULL, 0, str);
	if (rccstring) {
		if (*rccstring) {
			char *reencoded;

			reencoded = rccToCharset (NULL, "UTF-8", rccstring);
			if (reencoded) {
		    	free (result);
		    	result = reencoded;
			}
		}

		free (rccstring);
	}

	return result;
}

str rcc_reencode (const str &s)
{
	rcc_string rs = rccFrom (NULL, 0, s.c_str());
	if (rs) {
		if (*rs) {
			char *ss = rccToCharset (NULL, "UTF-8", rs);
			if (ss) {
				str ret = ss;
				free (ss);
			    	free (rs);
				return ret;
			}
		}
		free (rs);
	}
	return s;
}

void rcc_init ()
{
	rcc_class classes[] = {
		{"input", RCC_CLASS_STANDARD, NULL, NULL, "Input Encoding", 0},
		{"output", RCC_CLASS_KNOWN, NULL, NULL, "Output Encoding", 0},
		{NULL, (rcc_class_type)0, NULL, NULL, NULL, 0}
	};

	rccInit ();
	rccInitDefaultContext (NULL, 0, 0, classes, 0);
	rccLoad (NULL, "moc");
	rccSetOption (NULL, RCC_OPTION_TRANSLATE,
	                    RCC_OPTION_TRANSLATE_SKIP_PARRENT);
	rccSetOption (NULL, RCC_OPTION_AUTODETECT_LANGUAGE, 1);
}

void rcc_cleanup ()
{
	rccFree ();
}
