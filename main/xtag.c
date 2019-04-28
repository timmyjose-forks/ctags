/*
 *
 *  Copyright (c) 2015, Red Hat, Inc.
 *  Copyright (c) 2015, Masatake YAMATO
 *
 *  Author: Masatake YAMATO <yamato@redhat.com>
 *
 *   This source code is released for free distribution under the terms of the
 *   GNU General Public License version 2 or (at your option) any later version.
 *
 */

#include "general.h"  /* must always come first */
#include "ctags.h"
#include "debug.h"
#include "options.h"
#include "options_p.h"
#include "parse_p.h"
#include "routines.h"
#include "trashbox.h"
#include "writer_p.h"
#include "xtag.h"
#include "xtag_p.h"

#include <string.h>
#include <ctype.h>

typedef struct sXtagObject {
	xtagDefinition *def;
	langType language;
	xtagType sibling;
} xtagObject;

static bool isPseudoTagsEnabled (xtagDefinition *pdef CTAGS_ATTR_UNUSED)
{
	if (!writerCanPrintPtag())
		return false;

	return ! isDestinationStdout ();
}

static bool isPseudoTagsFixed (xtagDefinition *pdef CTAGS_ATTR_UNUSED)
{
	if (!writerCanPrintPtag())
		return true;
	else
		return false;
}

static void enableFileKind (xtagDefinition *pdef, bool state)
{
	enableDefaultFileKind(state);
	pdef->enabled = state;
}

static xtagDefinition xtagDefinitions [] = {
	{ true, 'F',  "fileScope",
	  "Include tags of file scope" },
	{ false, 'f', "inputFile",
	  "Include an entry for the base file name of every input file",
	  NULL,
	  NULL,
	  enableFileKind},
	{ false, 'p', "pseudo",
	  "Include pseudo tags",
	  isPseudoTagsEnabled,
	  isPseudoTagsFixed},
	{ false, 'q', "qualified",
	  "Include an extra class-qualified tag entry for each tag"},
	{ false, 'r', "reference",
	  "Include reference tags"},
	{ false, 'g', "guest",
	  "Include tags generated by guest parsers"},
	{ true, 's', "subparser",
	  "Include tags generated by subparsers"},
	{ false, '\0', "subword",
	  "Include tags for subwords generated by splitting the original tag (only for ctags development)"},
	{ true, '\0', "anonymous",
	  "Include tags for non-named objects like lambda"},
};

static unsigned int       xtagObjectUsed;
static unsigned int       xtagObjectAllocated;
static xtagObject* xtagObjects;

static xtagObject* getXtagObject (xtagType type)
{
	Assert ((0 <= type) && ((unsigned int)type < xtagObjectUsed));
	return (xtagObjects + type);
}

extern xtagDefinition* getXtagDefinition (xtagType type)
{
	Assert ((0 <= type) && ((unsigned int)type < xtagObjectUsed));

	return getXtagObject (type)->def;
}

typedef bool (* xtagPredicate) (xtagObject *pobj, langType language, const void *user_data);
static xtagType  getXtagTypeGeneric (xtagPredicate predicate, langType language, const void *user_data)
{
	static bool initialized = false;
	unsigned int i;

	if (language == LANG_AUTO && (initialized == false))
	{
		initialized = true;
		initializeParser (LANG_AUTO);
	}
	else if (language != LANG_IGNORE && (initialized == false))
		initializeParser (language);

	for (i = 0; i < xtagObjectUsed; i++)
	{
		if (predicate (xtagObjects + i, language, user_data))
			return i;
	}
	return XTAG_UNKNOWN;
}

static bool xtagEqualByLetter (xtagObject *pobj, langType language CTAGS_ATTR_UNUSED,
							   const void *user_data)
{
	return (pobj->def->letter == *((char *)user_data))? true: false;
}

extern xtagType  getXtagTypeForLetter (char letter)
{
	return getXtagTypeGeneric (xtagEqualByLetter, LANG_IGNORE, &letter);
}

static bool xtagEqualByNameAndLanguage (xtagObject *pobj, langType language, const void *user_data)
{
	const char* name = user_data;

	if ((language == LANG_AUTO || pobj->language == language)
		&& (strcmp (pobj->def->name, name) == 0))
		return true;
	else
		return false;
}

extern xtagType  getXtagTypeForNameAndLanguage (const char *name, langType language)
{
	return getXtagTypeGeneric (xtagEqualByNameAndLanguage, language, name);
}

extern struct colprintTable * xtagColprintTableNew (void)
{
	return colprintTableNew ("L:LETTER", "L:NAME", "L:ENABLED",
							 "L:LANGUAGE", "L:FIXED", "L:DESCRIPTION", NULL);
}

static void  xtagColprintAddLine (struct colprintTable *table, int xtype)
{
	xtagObject* xobj = getXtagObject (xtype);
	xtagDefinition *xdef = xobj->def;

	struct colprintLine *line = colprintTableGetNewLine(table);

	colprintLineAppendColumnChar (line,
								  (xdef->letter == NUL_XTAG_LETTER)
								  ? '-'
								  : xdef->letter);
	colprintLineAppendColumnCString (line, xdef->name);
	colprintLineAppendColumnBool (line, isXtagEnabled(xdef->xtype));
	colprintLineAppendColumnCString (line,
									 xobj->language == LANG_IGNORE
									 ? RSV_NONE
									 : getLanguageName (xobj->language));
	colprintLineAppendColumnBool (line, isXtagFixed(xdef->xtype));
	colprintLineAppendColumnCString (line, xdef->description);
}

extern void xtagColprintAddCommonLines (struct colprintTable *table)
{
	for (int i = 0; i < XTAG_COUNT; i++)
		xtagColprintAddLine (table, i);
}

extern void xtagColprintAddLanguageLines (struct colprintTable *table, langType language)
{
	for (unsigned int i = XTAG_COUNT; i < xtagObjectUsed; i++)
	{
		xtagObject* xobj = getXtagObject (i);

		if (xobj->language == language)
			xtagColprintAddLine (table, i);
	}
}

static int xtagColprintCompareLines (struct colprintLine *a , struct colprintLine *b)
{
	const char *a_parser = colprintLineGetColumn (a, 3);
	const char *b_parser = colprintLineGetColumn (b, 3);

	if (strcmp (a_parser, RSV_NONE) == 0
		&& strcmp (b_parser, RSV_NONE) != 0)
		return -1;
	else if (strcmp (a_parser, RSV_NONE) != 0
			 && strcmp (b_parser, RSV_NONE) == 0)
		return 1;
	else if (strcmp (a_parser, RSV_NONE) != 0
			 && strcmp (b_parser, RSV_NONE) != 0)
	{
		int r;
		r = strcmp (a_parser, b_parser);
		if (r != 0)
			return r;
	}
	else
	{
		int r;

		const char *a_letter = colprintLineGetColumn (a, 0);
		const char *b_letter = colprintLineGetColumn (b, 0);
		r = strcmp(a_letter, b_letter);
		if (r != 0)
			return r;
	}

	const char *a_name = colprintLineGetColumn (a, 1);
	const char *b_name = colprintLineGetColumn (b, 1);

	return strcmp(a_name, b_name);
}

extern void xtagColprintTablePrint (struct colprintTable *table,
									bool withListHeader, bool machinable, FILE *fp)
{
	colprintTableSort (table, xtagColprintCompareLines);
	colprintTablePrint (table, 0, withListHeader, machinable, fp);
}

extern bool isXtagEnabled (xtagType type)
{
	xtagDefinition* def = getXtagDefinition (type);

	Assert (def);

	if (def->isEnabled)
		return def->isEnabled (def);
	else
		return def->enabled;
}

extern bool isXtagFixed (xtagType type)
{
	xtagDefinition* def = getXtagDefinition (type);

	Assert (def);

	if (def->isFixed)
		return def->isFixed (def);

	return false;
}

extern bool enableXtag (xtagType type, bool state)
{
	bool old;
	xtagDefinition* def = getXtagDefinition (type);

	Assert (def);

	old = isXtagEnabled (type);

	if (isXtagFixed(type))
		def->enabled = old;
	else if (def->enable)
		def->enable (def, state);
	else
		def->enabled = state;

	def->isEnabled = NULL;

	return old;
}

extern bool isCommonXtag (xtagType type)
{
	return (type < XTAG_COUNT)? true: false;
}

extern int     getXtagOwner (xtagType type)
{
	return getXtagObject (type)->language;
}

const char* getXtagName (xtagType type)
{
	xtagDefinition* def = getXtagDefinition (type);
	if (def)
		return def->name;
	else
		return NULL;
}

extern void initXtagObjects (void)
{
	xtagObject *xobj;

	xtagObjectAllocated = ARRAY_SIZE (xtagDefinitions);
	xtagObjects = xMalloc (xtagObjectAllocated, xtagObject);
	DEFAULT_TRASH_BOX(&xtagObjects, eFreeIndirect);

	for (unsigned int i = 0; i < ARRAY_SIZE (xtagDefinitions); i++)
	{
		xobj = xtagObjects + i;
		xobj->def = xtagDefinitions + i;
		xobj->def->xtype = i;
		xobj->language = LANG_IGNORE;
		xobj->sibling = XTAG_UNKNOWN;
		xtagObjectUsed++;
	}
}

extern int countXtags (void)
{
	return xtagObjectUsed;
}

static void updateSiblingXtag (xtagType type, const char* name)
{
	int i;
	xtagObject *xobj;

	for (i = type; i > 0; i--)
	{
		xobj = xtagObjects + i - 1;
		if (xobj->def->name && (strcmp (xobj->def->name, name) == 0))
		{
			Assert (xobj->sibling == XTAG_UNKNOWN);
			xobj->sibling = type;
			break;
		}
	}
}

extern int defineXtag (xtagDefinition *def, langType language)
{
	xtagObject *xobj;
	size_t i;

	Assert (def);
	Assert (def->name);
	for (i = 0; i < strlen (def->name); i++)
	{
		Assert ( isalnum (def->name [i]) );
	}
	def->letter = NUL_XTAG_LETTER;

	if (xtagObjectUsed == xtagObjectAllocated)
	{
		xtagObjectAllocated *= 2;
		xtagObjects = xRealloc (xtagObjects, xtagObjectAllocated, xtagObject);
	}
	xobj = xtagObjects + (xtagObjectUsed);
	def->xtype = xtagObjectUsed++;
	xobj->def = def;
	xobj->language = language;
	xobj->sibling  = XTAG_UNKNOWN;

	updateSiblingXtag (def->xtype, def->name);

	verbose ("Add extra[%d]: %s,%s in %s\n",
			 def->xtype,
			 def->name, def->description,
			 getLanguageName (language));

	return def->xtype;
}

extern xtagType nextSiblingXtag (xtagType type)
{
	xtagObject *xobj;

	xobj = xtagObjects + type;
	return xobj->sibling;
}
