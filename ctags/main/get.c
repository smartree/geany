/*
*   Copyright (c) 1996-2002, Darren Hiebert
*
*   This source code is released for free distribution under the terms of the
*   GNU General Public License.
*
*   This module contains the high level source read functions (preprocessor
*   directives are handled within this level).
*/

/*
*   INCLUDE FILES
*/
#include "general.h"  /* must always come first */

#include <string.h>
#include <glib.h>

#include "entry.h"
#include "get.h"
#include "options.h"
#include "read.h"
#include "vstring.h"

/*
*   MACROS
*/
#define stringMatch(s1,s2)		(strcmp (s1,s2) == 0)
#define isspacetab(c)			((c) == SPACE || (c) == TAB)

/*
*   DATA DECLARATIONS
*/
typedef enum { COMMENT_NONE, COMMENT_C, COMMENT_CPLUS, COMMENT_D } Comment;

enum eCppLimits {
	MaxCppNestingLevel = 20,
	MaxDirectiveName = 10
};

/*  Defines the one nesting level of a preprocessor conditional.
 */
typedef struct sConditionalInfo {
	boolean ignoreAllBranches;  /* ignoring parent conditional branch */
	boolean singleBranch;       /* choose only one branch */
	boolean branchChosen;       /* branch already selected */
	boolean ignoring;           /* current ignore state */
} conditionalInfo;

enum eState {
	DRCTV_NONE,    /* no known directive - ignore to end of line */
	DRCTV_DEFINE,  /* "#define" encountered */
	DRCTV_HASH,    /* initial '#' read; determine directive */
	DRCTV_IF,      /* "#if" or "#ifdef" encountered */
	DRCTV_PRAGMA,  /* #pragma encountered */
	DRCTV_UNDEF    /* "#undef" encountered */
};

/*  Defines the current state of the pre-processor.
 */
typedef struct sCppState {
	int		ungetch, ungetch2;   /* ungotten characters, if any */
	boolean resolveRequired;     /* must resolve if/else/elif/endif branch */
	boolean hasAtLiteralStrings; /* supports @"c:\" strings */
	boolean hasCxxRawLiteralStrings; /* supports R"xxx(...)xxx" strings */
	struct sDirective {
		enum eState state;       /* current directive being processed */
		boolean	accept;          /* is a directive syntactically permitted? */
		vString * name;          /* macro name */
		unsigned int nestLevel;  /* level 0 is not used */
		conditionalInfo ifdef [MaxCppNestingLevel];
	} directive;
} cppState;

/*
*   DATA DEFINITIONS
*/

/*  Use brace formatting to detect end of block.
 */
static boolean BraceFormat = FALSE;

static cppState Cpp = {
	'\0', '\0',  /* ungetch characters */
	FALSE,       /* resolveRequired */
	FALSE,       /* hasAtLiteralStrings */
	FALSE,       /* hasCxxRawLiteralStrings */
	{
		DRCTV_NONE,  /* state */
		FALSE,       /* accept */
		NULL,        /* tag name */
		0,           /* nestLevel */
		{ {FALSE,FALSE,FALSE,FALSE} }  /* ifdef array */
	}  /* directive */
};

/*
*   FUNCTION DEFINITIONS
*/

extern boolean isBraceFormat (void)
{
	return BraceFormat;
}

extern unsigned int getDirectiveNestLevel (void)
{
	return Cpp.directive.nestLevel;
}

extern void cppInit (const boolean state, const boolean hasAtLiteralStrings,
                     const boolean hasCxxRawLiteralStrings)
{
	BraceFormat = state;

	Cpp.ungetch         = '\0';
	Cpp.ungetch2        = '\0';
	Cpp.resolveRequired = FALSE;
	Cpp.hasAtLiteralStrings = hasAtLiteralStrings;
	Cpp.hasCxxRawLiteralStrings = hasCxxRawLiteralStrings;

	Cpp.directive.state     = DRCTV_NONE;
	Cpp.directive.accept    = TRUE;
	Cpp.directive.nestLevel = 0;

	Cpp.directive.ifdef [0].ignoreAllBranches = FALSE;
	Cpp.directive.ifdef [0].singleBranch = FALSE;
	Cpp.directive.ifdef [0].branchChosen = FALSE;
	Cpp.directive.ifdef [0].ignoring     = FALSE;

	if (Cpp.directive.name == NULL)
		Cpp.directive.name = vStringNew ();
	else
		vStringClear (Cpp.directive.name);
}

extern void cppTerminate (void)
{
	if (Cpp.directive.name != NULL)
	{
		vStringDelete (Cpp.directive.name);
		Cpp.directive.name = NULL;
	}
}

extern void cppBeginStatement (void)
{
	Cpp.resolveRequired = TRUE;
}

extern void cppEndStatement (void)
{
	Cpp.resolveRequired = FALSE;
}

/*
*   Scanning functions
*
*   This section handles preprocessor directives.  It strips out all
*   directives and may emit a tag for #define directives.
*/

/*  This puts a character back into the input queue for the source File.
 *  Up to two characters may be ungotten.
 */
extern void cppUngetc (const int c)
{
	Assert (Cpp.ungetch2 == '\0');
	Cpp.ungetch2 = Cpp.ungetch;
	Cpp.ungetch = c;
}

/*  Reads a directive, whose first character is given by "c", into "name".
 */
static boolean readDirective (int c, char *const name, unsigned int maxLength)
{
	unsigned int i;

	for (i = 0  ;  i < maxLength - 1  ;  ++i)
	{
		if (i > 0)
		{
			c = getcFromInputFile ();
			if (c == EOF  ||  ! isalpha (c))
			{
				fileUngetc (c);
				break;
			}
		}
		name [i] = c;
	}
	name [i] = '\0';  /* null terminate */

	return (boolean) isspacetab (c);
}

/*  Reads an identifier, whose first character is given by "c", into "tag",
 *  together with the file location and corresponding line number.
 */
static void readIdentifier (int c, vString *const name)
{
	vStringClear (name);
	do
	{
		vStringPut (name, c);
		c = getcFromInputFile ();
	} while (c != EOF && isident (c));
	fileUngetc (c);
	vStringTerminate (name);
}

static conditionalInfo *currentConditional (void)
{
	return &Cpp.directive.ifdef [Cpp.directive.nestLevel];
}

static boolean isIgnore (void)
{
	return Cpp.directive.ifdef [Cpp.directive.nestLevel].ignoring;
}

static boolean setIgnore (const boolean ignore)
{
	return Cpp.directive.ifdef [Cpp.directive.nestLevel].ignoring = ignore;
}

static boolean isIgnoreBranch (void)
{
	conditionalInfo *const ifdef = currentConditional ();

	/*  Force a single branch if an incomplete statement is discovered
	 *  en route. This may have allowed earlier branches containing complete
	 *  statements to be followed, but we must follow no further branches.
	 */
	if (Cpp.resolveRequired  &&  ! BraceFormat)
		ifdef->singleBranch = TRUE;

	/*  We will ignore this branch in the following cases:
	 *
	 *  1.  We are ignoring all branches (conditional was within an ignored
	 *        branch of the parent conditional)
	 *  2.  A branch has already been chosen and either of:
	 *      a.  A statement was incomplete upon entering the conditional
	 *      b.  A statement is incomplete upon encountering a branch
	 */
	return (boolean) (ifdef->ignoreAllBranches ||
					 (ifdef->branchChosen  &&  ifdef->singleBranch));
}

static void chooseBranch (void)
{
	if (! BraceFormat)
	{
		conditionalInfo *const ifdef = currentConditional ();

		ifdef->branchChosen = (boolean) (ifdef->singleBranch ||
										Cpp.resolveRequired);
	}
}

/*  Pushes one nesting level for an #if directive, indicating whether or not
 *  the branch should be ignored and whether a branch has already been chosen.
 */
static boolean pushConditional (const boolean firstBranchChosen)
{
	const boolean ignoreAllBranches = isIgnore ();  /* current ignore */
	boolean ignoreBranch = FALSE;

	if (Cpp.directive.nestLevel < (unsigned int) MaxCppNestingLevel - 1)
	{
		conditionalInfo *ifdef;

		++Cpp.directive.nestLevel;
		ifdef = currentConditional ();

		/*  We take a snapshot of whether there is an incomplete statement in
		 *  progress upon encountering the preprocessor conditional. If so,
		 *  then we will flag that only a single branch of the conditional
		 *  should be followed.
		 */
		ifdef->ignoreAllBranches = ignoreAllBranches;
		ifdef->singleBranch      = Cpp.resolveRequired;
		ifdef->branchChosen      = firstBranchChosen;
		ifdef->ignoring = (boolean) (ignoreAllBranches || (
				! firstBranchChosen  &&  ! BraceFormat  &&
				(ifdef->singleBranch || !Option.if0)));
		ignoreBranch = ifdef->ignoring;
	}
	return ignoreBranch;
}

/*  Pops one nesting level for an #endif directive.
 */
static boolean popConditional (void)
{
	if (Cpp.directive.nestLevel > 0)
		--Cpp.directive.nestLevel;

	return isIgnore ();
}

static void makeDefineTag (const char *const name, boolean parameterized)
{
	const boolean isFileScope = (boolean) (! isHeaderFile ());

	if (includingDefineTags () &&
		(! isFileScope  ||  Option.include.fileScope))
	{
		tagEntryInfo e;

		initTagEntry (&e, name);

		e.lineNumberEntry = (boolean) (Option.locate != EX_PATTERN);
		e.isFileScope  = isFileScope;
		e.truncateLine = TRUE;
		e.kindName     = "macro";
		e.kind         = 'd';
		if (parameterized)
		{
			e.extensionFields.signature = getArglistFromFilePos(getInputFilePosition()
					, e.name);
		}
		makeTagEntry (&e);
		if (parameterized)
			free((char *) e.extensionFields.signature);
	}
}

static void directiveDefine (const int c)
{
	boolean parameterized;
	int nc;

	if (isident1 (c))
	{
		readIdentifier (c, Cpp.directive.name);
		nc = getcFromInputFile ();
		fileUngetc (nc);
		parameterized = (boolean) (nc == '(');
		if (! isIgnore ())
			makeDefineTag (vStringValue (Cpp.directive.name), parameterized);
	}
	Cpp.directive.state = DRCTV_NONE;
}

static void directivePragma (int c)
{
	if (isident1 (c))
	{
		readIdentifier (c, Cpp.directive.name);
		if (stringMatch (vStringValue (Cpp.directive.name), "weak"))
		{
			/* generate macro tag for weak name */
			do
			{
				c = getcFromInputFile ();
			} while (c == SPACE);
			if (isident1 (c))
			{
				readIdentifier (c, Cpp.directive.name);
				makeDefineTag (vStringValue (Cpp.directive.name), FALSE);
			}
		}
	}
	Cpp.directive.state = DRCTV_NONE;
}

static boolean directiveIf (const int c)
{
	const boolean ignore = pushConditional ((boolean) (c != '0'));

	Cpp.directive.state = DRCTV_NONE;

	return ignore;
}

static boolean directiveHash (const int c)
{
	boolean ignore = FALSE;
	char directive [MaxDirectiveName];
	DebugStatement ( const boolean ignore0 = isIgnore (); )

	readDirective (c, directive, MaxDirectiveName);
	if (stringMatch (directive, "define"))
		Cpp.directive.state = DRCTV_DEFINE;
	else if (stringMatch (directive, "undef"))
		Cpp.directive.state = DRCTV_UNDEF;
	else if (strncmp (directive, "if", (size_t) 2) == 0)
		Cpp.directive.state = DRCTV_IF;
	else if (stringMatch (directive, "elif")  ||
			stringMatch (directive, "else"))
	{
		ignore = setIgnore (isIgnoreBranch ());
		if (! ignore  &&  stringMatch (directive, "else"))
			chooseBranch ();
		Cpp.directive.state = DRCTV_NONE;
		DebugStatement ( if (ignore != ignore0) debugCppIgnore (ignore); )
	}
	else if (stringMatch (directive, "endif"))
	{
		DebugStatement ( debugCppNest (FALSE, Cpp.directive.nestLevel); )
		ignore = popConditional ();
		Cpp.directive.state = DRCTV_NONE;
		DebugStatement ( if (ignore != ignore0) debugCppIgnore (ignore); )
	}
	else if (stringMatch (directive, "pragma"))
		Cpp.directive.state = DRCTV_PRAGMA;
	else
		Cpp.directive.state = DRCTV_NONE;

	return ignore;
}

/*  Handles a pre-processor directive whose first character is given by "c".
 */
static boolean handleDirective (const int c)
{
	boolean ignore = isIgnore ();

	switch (Cpp.directive.state)
	{
		case DRCTV_NONE:    ignore = isIgnore ();        break;
		case DRCTV_DEFINE:  directiveDefine (c);         break;
		case DRCTV_HASH:    ignore = directiveHash (c);  break;
		case DRCTV_IF:      ignore = directiveIf (c);    break;
		case DRCTV_PRAGMA:  directivePragma (c);         break;
		case DRCTV_UNDEF:   directiveDefine (c);         break;
	}
	return ignore;
}

/*  Called upon reading of a slash ('/') characters, determines whether a
 *  comment is encountered, and its type.
 */
static Comment isComment (void)
{
	Comment comment;
	const int next = getcFromInputFile ();

	if (next == '*')
		comment = COMMENT_C;
	else if (next == '/')
		comment = COMMENT_CPLUS;
	else if (next == '+')
		comment = COMMENT_D;
	else
	{
		fileUngetc (next);
		comment = COMMENT_NONE;
	}
	return comment;
}

/*  Skips over a C style comment. According to ANSI specification a comment
 *  is treated as white space, so we perform this substitution.
 */
int skipOverCComment (void)
{
	int c = getcFromInputFile ();

	while (c != EOF)
	{
		if (c != '*')
			c = getcFromInputFile ();
		else
		{
			const int next = getcFromInputFile ();

			if (next != '/')
				c = next;
			else
			{
				c = SPACE;  /* replace comment with space */
				break;
			}
		}
	}
	return c;
}

/*  Skips over a C++ style comment.
 */
static int skipOverCplusComment (void)
{
	int c;

	while ((c = getcFromInputFile ()) != EOF)
	{
		if (c == BACKSLASH)
			getcFromInputFile ();  /* throw away next character, too */
		else if (c == NEWLINE)
			break;
	}
	return c;
}

/* Skips over a D style comment.
 * Really we should match nested /+ comments. At least they're less common.
 */
static int skipOverDComment (void)
{
	int c = getcFromInputFile ();

	while (c != EOF)
	{
		if (c != '+')
			c = getcFromInputFile ();
		else
		{
			const int next = getcFromInputFile ();

			if (next != '/')
				c = next;
			else
			{
				c = SPACE;  /* replace comment with space */
				break;
			}
		}
	}
	return c;
}

/*  Skips to the end of a string, returning a special character to
 *  symbolically represent a generic string.
 */
static int skipToEndOfString (boolean ignoreBackslash)
{
	int c;

	while ((c = getcFromInputFile ()) != EOF)
	{
		if (c == BACKSLASH && ! ignoreBackslash)
			getcFromInputFile ();  /* throw away next character, too */
		else if (c == DOUBLE_QUOTE)
			break;
	}
	return STRING_SYMBOL;  /* symbolic representation of string */
}

static int isCxxRawLiteralDelimiterChar (int c)
{
	return (c != ' ' && c != '\f' && c != '\n' && c != '\r' && c != '\t' && c != '\v' &&
	        c != '(' && c != ')' && c != '\\');
}

static int skipToEndOfCxxRawLiteralString (void)
{
	int c = getcFromInputFile ();

	if (c != '(' && ! isCxxRawLiteralDelimiterChar (c))
	{
		fileUngetc (c);
		c = skipToEndOfString (FALSE);
	}
	else
	{
		char delim[16];
		unsigned int delimLen = 0;
		boolean collectDelim = TRUE;

		do
		{
			if (collectDelim)
			{
				if (isCxxRawLiteralDelimiterChar (c) &&
				    delimLen < (sizeof delim / sizeof *delim))
					delim[delimLen++] = c;
				else
					collectDelim = FALSE;
			}
			else if (c == ')')
			{
				unsigned int i = 0;

				while ((c = getcFromInputFile ()) != EOF && i < delimLen && delim[i] == c)
					i++;
				if (i == delimLen && c == DOUBLE_QUOTE)
					break;
				else
					fileUngetc (c);
			}
		}
		while ((c = getcFromInputFile ()) != EOF);
		c = STRING_SYMBOL;
	}
	return c;
}

/*  Skips to the end of the three (possibly four) 'c' sequence, returning a
 *  special character to symbolically represent a generic character.
 *  Also detects Vera numbers that include a base specifier (ie. 'b1010).
 */
static int skipToEndOfChar (void)
{
	int c;
	int count = 0, veraBase = '\0';

	while ((c = getcFromInputFile ()) != EOF)
	{
	    ++count;
		if (c == BACKSLASH)
			getcFromInputFile ();  /* throw away next character, too */
		else if (c == SINGLE_QUOTE)
			break;
		else if (c == NEWLINE)
		{
			fileUngetc (c);
			break;
		}
		else if (count == 1  &&  strchr ("DHOB", toupper (c)) != NULL)
			veraBase = c;
		else if (veraBase != '\0'  &&  ! isalnum (c))
		{
			fileUngetc (c);
			break;
		}
	}
	return CHAR_SYMBOL;  /* symbolic representation of character */
}

/*  This function returns the next character, stripping out comments,
 *  C pre-processor directives, and the contents of single and double
 *  quoted strings. In short, strip anything which places a burden upon
 *  the tokenizer.
 */
extern int cppGetc (void)
{
	boolean directive = FALSE;
	boolean ignore = FALSE;
	int c;

	if (Cpp.ungetch != '\0')
	{
		c = Cpp.ungetch;
		Cpp.ungetch = Cpp.ungetch2;
		Cpp.ungetch2 = '\0';
		return c;  /* return here to avoid re-calling debugPutc () */
	}
	else do
	{
		c = getcFromInputFile ();
process:
		switch (c)
		{
			case EOF:
				ignore    = FALSE;
				directive = FALSE;
				break;

			case TAB:
			case SPACE:
				break;  /* ignore most white space */

			case NEWLINE:
				if (directive  &&  ! ignore)
					directive = FALSE;
				Cpp.directive.accept = TRUE;
				break;

			case DOUBLE_QUOTE:
				Cpp.directive.accept = FALSE;
				c = skipToEndOfString (FALSE);
				break;

			case '#':
				if (Cpp.directive.accept)
				{
					directive = TRUE;
					Cpp.directive.state  = DRCTV_HASH;
					Cpp.directive.accept = FALSE;
				}
				break;

			case SINGLE_QUOTE:
				Cpp.directive.accept = FALSE;
				c = skipToEndOfChar ();
				break;

			case '/':
			{
				const Comment comment = isComment ();

				if (comment == COMMENT_C)
					c = skipOverCComment ();
				else if (comment == COMMENT_CPLUS)
				{
					c = skipOverCplusComment ();
					if (c == NEWLINE)
						fileUngetc (c);
				}
				else if (comment == COMMENT_D)
					c = skipOverDComment ();
				else
					Cpp.directive.accept = FALSE;
				break;
			}

			case BACKSLASH:
			{
				int next = getcFromInputFile ();

				if (next == NEWLINE)
					continue;
				else
					fileUngetc (next);
				break;
			}

			case '?':
			{
				int next = getcFromInputFile ();
				if (next != '?')
					fileUngetc (next);
				else
				{
					next = getcFromInputFile ();
					switch (next)
					{
						case '(':          c = '[';       break;
						case ')':          c = ']';       break;
						case '<':          c = '{';       break;
						case '>':          c = '}';       break;
						case '/':          c = BACKSLASH; goto process;
						case '!':          c = '|';       break;
						case SINGLE_QUOTE: c = '^';       break;
						case '-':          c = '~';       break;
						case '=':          c = '#';       goto process;
						default:
							fileUngetc ('?');
							fileUngetc (next);
							break;
					}
				}
			} break;

			/* digraphs:
			 * input:  <:  :>  <%  %>  %:  %:%:
			 * output: [   ]   {   }   #   ##
			 */
			case '<':
			{
				int next = getcFromInputFile ();
				switch (next)
				{
					case ':':	c = '['; break;
					case '%':	c = '{'; break;
					default: fileUngetc (next);
				}
				goto enter;
			}
			case ':':
			{
				int next = getcFromInputFile ();
				if (next == '>')
					c = ']';
				else
					fileUngetc (next);
				goto enter;
			}
			case '%':
			{
				int next = getcFromInputFile ();
				switch (next)
				{
					case '>':	c = '}'; break;
					case ':':	c = '#'; goto process;
					default: fileUngetc (next);
				}
				goto enter;
			}

			default:
				if (c == '@' && Cpp.hasAtLiteralStrings)
				{
					int next = getcFromInputFile ();
					if (next == DOUBLE_QUOTE)
					{
						Cpp.directive.accept = FALSE;
						c = skipToEndOfString (TRUE);
						break;
					}
					else
						fileUngetc (next);
				}
				else if (c == 'R' && Cpp.hasCxxRawLiteralStrings)
				{
					/* OMG!11 HACK!!11  Get the previous character.
					 *
					 * We need to know whether the previous character was an identifier or not,
					 * because "R" has to be on its own, not part of an identifier.  This allows
					 * for constructs like:
					 *
					 * 	#define FOUR "4"
					 * 	const char *p = FOUR"5";
					 *
					 * which is not a raw literal, but a preprocessor concatenation.
					 *
					 * FIXME: handle
					 *
					 * 	const char *p = R\
					 * 	"xxx(raw)xxx";
					 *
					 * which is perfectly valid (yet probably very unlikely). */
					int prev = fileGetNthPrevC (1, '\0');
					int prev2 = fileGetNthPrevC (2, '\0');
					int prev3 = fileGetNthPrevC (3, '\0');

					if (! isident (prev) ||
					    (! isident (prev2) && (prev == 'L' || prev == 'u' || prev == 'U')) ||
					    (! isident (prev3) && (prev2 == 'u' && prev == '8')))
					{
						int next = getcFromInputFile ();
						if (next != DOUBLE_QUOTE)
							fileUngetc (next);
						else
						{
							Cpp.directive.accept = FALSE;
							c = skipToEndOfCxxRawLiteralString ();
							break;
						}
					}
				}
			enter:
				Cpp.directive.accept = FALSE;
				if (directive)
					ignore = handleDirective (c);
				break;
		}
	} while (directive || ignore);

	DebugStatement ( debugPutc (DEBUG_CPP, c); )
	DebugStatement ( if (c == NEWLINE)
				debugPrintf (DEBUG_CPP, "%6ld: ", getInputLineNumber () + 1); )

	return c;
}

extern char *getArglistFromFilePos(MIOPos startPosition, const char *tokenName)
{
	MIOPos originalPosition;
	char *result = NULL;
	char *arglist = NULL;
	long pos1, pos2;

	pos2 = mio_tell(File.fp);

	mio_getpos(File.fp, &originalPosition);
	mio_setpos(File.fp, &startPosition);
	pos1 = mio_tell(File.fp);

	if (pos2 > pos1)
	{
		size_t len = pos2 - pos1;

		result = (char *) g_malloc(len + 1);
		if (result != NULL && (len = mio_read(File.fp, result, 1, len)) > 0)
		{
			result[len] = '\0';
			arglist = getArglistFromStr(result, tokenName);
		}
		g_free(result);
	}
	mio_setpos(File.fp, &originalPosition);
	return arglist;
}

typedef enum
{
	st_none_t,
	st_escape_t,
	st_c_comment_t,
	st_cpp_comment_t,
	st_double_quote_t,
	st_single_quote_t
} ParseState;

static void stripCodeBuffer(char *buf)
{
	int i = 0, pos = 0;
	ParseState state = st_none_t, prev_state = st_none_t;

	while (buf[i] != '\0')
	{
		switch(buf[i])
		{
			case '/':
				if (st_none_t == state)
				{
					/* Check if this is the start of a comment */
					if (buf[i+1] == '*') /* C comment */
						state = st_c_comment_t;
					else if (buf[i+1] == '/') /* C++ comment */
						state = st_cpp_comment_t;
					else /* Normal character */
						buf[pos++] = '/';
				}
				else if (st_c_comment_t == state)
				{
					/* Check if this is the end of a C comment */
					if (buf[i-1] == '*')
					{
						if ((pos > 0) && (buf[pos-1] != ' '))
							buf[pos++] = ' ';
						state = st_none_t;
					}
				}
				break;
			case '"':
				if (st_none_t == state)
					state = st_double_quote_t;
				else if (st_double_quote_t == state)
					state = st_none_t;
				break;
			case '\'':
				if (st_none_t == state)
					state = st_single_quote_t;
				else if (st_single_quote_t == state)
					state = st_none_t;
				break;
			default:
				if ((buf[i] == '\\') && (st_escape_t != state))
				{
					prev_state = state;
					state = st_escape_t;
				}
				else if (st_escape_t == state)
				{
					state = prev_state;
					prev_state = st_none_t;
				}
				else if ((buf[i] == '\n') && (st_cpp_comment_t == state))
				{
					if ((pos > 0) && (buf[pos-1] != ' '))
						buf[pos++] = ' ';
					state = st_none_t;
				}
				else if (st_none_t == state)
				{
					if (isspace(buf[i]))
					{
						if ((pos > 0) && (buf[pos-1] != ' '))
							buf[pos++] = ' ';
					}
					else
						buf[pos++] = buf[i];
				}
				break;
		}
		++i;
	}
	buf[pos] = '\0';
	return;
}

extern char *getArglistFromStr(char *buf, const char *name)
{
	char *start, *end;
	int level;
	if ((NULL == buf) || (NULL == name) || ('\0' == name[0]))
		return NULL;
	stripCodeBuffer(buf);
	if (NULL == (start = strstr(buf, name)))
		return NULL;
	if (NULL == (start = strchr(start, '(')))
		return NULL;
	for (level = 1, end = start + 1; level > 0; ++end)
	{
		if ('\0' == *end)
			break;
		else if ('(' == *end)
			++ level;
		else if (')' == *end)
			-- level;
	}
	*end = '\0';
	return strdup(start);
}

/* vi:set tabstop=4 shiftwidth=4: */
