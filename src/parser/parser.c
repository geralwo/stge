#define _DEFAULT_SOURCE

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/native.h"
#include "core/vm.h"
#include "parser/ast.h"
#include "parser/parser.h"

#define STGE_MAX_NS 32
#define STGE_MAX_INCLUDE 8
#define STGE_MAX_PATH 8
#define STGE_MAX_FIELD_KINDS 32
#define STGE_MAX_CONST 128
#define STGE_MAX_CALL_PATCH 32
#define STGE_MAX_PACKED 16
#define STGE_MAX_ALIASES 8

typedef enum
{
	EK_INT = 0,
	EK_STR,
	EK_FLOAT,
	EK_DOUBLE,
	EK_OBJ
} ExprKind;

typedef enum
{
	FK_NONE = 0,
	FK_INT,
	FK_UINT,
	FK_FLOAT,
	FK_DOUBLE,
	FK_STR,
	FK_OBJ,
	FK_BYTE
} FieldKind;

typedef struct
{
	char name[STGE_NAME_MAX];
	FieldKind kind;
	int32_t arr_len; /* n for T[n], -1 for T[], 1 for scalar */
	bool growable;
	char type[STGE_NAME_MAX];
} FieldType;

/* ---- tokens ---------------------------------------------------------- */

typedef enum
{
	TOK_IDENT,
	TOK_STRING,
	TOK_CHAR,
	TOK_INT,
	TOK_FLOAT,
	TOK_EQUAL,
	TOK_EQEQ,
	TOK_LT,
	TOK_LTE,
	TOK_GT,
	TOK_GTE,
	TOK_DOT,
	TOK_COLONCOLON,
	TOK_LPAREN,
	TOK_RPAREN,
	TOK_LBRACE,
	TOK_RBRACE,
	TOK_COMMA,
	TOK_LBRACK,
	TOK_RBRACK,
	TOK_PLUS,
	TOK_PLUSEQ,
	TOK_MINUS,
	TOK_MINUSEQ,
	TOK_ARROW,
	TOK_STAR,
	TOK_STAREQ,
	TOK_SLASH,
	TOK_SLASHEQ,
	TOK_EOL,
	TOK_EOF,
	TOK_BAD
} TokKind;

typedef struct
{
	TokKind kind;
	char *text;
	size_t len;
	int32_t value;
	double dvalue;
	int line;
	int col;
	size_t start;
	bool hole;       /* string chunk is followed by ${ */
	bool closes_str; /* '}' ends a ${ ... } hole */
} Tok;

#define STGE_MAX_INTERP 8

typedef struct
{
	const char *src;
	size_t len;
	size_t pos;
	int line;
	bool cont_string;
	int interp_sp;
	int interp_depth[STGE_MAX_INTERP];
	char *buf;
	size_t buf_cap;
	size_t buf_len;
	char **persist;
	int persist_count;
	int persist_cap;
} Lex;

static bool lex_buf_reserve(Lex *lex, size_t need)
{
	size_t cap;
	char *grown;

	if (lex == NULL)
		return false;
	if (need <= lex->buf_cap)
		return true;

	cap = lex->buf_cap == 0 ? 64 : lex->buf_cap;
	while (cap < need)
	{
		if (cap > (SIZE_MAX / 2))
			return false;
		cap *= 2;
	}

	grown = realloc(lex->buf, cap);
	if (grown == NULL)
		return false;

	lex->buf = grown;
	lex->buf_cap = cap;
	return true;
}

static bool lex_buf_clear(Lex *lex)
{
	if (!lex_buf_reserve(lex, 1))
		return false;

	lex->buf[0] = '\0';
	lex->buf_len = 0;
	return true;
}

static bool lex_buf_push(Lex *lex, char ch)
{
	if (!lex_buf_reserve(lex, lex->buf_len + 2))
		return false;

	lex->buf[lex->buf_len++] = ch;
	lex->buf[lex->buf_len] = '\0';
	return true;
}

static char *lex_persist(Lex *lex, const char *s, size_t n)
{
	char *copy;
	char **grown;

	if (lex == NULL)
		return NULL;

	if (s == NULL)
	{
		s = "";
		n = 0;
	}

	copy = malloc(n + 1);
	if (copy == NULL)
		return NULL;

	memcpy(copy, s, n);
	copy[n] = '\0';

	if (lex->persist_count >= lex->persist_cap)
	{
		int cap = lex->persist_cap == 0 ? 32 : lex->persist_cap * 2;

		grown = realloc(lex->persist, (size_t)cap * sizeof *grown);
		if (grown == NULL)
		{
			free(copy);
			return NULL;
		}

		lex->persist = grown;
		lex->persist_cap = cap;
	}

	lex->persist[lex->persist_count++] = copy;
	return copy;
}


static void lex_keep_buf(Lex *dst, const Lex *src)
{
	char *buf;
	size_t cap;
	char **persist;
	int persist_count;
	int persist_cap;

	if (dst == NULL || src == NULL)
		return;

	buf = dst->buf;
	cap = dst->buf_cap;
	persist = dst->persist;
	persist_count = dst->persist_count;
	persist_cap = dst->persist_cap;
	*dst = *src;
	dst->buf = buf;
	dst->buf_cap = cap;
	dst->buf_len = 0;
	dst->persist = persist;
	dst->persist_count = persist_count;
	dst->persist_cap = persist_cap;
}

static void lex_persist_free(Lex *lex)
{
	int i;

	if (lex == NULL)
		return;

	for (i = 0; i < lex->persist_count; i++)
		free(lex->persist[i]);
	free(lex->persist);
	lex->persist = NULL;
	lex->persist_count = 0;
	lex->persist_cap = 0;
}

/* Copy position/interp state for lookahead without sharing buf/persist. */
static Lex lex_probe(const Lex *src)
{
	Lex lex;

	memset(&lex, 0, sizeof lex);
	if (src == NULL)
		return lex;

	lex.src = src->src;
	lex.len = src->len;
	lex.pos = src->pos;
	lex.line = src->line;
	lex.cont_string = src->cont_string;
	lex.interp_sp = src->interp_sp;
	memcpy(lex.interp_depth, src->interp_depth, sizeof lex.interp_depth);
	return lex;
}

static void lex_probe_free(Lex *lex)
{
	if (lex == NULL)
		return;

	free(lex->buf);
	lex->buf = NULL;
	lex->buf_cap = 0;
	lex->buf_len = 0;
	lex_persist_free(lex);
}

static void tok_from_buf(Tok *tok, Lex *lex)
{
	char *copy = lex_persist(lex, lex->buf, lex->buf_len);

	tok->text = copy != NULL ? copy : "";
	tok->len = copy != NULL ? lex->buf_len : 0;
}

static void tok_copy(char *dst, size_t n, const Tok *tok)
{
	size_t len;

	if (dst == NULL || n == 0)
		return;

	len = tok != NULL ? tok->len : 0;
	if (tok == NULL || tok->text == NULL)
		len = 0;
	if (len >= n)
		len = n - 1;
	if (len > 0)
		memcpy(dst, tok->text, len);
	dst[len] = '\0';
}


static char peek(const Lex *lex)
{
	if (lex->pos >= lex->len)
		return '\0';

	return lex->src[lex->pos];
}

static char next(Lex *lex)
{
	char c;

	if (lex->pos >= lex->len)
		return '\0';

	c = lex->src[lex->pos++];
	if (c == '\n')
		lex->line++;

	return c;
}

static void skip_space(Lex *lex)
{
	for (;;)
	{
		char c = peek(lex);

		while (c == ' ' || c == '\t' || c == '\r')
		{
			next(lex);
			c = peek(lex);
		}

		if (c == '/' && lex->pos + 1 < lex->len && lex->src[lex->pos + 1] == '/')
		{
			while (peek(lex) != '\0' && peek(lex) != '\n')
				next(lex);
			continue;
		}

		break;
	}
}

static bool take_word(Lex *lex, Tok *tok)
{
	size_t start;

	if (lex == NULL || tok == NULL || lex->src == NULL)
		return false;

	start = lex->pos;
	while (isalnum((unsigned char)peek(lex)) || peek(lex) == '_')
		next(lex);

	if (lex->pos == start)
		return false;

	tok->text = (char *)(lex->src + start);
	tok->len = lex->pos - start;
	return true;
}

static bool tok_eq(const Tok *tok, const char *s)
{
	size_t n;

	if (tok == NULL || tok->text == NULL || s == NULL)
		return false;

	n = strlen(s);
	return tok->len == n && memcmp(tok->text, s, n) == 0;
}

static int column_at(const Lex *lex, size_t pos)
{
	size_t i;
	int col = 1;

	if (lex == NULL || lex->src == NULL)
		return 1;

	i = pos;
	while (i > 0 && lex->src[i - 1] != '\n')
		i--;

	while (i < pos && i < lex->len)
	{
		col++;
		i++;
	}

	return col;
}

/* quote is already consumed. "${" in "..." pauses the string; the matching
   '}' (tracked in lex->interp_*) resumes it via cont_string. */
static Tok lex_string(Lex *lex, char quote)
{
	Tok tok;
	int decode = quote == '"';

	memset(&tok, 0, sizeof tok);
	tok.line = lex->line;
	tok.start = lex->pos;
	tok.col = column_at(lex, lex->pos);
	tok.kind = quote == '\'' ? TOK_CHAR : TOK_STRING;
	if (!lex_buf_clear(lex))
	{
		tok.kind = TOK_BAD;
		return tok;
	}

	while (peek(lex) != '\0' && peek(lex) != quote && peek(lex) != '\n')
	{
		char ch = next(lex);

		if (decode && ch == '\\')
		{
			char e = peek(lex);

			if (e == '\0' || e == '\n')
			{
				tok.kind = TOK_BAD;
				return tok;
			}

			next(lex);
			if (e == 'n')
				ch = '\n';
			else if (e == 't')
				ch = '\t';
			else if (e == 'r')
				ch = '\r';
			else if (e == '\\')
				ch = '\\';
			else if (e == '"')
				ch = '"';
			else if (e == '\'')
				ch = '\'';
			else if (e == '$')
				ch = '$';
			else if (e == 'e')
				ch = '\x1b';
			else if (e == 'x')
			{
				char h1 = peek(lex);
				char h2;
				unsigned int v;
				int d1;
				int d2;

				if (!isxdigit((unsigned char)h1))
				{
					tok.kind = TOK_BAD;
					return tok;
				}

				next(lex);
				h2 = peek(lex);
				if (!isxdigit((unsigned char)h2))
				{
					tok.kind = TOK_BAD;
					return tok;
				}

				next(lex);
				d1 = h1 <= '9' ? h1 - '0'
					: (h1 | 32) - 'a' + 10;
				d2 = h2 <= '9' ? h2 - '0'
					: (h2 | 32) - 'a' + 10;
				v = (unsigned int)((d1 << 4) | d2);
				ch = (char)(unsigned char)v;
			}
			else
			{
				tok.kind = TOK_BAD;
				return tok;
			}
		}
		else if (quote == '"' && ch == '$' && peek(lex) == '{')
		{
			next(lex);
			if (lex->interp_sp >= STGE_MAX_INTERP)
			{
				tok.kind = TOK_BAD;
				return tok;
			}

			lex->interp_depth[lex->interp_sp++] = 0;
			tok_from_buf(&tok, lex);
			tok.hole = true;
			return tok;
		}

		if (!lex_buf_push(lex, ch))
		{
			tok.kind = TOK_BAD;
			return tok;
		}
	}

	tok_from_buf(&tok, lex);
	if (peek(lex) != quote)
	{
		tok.kind = TOK_BAD;
		return tok;
	}

	next(lex);
	if (tok.kind == TOK_CHAR && tok.len != 1)
	{
		tok.kind = TOK_BAD;
		return tok;
	}

	return tok;
}

static Tok lex_tok(Lex *lex)
{
	Tok tok;
	char c;

	if (lex->cont_string)
	{
		lex->cont_string = false;
		return lex_string(lex, '"');
	}

	memset(&tok, 0, sizeof tok);
	skip_space(lex);
	tok.line = lex->line;
	tok.start = lex->pos;
	tok.col = column_at(lex, lex->pos);
	c = peek(lex);

	switch (c)
	{
	case '\0':
		tok.kind = TOK_EOF;
		return tok;
	case '\n':
		next(lex);
		tok.kind = TOK_EOL;
		return tok;
	case '=':
		next(lex);
		if (peek(lex) == '=')
		{
			next(lex);
			tok.kind = TOK_EQEQ;
			return tok;
		}

		tok.kind = TOK_EQUAL;
		return tok;
	case '<':
		next(lex);
		if (peek(lex) == '=')
		{
			next(lex);
			tok.kind = TOK_LTE;
			return tok;
		}

		tok.kind = TOK_LT;
		return tok;
	case '>':
		next(lex);
		if (peek(lex) == '=')
		{
			next(lex);
			tok.kind = TOK_GTE;
			return tok;
		}

		tok.kind = TOK_GT;
		return tok;
	case '.':
		next(lex);
		tok.kind = TOK_DOT;
		return tok;
	case ':':
		next(lex);
		if (peek(lex) == ':')
		{
			next(lex);
			tok.kind = TOK_COLONCOLON;
			return tok;
		}

		tok.kind = TOK_BAD;
		return tok;
	case '(':
		next(lex);
		tok.kind = TOK_LPAREN;
		return tok;
	case ')':
		next(lex);
		tok.kind = TOK_RPAREN;
		return tok;
	case '{':
		next(lex);
		if (lex->interp_sp > 0)
			lex->interp_depth[lex->interp_sp - 1]++;
		tok.kind = TOK_LBRACE;
		return tok;
	case '}':
		next(lex);
		tok.kind = TOK_RBRACE;
		if (lex->interp_sp > 0)
		{
			int sp = lex->interp_sp - 1;

			if (lex->interp_depth[sp] == 0)
			{
				lex->interp_sp--;
				lex->cont_string = true;
				tok.closes_str = true;
			}
			else
				lex->interp_depth[sp]--;
		}
		return tok;
	case ',':
		next(lex);
		tok.kind = TOK_COMMA;
		return tok;
	case '[':
		next(lex);
		tok.kind = TOK_LBRACK;
		return tok;
	case ']':
		next(lex);
		tok.kind = TOK_RBRACK;
		return tok;
	case '+':
		next(lex);
		if (peek(lex) == '=')
		{
			next(lex);
			tok.kind = TOK_PLUSEQ;
			return tok;
		}

		tok.kind = TOK_PLUS;
		return tok;
	case '-':
		next(lex);
		if (peek(lex) == '>')
		{
			next(lex);
			tok.kind = TOK_ARROW;
			return tok;
		}

		if (peek(lex) == '=')
		{
			next(lex);
			tok.kind = TOK_MINUSEQ;
			return tok;
		}

		tok.kind = TOK_MINUS;
		return tok;
	case '*':
		next(lex);
		if (peek(lex) == '=')
		{
			next(lex);
			tok.kind = TOK_STAREQ;
			return tok;
		}

		tok.kind = TOK_STAR;
		return tok;
	case '/':
		next(lex);
		if (peek(lex) == '/')
		{
			while (peek(lex) != '\0' && peek(lex) != '\n')
				next(lex);
			return lex_tok(lex);
		}

		if (peek(lex) == '=')
		{
			next(lex);
			tok.kind = TOK_SLASHEQ;
			return tok;
		}

		tok.kind = TOK_SLASH;
		return tok;
	default:
		break;
	}

	if (c == '"' || c == '\'')
	{
		char quote = c;

		next(lex);
		return lex_string(lex, quote);
	}

	if (isdigit((unsigned char)c))
	{
		const char *start = lex->src + lex->pos;
		size_t p = lex->pos;
		char *end;

		while (p < lex->len && isdigit((unsigned char)lex->src[p]))
			p++;

		if (p + 1 < lex->len && lex->src[p] == '.' && isdigit((unsigned char)lex->src[p + 1]))
		{
			tok.kind = TOK_FLOAT;
			tok.dvalue = strtod(start, &end);
			lex->pos = (size_t)(end - lex->src);
			return tok;
		}

		tok.kind = TOK_INT;
		tok.value = (int32_t)strtol(start, &end, 10);
		lex->pos = (size_t)(end - lex->src);
		return tok;
	}

	if (isalpha((unsigned char)c) || c == '_')
	{
		tok.kind = TOK_IDENT;
		if (!take_word(lex, &tok))
			tok.kind = TOK_BAD;

		return tok;
	}

	tok.kind = TOK_BAD;
	if (lex_buf_clear(lex) && lex_buf_push(lex, c))
		tok_from_buf(&tok, lex);
	else
	{
		tok.text = "";
		tok.len = 0;
	}
	return tok;
}

/* ---- compiler state ---------------------------------------------------- */

typedef struct
{
	char name[STGE_NAME_MAX];
	int id;
	ObjFn *fns;
	int fn_count;
	int fns_cap;
	FieldType fields[STGE_MAX_FIELD_KINDS];
	int field_count;
	bool type_bound; /* claimed by compile Name = {} / packed / enum / extern */
} NodeRef;

typedef struct
{
	char name[STGE_NAME_MAX];
	int32_t value;
} NsConst;

typedef struct
{
	char name[STGE_NAME_MAX];
	int obj_id;
	ObjFn *fns;
	int fn_count;
	int fns_cap;
	NsConst consts[STGE_MAX_CONST];
	int const_count;
} NsRef;

typedef struct
{
	char name[STGE_NAME_MAX];
	int32_t index;
} PackedAlias;

typedef struct
{
	char name[STGE_NAME_MAX];
	char elem[STGE_NAME_MAX];
	int32_t len;
	FieldKind elem_kind;
	PackedAlias aliases[STGE_MAX_ALIASES];
	int alias_count;
} PackedType;

typedef struct
{
	const char *path;
	Lex lex;
	Tok tok;

	NodeRef *nodes;
	int node_count;
	int nodes_cap;
	int spawn_count;

	NsRef nss[STGE_MAX_NS];
	int ns_count;
	PackedType packed[STGE_MAX_PACKED];
	int packed_count;

	int self;
	char param[STGE_NAME_MAX];
	char param_type[STGE_NAME_MAX];
	bool has_param;
	char (*locals)[STGE_NAME_MAX];
	bool *local_growable;
	int local_count;
	int locals_cap;
	char (*slot_names)[STGE_NAME_MAX];
	FieldKind *slot_kinds;
	char (*slot_types)[STGE_NAME_MAX];
	int slot_count;
	int slots_cap;
	int fn_argc;
	int fn_self_param;
	char fn_ret_type[STGE_NAME_MAX];
	size_t call_patch_at[STGE_MAX_CALL_PATCH];
	int call_patch_obj[STGE_MAX_CALL_PATCH];
	char call_patch_fn[STGE_MAX_CALL_PATCH][STGE_NAME_MAX];
	int call_patch_count;
	bool file_add_child;
	int include_depth;
	bool failed;
	ExprKind expr_kind;
	char last_ret_type[STGE_NAME_MAX];
	AstUse used[STGE_MAX_USE];
	int used_count;
	char *name_scratch;
	size_t name_scratch_cap;
} Comp;

static bool ensure_fns_cap(ObjFn **fns, int *count, int *cap)
{
	ObjFn *grown;
	int ncap;

	if (*count < *cap)
		return true;

	ncap = *cap > 0 ? *cap * 2 : 8;
	grown = realloc(*fns, (size_t)ncap * sizeof *grown);
	if (grown == NULL)
		return false;
	memset(grown + *cap, 0, (size_t)(ncap - *cap) * sizeof *grown);
	*fns = grown;
	*cap = ncap;
	return true;
}

static bool ensure_nodes_cap(Comp *c)
{
	NodeRef *grown;
	int ncap;

	if (c->node_count < c->nodes_cap)
		return true;

	ncap = c->nodes_cap > 0 ? c->nodes_cap * 2 : 32;
	grown = realloc(c->nodes, (size_t)ncap * sizeof *grown);
	if (grown == NULL)
		return false;
	memset(grown + c->nodes_cap, 0, (size_t)(ncap - c->nodes_cap) * sizeof *grown);
	c->nodes = grown;
	c->nodes_cap = ncap;
	return true;
}

static bool ensure_slots_cap(Comp *c)
{
	void *a;
	void *b;
	void *d;
	int ncap;

	if (c->slot_count < c->slots_cap)
		return true;

	ncap = c->slots_cap > 0 ? c->slots_cap * 2 : 16;
	a = realloc(c->slot_names, (size_t)ncap * sizeof *c->slot_names);
	b = realloc(c->slot_kinds, (size_t)ncap * sizeof *c->slot_kinds);
	d = realloc(c->slot_types, (size_t)ncap * sizeof *c->slot_types);
	if (a == NULL || b == NULL || d == NULL)
	{
		free(a == c->slot_names ? NULL : a);
		free(b == c->slot_kinds ? NULL : b);
		free(d == c->slot_types ? NULL : d);
		return false;
	}
	c->slot_names = a;
	c->slot_kinds = b;
	c->slot_types = d;
	memset(c->slot_names + c->slots_cap, 0,
	       (size_t)(ncap - c->slots_cap) * sizeof *c->slot_names);
	memset(c->slot_kinds + c->slots_cap, 0,
	       (size_t)(ncap - c->slots_cap) * sizeof *c->slot_kinds);
	memset(c->slot_types + c->slots_cap, 0,
	       (size_t)(ncap - c->slots_cap) * sizeof *c->slot_types);
	c->slots_cap = ncap;
	return true;
}

static bool ensure_locals_cap(Comp *c)
{
	void *a;
	void *b;
	int ncap;

	if (c->local_count < c->locals_cap)
		return true;

	ncap = c->locals_cap > 0 ? c->locals_cap * 2 : 16;
	a = realloc(c->locals, (size_t)ncap * sizeof *c->locals);
	b = realloc(c->local_growable, (size_t)ncap * sizeof *c->local_growable);
	if (a == NULL || b == NULL)
	{
		free(a == c->locals ? NULL : a);
		free(b == c->local_growable ? NULL : b);
		return false;
	}
	c->locals = a;
	c->local_growable = b;
	memset(c->locals + c->locals_cap, 0,
	       (size_t)(ncap - c->locals_cap) * sizeof *c->locals);
	memset(c->local_growable + c->locals_cap, 0,
	       (size_t)(ncap - c->locals_cap) * sizeof *c->local_growable);
	c->locals_cap = ncap;
	return true;
}

static void comp_free_tables(Comp *c)
{
	int i;

	if (c == NULL)
		return;

	for (i = 0; i < c->node_count; i++)
		free(c->nodes[i].fns);
	free(c->nodes);
	c->nodes = NULL;
	for (i = 0; i < c->ns_count; i++)
		free(c->nss[i].fns);
	free(c->locals);
	free(c->local_growable);
	free(c->slot_names);
	free(c->slot_kinds);
	free(c->slot_types);
	c->locals = NULL;
	c->local_growable = NULL;
	c->slot_names = NULL;
	c->slot_kinds = NULL;
	c->slot_types = NULL;
}

/* NUL-terminated view of tok text; valid until the next tok_cstr call. */
static const char *tok_cstr(Comp *c, const Tok *tok)
{
	size_t n;

	if (c == NULL || tok == NULL || tok->text == NULL)
		return "";

	n = tok->len;
	if (n + 1 > c->name_scratch_cap)
	{
		char *grown = realloc(c->name_scratch, n + 1);

		if (grown == NULL)
			return "";

		c->name_scratch = grown;
		c->name_scratch_cap = n + 1;
	}

	memcpy(c->name_scratch, tok->text, n);
	c->name_scratch[n] = '\0';
	return c->name_scratch;
}

static bool g_parser_error;

bool parser_had_error(void)
{
	return g_parser_error;
}

static const char *tok_label(const Tok *tok)
{
	static char buf[96];
	const char *text;
	int n;

	if (tok == NULL)
		return "end of file";

	text = tok->text != NULL ? tok->text : "";
	n = tok->len > 64 ? 64 : (int)tok->len;
	switch (tok->kind)
	{
	case TOK_IDENT:
		snprintf(buf, sizeof buf, "name '%.*s'", n, text);
		return buf;
	case TOK_STRING:
		snprintf(buf, sizeof buf, "string \"%.*s\"", n, text);
		return buf;
	case TOK_INT:
		snprintf(buf, sizeof buf, "number %d", (int)tok->value);
		return buf;
	case TOK_FLOAT:
		return "a number";
	case TOK_EQUAL:
		return "'='";
	case TOK_EQEQ:
		return "'=='";
	case TOK_LT:
		return "'<'";
	case TOK_LTE:
		return "'<='";
	case TOK_GT:
		return "'>'";
	case TOK_GTE:
		return "'>='";
	case TOK_DOT:
		return "'.'";
	case TOK_COLONCOLON:
		return "'::'";
	case TOK_LPAREN:
		return "'('";
	case TOK_RPAREN:
		return "')'";
	case TOK_LBRACE:
		return "'{'";
	case TOK_RBRACE:
		return "'}'";
	case TOK_COMMA:
		return "','";
	case TOK_LBRACK:
		return "'['";
	case TOK_RBRACK:
		return "']'";
	case TOK_PLUS:
		return "'+'";
	case TOK_PLUSEQ:
		return "'+='";
	case TOK_MINUS:
		return "'-'";
	case TOK_MINUSEQ:
		return "'-='";
	case TOK_ARROW:
		return "'->'";
	case TOK_STAR:
		return "'*'";
	case TOK_STAREQ:
		return "'*='";
	case TOK_SLASH:
		return "'/'";
	case TOK_SLASHEQ:
		return "'/='";
	case TOK_EOL:
		return "end of line";
	case TOK_EOF:
		return "end of file";
	case TOK_BAD:
		if (tok->text[0] != '\0')
		{
			snprintf(buf, sizeof buf, "'%s'", tok->text);
			return buf;
		}
		return "an invalid token";
	default:
		return "a token";
	}
}

static void print_fail_caret(const Comp *c)
{
	const char *src;
	size_t i;
	size_t start;
	size_t end;
	int col;

	if (c == NULL || c->lex.src == NULL)
		return;

	src = c->lex.src;
	i = c->tok.start;
	if (i > c->lex.len)
		i = c->lex.len;

	while (i > 0 && src[i - 1] != '\n')
		i--;
	start = i;
	while (i < c->lex.len && src[i] != '\n')
		i++;
	end = i;

	fputc(' ', stderr);
	for (i = start; i < end; i++)
		fputc(src[i] == '\t' ? ' ' : src[i], stderr);
	fputc('\n', stderr);

	col = c->tok.col > 0 ? c->tok.col : 1;
	fputc(' ', stderr);
	for (i = 1; (int)i < col; i++)
		fputc(' ', stderr);
	fputc('^', stderr);
	fputc('\n', stderr);
}

static void fail(Comp *c, const char *msg)
{
	const char *path;
	int line;
	int col;

	if (c == NULL || c->failed)
		return;

	g_parser_error = true;
	c->failed = true;
	path = c->path != NULL ? c->path : "script";
	line = c->tok.line > 0 ? c->tok.line : 1;
	col = c->tok.col > 0 ? c->tok.col : 1;
	fprintf(stderr, "stge: %s:%d:%d: %s\n", path, line, col, msg != NULL ? msg : "parse error");
	print_fail_caret(c);
}

static void failf(Comp *c, const char *fmt, ...)
{
	char msg[256];
	va_list ap;

	if (c == NULL || c->failed)
		return;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof msg, fmt, ap);
	va_end(ap);
	fail(c, msg);
}

static void advance(Comp *c)
{
	c->tok = lex_tok(&c->lex);
	if (c->tok.kind == TOK_BAD)
		failf(c, "invalid token, found %s", tok_label(&c->tok));
}

static bool at(const Comp *c, TokKind kind)
{
	return c->tok.kind == kind;
}

static void expect(Comp *c, TokKind kind, const char *msg)
{
	if (c->failed)
		return;

	if (!at(c, kind))
	{
		failf(c, "%s, found %s", msg, tok_label(&c->tok));
		return;
	}

	advance(c);
}

static void skip_eol(Comp *c)
{
	while (!c->failed && at(c, TOK_EOL))
		advance(c);
}

static bool name_eq(const char *a, const char *b)
{
	if (a == NULL || b == NULL)
		return a == b;

	while (*a != '\0' && *b != '\0')
	{
		if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
			return false;
		a++;
		b++;
	}

	return *a == *b;
}

static NodeRef *find_node(Comp *c, const char *name)
{
	int i;

	for (i = 0; i < c->node_count; i++)
	{
		if (name_eq(c->nodes[i].name, name))
			return &c->nodes[i];
	}

	return NULL;
}

static const PackedType *find_packed(const Comp *c, const char *name)
{
	int i;

	if (c == NULL || name == NULL || name[0] == '\0')
		return NULL;

	for (i = 0; i < c->packed_count; i++)
	{
		if (name_eq(c->packed[i].name, name))
			return &c->packed[i];
	}

	return NULL;
}

static PackedType *ensure_packed(Comp *c, const char *name)
{
	PackedType *p = (PackedType *)find_packed(c, name);

	if (p != NULL)
		return p;
	if (c == NULL || name == NULL || name[0] == '\0'
	    || c->packed_count >= STGE_MAX_PACKED)
		return NULL;

	p = &c->packed[c->packed_count++];
	memset(p, 0, sizeof *p);
	snprintf(p->name, sizeof p->name, "%s", name);
	p->elem_kind = FK_INT;
	return p;
}

static int packed_alias_index(const PackedType *p, const char *comp)
{
	int i;

	if (p == NULL || comp == NULL)
		return -1;

	for (i = 0; i < p->alias_count; i++)
	{
		if (strcmp(p->aliases[i].name, comp) == 0)
			return (int)p->aliases[i].index;
	}

	return -1;
}

static bool packed_is_composite(const PackedType *p)
{
	return p != NULL && p->alias_count > 0;
}

static void packed_add_alias(PackedType *p, const char *comp, int32_t index)
{
	if (p == NULL || comp == NULL || p->alias_count >= STGE_MAX_ALIASES)
		return;
	if (packed_alias_index(p, comp) >= 0)
		return;

	snprintf(p->aliases[p->alias_count].name,
		 sizeof p->aliases[0].name, "%s", comp);
	p->aliases[p->alias_count].index = index;
	p->alias_count++;
}

static Op packed_new_op(FieldKind k)
{
	if (k == FK_UINT)
		return OP_NEW_UINT;
	if (k == FK_BYTE)
		return OP_NEW_BYTE;
	if (k == FK_FLOAT)
		return OP_NEW_FLOAT;
	if (k == FK_DOUBLE)
		return OP_NEW_DOUBLE;
	if (k == FK_STR)
		return OP_NEW_STR;
	return OP_NEW_INT;
}

static FieldKind lookup_field_kind(const Comp *c, const char *node, const char *field)
{
	const NodeRef *n = find_node((Comp *)c, node);
	int i;

	if (n == NULL)
		return FK_NONE;

	for (i = 0; i < n->field_count; i++)
	{
		if (strcmp(n->fields[i].name, field) == 0)
			return n->fields[i].kind;
	}

	return FK_NONE;
}

static void record_field_kind(Comp *c, const char *node, const char *field, FieldKind kind)
{
	NodeRef *n = find_node(c, node);
	int i;

	if (n == NULL || field == NULL)
		return;

	for (i = 0; i < n->field_count; i++)
	{
		if (strcmp(n->fields[i].name, field) == 0)
		{
			n->fields[i].kind = kind;
			n->fields[i].arr_len = 1;
			n->fields[i].growable = false;
			n->fields[i].type[0] = '\0';
			return;
		}
	}

	if (n->field_count >= STGE_MAX_FIELD_KINDS)
		return;

	strcpy(n->fields[n->field_count].name, field);
	n->fields[n->field_count].kind = kind;
	n->fields[n->field_count].arr_len = 1;
	n->fields[n->field_count].growable = false;
	n->fields[n->field_count].type[0] = '\0';
	n->field_count++;
}

static void record_field_obj(Comp *c, const char *node, const char *field, const char *type)
{
	NodeRef *n = find_node(c, node);
	int i;

	if (n == NULL || field == NULL)
		return;

	record_field_kind(c, node, field, FK_OBJ);
	for (i = 0; i < n->field_count; i++)
	{
		if (strcmp(n->fields[i].name, field) == 0)
		{
			if (type != NULL)
				snprintf(n->fields[i].type, sizeof n->fields[i].type, "%s", type);
			return;
		}
	}
}

static const char *lookup_field_type(Comp *c, const char *node, const char *field)
{
	const NodeRef *n = find_node(c, node);
	int i;

	if (n == NULL || field == NULL)
		return "";

	for (i = 0; i < n->field_count; i++)
	{
		if (strcmp(n->fields[i].name, field) == 0)
			return n->fields[i].type;
	}

	return "";
}

static void record_field_array(Comp *c, const char *node, const char *field,
			       FieldKind kind, int32_t arr_len, bool growable)
{
	NodeRef *n = find_node(c, node);
	int i;

	if (n == NULL || field == NULL)
		return;

	for (i = 0; i < n->field_count; i++)
	{
		if (strcmp(n->fields[i].name, field) == 0)
		{
			n->fields[i].kind = kind;
			n->fields[i].arr_len = arr_len;
			n->fields[i].growable = growable;
			return;
		}
	}

	if (n->field_count >= STGE_MAX_FIELD_KINDS)
		return;

	strcpy(n->fields[n->field_count].name, field);
	n->fields[n->field_count].kind = kind;
	n->fields[n->field_count].arr_len = arr_len;
	n->fields[n->field_count].growable = growable;
	n->fields[n->field_count].type[0] = '\0';
	n->field_count++;
}

static void record_field_packed(Comp *c, const char *node, const char *field,
				const PackedType *p)
{
	NodeRef *n;
	int i;

	if (p == NULL)
		return;

	record_field_array(c, node, field, p->elem_kind, p->len, false);
	n = find_node(c, node);
	if (n == NULL)
		return;

	for (i = 0; i < n->field_count; i++)
	{
		if (strcmp(n->fields[i].name, field) == 0)
		{
			snprintf(n->fields[i].type, sizeof n->fields[i].type, "%s", p->name);
			return;
		}
	}
}

static int32_t lookup_field_arr_len(const Comp *c, const char *node, const char *field)
{
	const NodeRef *n = find_node((Comp *)c, node);
	int i;

	if (n == NULL)
		return 1;

	for (i = 0; i < n->field_count; i++)
	{
		if (strcmp(n->fields[i].name, field) == 0)
			return n->fields[i].arr_len;
	}

	return 1;
}

static bool lookup_field_growable(const Comp *c, const char *node, const char *field)
{
	const NodeRef *n = find_node((Comp *)c, node);
	int i;

	if (n == NULL)
		return false;

	for (i = 0; i < n->field_count; i++)
	{
		if (strcmp(n->fields[i].name, field) == 0)
			return n->fields[i].growable;
	}

	return false;
}

static bool field_is_vec(FieldKind k)
{
	(void)k;
	return false;
}

static int vec_len(FieldKind k)
{
	(void)k;
	return 0;
}

static Op mem_get_op(FieldKind k)
{
	if (k == FK_FLOAT)
		return OP_GET_MEM_F;
	if (k == FK_DOUBLE)
		return OP_GET_MEM_D;
	if (k == FK_STR)
		return OP_GET_MEM_STR;
	return OP_GET_MEM;
}

static Op mem_set_op(FieldKind k)
{
	if (k == FK_FLOAT)
		return OP_SET_MEM_F;
	if (k == FK_DOUBLE)
		return OP_SET_MEM_D;
	if (k == FK_STR)
		return OP_SET_MEM_STR;
	if (k == FK_OBJ)
		return OP_SET_OBJ;
	return OP_SET_MEM;
}

static ExprKind field_expr_kind(FieldKind k)
{
	if (k == FK_FLOAT)
		return EK_FLOAT;
	if (k == FK_DOUBLE)
		return EK_DOUBLE;
	if (k == FK_STR)
		return EK_STR;
	if (k == FK_OBJ)
		return EK_OBJ;
	if (k == FK_NONE)
		return EK_INT;
	return EK_INT;
}

static NsRef *find_ns(Comp *c, const char *name)
{
	int i;

	for (i = 0; i < c->ns_count; i++)
	{
		if (name_eq(c->nss[i].name, name))
			return &c->nss[i];
	}

	return NULL;
}

static NsRef *ensure_ns(Comp *c, const char *name)
{
	NsRef *ns = find_ns(c, name);

	if (ns != NULL)
		return ns;

	if (c->ns_count >= STGE_MAX_NS)
	{
		fail(c, "too many namespaces");
		return NULL;
	}

	ns = &c->nss[c->ns_count++];
	memset(ns, 0, sizeof *ns);
	strcpy(ns->name, name);
	ns->obj_id = -1;
	return ns;
}

static NodeRef *ensure_node(Comp *c, const char *name);
static bool join_ns(char *dst, size_t dst_size, const char *a, const char *b);

static NodeRef *add_node(Comp *c, const char *name)
{
	NodeRef *node;
	NsRef *ns;

	if (find_node(c, name) != NULL)
	{
		failf(c, "name '%s' is already defined", name);
		return NULL;
	}

	if (!ensure_nodes_cap(c))
	{
		fail(c, "out of memory");
		return NULL;
	}

	node = &c->nodes[c->node_count];
	memset(node, 0, sizeof *node);
	strcpy(node->name, name);
	{
		int tindex = stge_type_register(name);

		if (tindex < 0)
		{
			fail(c, "could not create type");
			return NULL;
		}

		node->id = stge_type_ref(tindex);
	}

	c->spawn_count = c->node_count + 1;
	c->node_count++;

	ns = ensure_ns(c, name);
	if (ns != NULL)
		ns->obj_id = node->id;

	return node;
}

static NodeRef *ensure_node(Comp *c, const char *name)
{
	NodeRef *n = find_node(c, name);

	if (n != NULL)
		return n;

	return add_node(c, name);
}

/* First compile-time type bind for name; second Name = ... fails. */
static NodeRef *bind_type_node(Comp *c, const char *name)
{
	NodeRef *n = find_node(c, name);

	if (n != NULL && n->type_bound)
	{
		failf(c, "name '%s' is already defined", name);
		return NULL;
	}

	n = ensure_node(c, name);
	if (n == NULL)
		return NULL;

	n->type_bound = true;
	return n;
}

static bool join_into(char *dst, size_t dst_size, const char *a, const char *b)
{
	char tmp[STGE_NAME_MAX * 2];

	if (!join_ns(tmp, sizeof tmp, a, b))
		return false;

	if (strlen(tmp) >= dst_size)
		return false;

	strcpy(dst, tmp);
	return true;
}

static bool take_qname(Comp *c, char *out, size_t cap)
{
	if (!at(c, TOK_IDENT))
		return false;

	if (c->tok.len >= cap)
		return false;

	tok_copy(out, cap, &c->tok);
	advance(c);
	while (!c->failed && at(c, TOK_COLONCOLON))
	{
		advance(c);
		if (!at(c, TOK_IDENT))
			return false;

		if (!join_into(out, cap, out, tok_cstr(c, &c->tok)))
			return false;

		advance(c);
	}

	return !c->failed;
}

static bool take_colon_path(Comp *c, const char *head, char *qname, size_t qn,
			    char *last, size_t ln)
{
	if (head == NULL || strlen(head) >= qn)
		return false;

	strcpy(qname, head);
	last[0] = '\0';
	if (!at(c, TOK_COLONCOLON))
		return false;

	while (!c->failed && at(c, TOK_COLONCOLON))
	{
		advance(c);
		if (at(c, TOK_PLUSEQ) || at(c, TOK_MINUSEQ)
		    || at(c, TOK_STAREQ) || at(c, TOK_SLASHEQ))
		{
			const char *op = at(c, TOK_PLUSEQ) ? "+="
				: at(c, TOK_MINUSEQ) ? "-="
				: at(c, TOK_STAREQ) ? "*=" : "/=";

			if (strlen(op) >= ln)
			{
				fail(c, "name too long");
				return false;
			}

			strcpy(last, op);
			if (!join_into(qname, qn, qname, last))
			{
				fail(c, "name too long");
				return false;
			}

			advance(c);
			break;
		}

		if (!at(c, TOK_IDENT))
		{
			fail(c, "expected a name");
			return false;
		}

		if (c->tok.len >= ln)
		{
			fail(c, "name too long");
			return false;
		}

		tok_copy(last, ln, &c->tok);
		if (!join_into(qname, qn, qname, last))
		{
			fail(c, "name too long");
			return false;
		}

		advance(c);
	}

	return !c->failed;
}

static bool qual_parent(const char *qname, char *parent, size_t n)
{
	const char *sep = NULL;
	const char *p;
	size_t len;

	if (qname == NULL || parent == NULL)
		return false;

	for (p = qname; *p != '\0'; p++)
	{
		if (p[0] == ':' && p[1] == ':')
			sep = p;
	}

	if (sep == NULL)
		return false;

	len = (size_t)(sep - qname);
	if (len >= n)
		return false;

	memcpy(parent, qname, len);
	parent[len] = '\0';
	return true;
}

static bool ns_add_fn_arity(NsRef *ns, const char *name, int pc, bool has_arg, int argc,
			    const char *ret_type);
static bool ns_set_fn_arity(NsRef *ns, const char *name, int pc, bool has_arg, int argc,
			    const char *ret_type);

static const ObjFn *ns_find_fn(const NsRef *ns, const char *name)
{
	int i;

	if (ns == NULL)
		return NULL;

	for (i = 0; i < ns->fn_count; i++)
	{
		if (strcmp(ns->fns[i].name, name) == 0)
			return &ns->fns[i];
	}

	return NULL;
}

static const ObjFn *ns_find_fn_arity(const NsRef *ns, const char *name, int argc)
{
	int i;

	if (ns == NULL || name == NULL)
		return NULL;

	for (i = 0; i < ns->fn_count; i++)
	{
		if (strcmp(ns->fns[i].name, name) == 0 && ns->fns[i].argc == argc)
			return &ns->fns[i];
	}

	return NULL;
}

static bool ns_add_fn(NsRef *ns, const char *name, int pc, bool has_arg)
{
	return ns_add_fn_arity(ns, name, pc, has_arg, has_arg ? 1 : 0, NULL);
}

static bool ns_add_fn_arity(NsRef *ns, const char *name, int pc, bool has_arg, int argc,
			    const char *ret_type)
{
	if (ns == NULL || !ensure_fns_cap(&ns->fns, &ns->fn_count, &ns->fns_cap))
		return false;

	memset(&ns->fns[ns->fn_count], 0, sizeof ns->fns[0]);
	strcpy(ns->fns[ns->fn_count].name, name);
	ns->fns[ns->fn_count].pc = pc;
	ns->fns[ns->fn_count].has_arg = has_arg;
	ns->fns[ns->fn_count].argc = argc;
	ns->fns[ns->fn_count].self_param = -1;
	if (ret_type != NULL && ret_type[0] != '\0')
		snprintf(ns->fns[ns->fn_count].ret_type,
			 sizeof ns->fns[ns->fn_count].ret_type, "%s", ret_type);
	ns->fn_count++;
	return true;
}

static bool ns_set_fn_arity(NsRef *ns, const char *name, int pc, bool has_arg, int argc,
			    const char *ret_type)
{
	int i;

	if (ns == NULL)
		return false;

	for (i = 0; i < ns->fn_count; i++)
	{
		if (strcmp(ns->fns[i].name, name) == 0 && ns->fns[i].argc == argc)
		{
			if (ns->fns[i].is_extern)
				return false;

			if (ns->fns[i].pc >= 0 && strcmp(name, "new") != 0)
				return false;

			ns->fns[i].pc = pc;
			ns->fns[i].has_arg = has_arg;
			ns->fns[i].argc = argc;
			if (ret_type != NULL && ret_type[0] != '\0')
				snprintf(ns->fns[i].ret_type, sizeof ns->fns[i].ret_type,
					 "%s", ret_type);
			else
				ns->fns[i].ret_type[0] = '\0';
			return true;
		}
	}

	return ns_add_fn_arity(ns, name, pc, has_arg, argc, ret_type);
}

static int peek_argc(const Comp *c)
{
	Lex lex;
	Tok t;
	int depth = 1;
	int argc = 1;

	if (!at(c, TOK_LPAREN))
		return 0;

	lex = lex_probe(&c->lex);
	t = lex_tok(&lex);
	if (t.kind == TOK_RPAREN || t.kind == TOK_EOF)
	{
		lex_probe_free(&lex);
		return 0;
	}

	while (t.kind != TOK_EOF && depth > 0)
	{
		if (t.kind == TOK_LPAREN)
			depth++;
		else if (t.kind == TOK_RPAREN)
			depth--;
		else if (t.kind == TOK_COMMA && depth == 1)
			argc++;
		t = lex_tok(&lex);
	}

	lex_probe_free(&lex);
	return argc;
}

static void record_fn_name(Comp *c, const char *owner, const char *fn, int argc)
{
	NsRef *ns;
	NodeRef *node;
	int i;
	bool has_arg = argc > 0;

	if (c == NULL || owner == NULL || fn == NULL)
		return;

	ns = ensure_ns(c, owner);
	if (ns != NULL && ns_find_fn_arity(ns, fn, argc) == NULL)
		ns_add_fn_arity(ns, fn, -1, has_arg, argc, NULL);

	node = find_node(c, owner);
	if (node == NULL)
		return;

	for (i = 0; i < node->fn_count; i++)
	{
		if (strcmp(node->fns[i].name, fn) == 0 && node->fns[i].argc == argc)
		{
			node->fns[i].has_arg = has_arg;
			node->fns[i].argc = argc;
			return;
		}
	}

	if (!ensure_fns_cap(&node->fns, &node->fn_count, &node->fns_cap))
		return;

	memset(&node->fns[node->fn_count], 0, sizeof node->fns[0]);
	memcpy(node->fns[node->fn_count].name, fn, sizeof node->fns[0].name);
	node->fns[node->fn_count].pc = -1;
	node->fns[node->fn_count].has_arg = has_arg;
	node->fns[node->fn_count].argc = argc;
	node->fns[node->fn_count].self_param = -1;
	node->fn_count++;
}

static bool ns_add_extern(NsRef *ns, const char *name, int native_id, bool has_arg)
{
	if (ns == NULL || !ensure_fns_cap(&ns->fns, &ns->fn_count, &ns->fns_cap))
		return false;

	memset(&ns->fns[ns->fn_count], 0, sizeof ns->fns[0]);
	strcpy(ns->fns[ns->fn_count].name, name);
	ns->fns[ns->fn_count].pc = -2;
	ns->fns[ns->fn_count].has_arg = has_arg;
	ns->fns[ns->fn_count].argc = has_arg ? 1 : 0;
	ns->fns[ns->fn_count].self_param = -1;
	ns->fns[ns->fn_count].is_extern = true;
	ns->fns[ns->fn_count].native_id = native_id;
	ns->fn_count++;
	return true;
}

static bool ns_add_const(NsRef *ns, const char *name, int32_t value)
{
	int i;

	if (ns == NULL || name == NULL || ns->const_count >= STGE_MAX_CONST)
		return false;

	for (i = 0; i < ns->const_count; i++)
	{
		if (strcmp(ns->consts[i].name, name) == 0)
		{
			ns->consts[i].value = value;
			return true;
		}
	}

	memset(&ns->consts[ns->const_count], 0, sizeof ns->consts[0]);
	strcpy(ns->consts[ns->const_count].name, name);
	ns->consts[ns->const_count].value = value;
	ns->const_count++;
	return true;
}

static const NsConst *ns_find_const(const NsRef *ns, const char *name)
{
	int i;

	if (ns == NULL || name == NULL)
		return NULL;

	for (i = 0; i < ns->const_count; i++)
	{
		if (strcmp(ns->consts[i].name, name) == 0)
			return &ns->consts[i];
	}

	return NULL;
}

static const NsConst *find_enum_const(const Comp *c, const char *name)
{
	int i;

	if (c == NULL || name == NULL)
		return NULL;

	for (i = 0; i < c->ns_count; i++)
	{
		const NsConst *cn = ns_find_const(&c->nss[i], name);

		if (cn != NULL)
			return cn;
	}

	return NULL;
}

static bool is_local(const Comp *c, const char *name)
{
	int i;

	if (c == NULL || name == NULL)
		return false;

	for (i = 0; i < c->local_count; i++)
	{
		if (strcmp(c->locals[i], name) == 0)
			return true;
	}

	return false;
}

static bool is_local_buf(const Comp *c, const char *name)
{
	int i;

	if (c == NULL || name == NULL)
		return false;

	for (i = 0; i < c->local_count; i++)
	{
		if (strcmp(c->locals[i], name) == 0)
			return c->local_growable[i];
	}

	return false;
}

static bool add_local_kind(Comp *c, const char *name, bool growable)
{
	int i;

	if (c == NULL || name == NULL || name[0] == '\0')
		return false;

	for (i = 0; i < c->local_count; i++)
	{
		if (strcmp(c->locals[i], name) == 0)
		{
			c->local_growable[i] = growable;
			return true;
		}
	}

	if (!ensure_locals_cap(c))
		return false;

	strcpy(c->locals[c->local_count], name);
	c->local_growable[c->local_count] = growable;
	c->local_count++;
	return true;
}

static int find_slot_local(const Comp *c, const char *name)
{
	int i;

	if (c == NULL || name == NULL)
		return -1;

	for (i = 0; i < c->slot_count; i++)
	{
		if (c->slot_names[i][0] != '\0' && strcmp(c->slot_names[i], name) == 0)
			return i;
	}

	return -1;
}

static bool name_in_scope(const Comp *c, const char *name)
{
	if (c == NULL || name == NULL || name[0] == '\0')
		return false;

	if (strcmp(name, "self") == 0)
		return true;

	if (c->has_param && strcmp(name, c->param) == 0)
		return true;

	if (find_slot_local(c, name) >= 0 || is_local(c, name))
		return true;

	if (find_node((Comp *)c, name) != NULL || find_ns((Comp *)c, name) != NULL)
		return true;

	return false;
}

static int add_slot_local(Comp *c, const char *name, FieldKind kind)
{
	int i;

	if (c == NULL || name == NULL || name[0] == '\0')
		return -1;

	i = find_slot_local(c, name);
	if (i >= 0)
	{
		c->slot_kinds[i] = kind;
		return i;
	}

	if (!ensure_slots_cap(c))
		return -1;

	strcpy(c->slot_names[c->slot_count], name);
	c->slot_kinds[c->slot_count] = kind;
	c->slot_types[c->slot_count][0] = '\0';
	c->slot_count++;
	return c->slot_count - 1;
}

static void set_slot_type(Comp *c, int slot, const char *type)
{
	if (c == NULL || slot < 0 || slot >= c->slot_count)
		return;
	if (type == NULL || type[0] == '\0')
		c->slot_types[slot][0] = '\0';
	else
		snprintf(c->slot_types[slot], sizeof c->slot_types[slot], "%s", type);
}

static void apply_slot_type(Comp *c, int slot)
{
	if (c == NULL || slot < 0 || slot >= c->slot_count)
		return;
	if (c->slot_types[slot][0] != '\0')
		snprintf(c->last_ret_type, sizeof c->last_ret_type, "%s",
			 c->slot_types[slot]);
}

static void apply_param_type(Comp *c)
{
	if (c != NULL && c->param_type[0] != '\0')
		snprintf(c->last_ret_type, sizeof c->last_ret_type, "%s",
			 c->param_type);
}

static bool join_ns(char *dst, size_t dst_size, const char *a, const char *b)
{
	size_t na;
	size_t nb;

	if (dst == NULL || a == NULL || b == NULL)
		return false;

	na = strlen(a);
	nb = strlen(b);
	if (na + 2 + nb + 1 > dst_size)
		return false;

	memcpy(dst, a, na);
	dst[na] = ':';
	dst[na + 1] = ':';
	memcpy(dst + na + 2, b, nb + 1);
	return true;
}

/* ---- emit -------------------------------------------------------------- */

static void emit_u8(Comp *c, uint8_t b)
{
	if (c->failed)
		return;

	if (!stge_code_emit(&b, 1))
		fail(c, "out of code memory");
}

static void emit_u16(Comp *c, uint16_t v)
{
	emit_u8(c, (uint8_t)(v & 0xff));
	emit_u8(c, (uint8_t)(v >> 8));
}

static void emit_call(Comp *c, Op op, int argc)
{
	if (argc < 0)
		argc = 0;
	if (argc > 65535)
	{
		fail(c, "too many arguments");
		return;
	}

	emit_u8(c, (uint8_t)op);
	emit_u16(c, (uint16_t)argc);
}

static void emit_get_local(Comp *c, int slot)
{
	emit_u8(c, OP_GET_LOCAL);
	emit_u16(c, (uint16_t)slot);
}

static void emit_set_local(Comp *c, int slot)
{
	emit_u8(c, OP_SET_LOCAL);
	emit_u16(c, (uint16_t)slot);
}

static void emit_push_arg(Comp *c, int i)
{
	emit_u8(c, OP_PUSH_ARG);
	emit_u16(c, (uint16_t)i);
}

static void emit_extern(Comp *c, int native_id, int argc)
{
	if (argc < 0)
		argc = 0;
	if (argc > 65535)
	{
		fail(c, "too many arguments");
		return;
	}

	emit_u8(c, OP_EXTERN);
	emit_u8(c, (uint8_t)native_id);
	emit_u16(c, (uint16_t)argc);
}

static void emit_i32(Comp *c, int32_t v)
{
	uint32_t u = (uint32_t)v;

	emit_u8(c, (uint8_t)(u & 0xff));
	emit_u8(c, (uint8_t)((u >> 8) & 0xff));
	emit_u8(c, (uint8_t)((u >> 16) & 0xff));
	emit_u8(c, (uint8_t)((u >> 24) & 0xff));
}

static void emit_push(Comp *c, int32_t v)
{
	emit_u8(c, OP_PUSH);
	emit_i32(c, v);
}

static void emit_extern_getter(Comp *c, const ObjFn *sf)
{
	if (sf == NULL)
		return;

	if (sf->native_id == stge_native_find("screen_size"))
	{
		emit_u8(c, OP_GET_WIDTH);
		c->expr_kind = EK_INT;
		return;
	}

	if (sf->native_id == stge_native_find("screen_buf"))
	{
		emit_u8(c, OP_GET_SCREEN_BUF);
		c->expr_kind = EK_INT;
		return;
	}

	emit_extern(c, sf->native_id, 0);
	c->expr_kind = EK_INT;
}

static void emit_push_f(Comp *c, float f)
{
	uint32_t u;

	memcpy(&u, &f, sizeof u);
	emit_u8(c, OP_PUSH_F);
	emit_i32(c, (int32_t)u);
}

static void emit_push_d(Comp *c, double d)
{
	uint64_t u;
	uint32_t lo;
	uint32_t hi;

	memcpy(&u, &d, sizeof u);
	lo = (uint32_t)u;
	hi = (uint32_t)(u >> 32);
	emit_u8(c, OP_PUSH_D);
	emit_i32(c, (int32_t)lo);
	emit_i32(c, (int32_t)hi);
}

static void emit_coerce(Comp *c, ExprKind from, ExprKind to)
{
	if (c->failed || from == to)
		return;

	if (to == EK_FLOAT)
	{
		if (from == EK_INT)
			emit_u8(c, OP_I2F);
		else if (from == EK_DOUBLE)
			emit_u8(c, OP_D2F);
		return;
	}

	if (to == EK_DOUBLE)
	{
		if (from == EK_INT)
			emit_u8(c, OP_I2D);
		else if (from == EK_FLOAT)
			emit_u8(c, OP_F2D);
		return;
	}

	if (to == EK_INT)
	{
		if (from == EK_FLOAT)
			emit_u8(c, OP_F2I);
		else if (from == EK_DOUBLE)
			emit_u8(c, OP_D2I);
	}
}

static ExprKind promote_kind(ExprKind a, ExprKind b)
{
	if (a == EK_DOUBLE || b == EK_DOUBLE)
		return EK_DOUBLE;
	if (a == EK_FLOAT || b == EK_FLOAT)
		return EK_FLOAT;
	return EK_INT;
}

static void emit_push_str(Comp *c, const char *s)
{
	int idx;

	if (c->failed)
		return;

	idx = stge_intern(s);
	if (idx < 0)
	{
		fail(c, "too many strings");
		return;
	}

	emit_u8(c, OP_PUSH_STR);
	emit_u16(c, (uint16_t)idx);
}

static void emit_push_node(Comp *c, int id)
{
	emit_u8(c, OP_PUSH_NODE);
	emit_i32(c, (int32_t)id);
}

static size_t emit_jump(Comp *c, Op op)
{
	size_t at_operand;

	emit_u8(c, op);
	at_operand = stge_prog() != NULL ? stge_prog()->code_len : 0;
	emit_u16(c, 0);
	return at_operand;
}

static void patch_jump(Comp *c, size_t operand)
{
	VmProg *s;
	long off;

	if (c->failed)
		return;

	s = stge_prog();
	if (s == NULL)
	{
		fail(c, "no compile context");
		return;
	}

	off = (long)s->code_len - (long)(operand + 2);
	if (off < -32768 || off > 32767)
	{
		fail(c, "body too large");
		return;
	}

	s->code[operand] = (uint8_t)((uint16_t)off & 0xff);
	s->code[operand + 1] = (uint8_t)((uint16_t)off >> 8);
}

static void patch_jump_to(Comp *c, size_t operand, size_t target)
{
	VmProg *s;
	long off;

	if (c->failed)
		return;

	s = stge_prog();
	if (s == NULL)
	{
		fail(c, "no compile context");
		return;
	}

	off = (long)target - (long)(operand + 2);
	if (off < -32768 || off > 32767)
	{
		fail(c, "body too large");
		return;
	}

	s->code[operand] = (uint8_t)((uint16_t)off & 0xff);
	s->code[operand + 1] = (uint8_t)((uint16_t)off >> 8);
}

static void poke_i32(Comp *c, size_t at, int32_t v)
{
	VmProg *s = stge_prog();
	uint32_t u = (uint32_t)v;

	if (c->failed || s == NULL || at + 4 > s->code_len)
		return;

	s->code[at] = (uint8_t)(u & 0xff);
	s->code[at + 1] = (uint8_t)((u >> 8) & 0xff);
	s->code[at + 2] = (uint8_t)((u >> 16) & 0xff);
	s->code[at + 3] = (uint8_t)((u >> 24) & 0xff);
}

static void patch_fn_calls(Comp *c, int obj_id, const char *fn, int pc)
{
	int i;

	if (c == NULL || fn == NULL || pc < 0)
		return;

	for (i = 0; i < c->call_patch_count; i++)
	{
		if (c->call_patch_obj[i] == obj_id && strcmp(c->call_patch_fn[i], fn) == 0)
			poke_i32(c, c->call_patch_at[i], pc);
	}
}

/* ---- names the script can use ------------------------------------------ */

typedef struct
{
	const char *name;
	Op op;
	int argc;
	bool is_static;
} Method;

static const Method methods[] = {
	{"quit", OP_QUIT, 1, true},
	{"Quit", OP_QUIT, 1, true},
	{"rand", OP_RAND, 1, true},
	{"print", OP_PRINT, 1, true},
	{"find", OP_FIND, 1, true},
	{"spawn", OP_SPAWN, 2, true},
	{"center_x", OP_CENTER_X, 0, false},
	{"center_y", OP_CENTER_Y, 0, false},
	{"center_xy", OP_CENTER_XY, 0, false},
	{"center_on", OP_CAM_CENTER_ON, 1, true},
	{"add_child", OP_ADD_CHILD, 1, false},
};

typedef struct
{
	const char *name;
	int32_t value;
} Constant;

static const Constant constants[] = {
	{"COLOR_DEFAULT", COLOR_DEFAULT},
	{"COLOR_BLACK", COLOR_BLACK},
	{"COLOR_RED", COLOR_RED},
	{"COLOR_GREEN", COLOR_GREEN},
	{"COLOR_YELLOW", COLOR_YELLOW},
	{"COLOR_BLUE", COLOR_BLUE},
	{"COLOR_MAGENTA", COLOR_MAGENTA},
	{"COLOR_CYAN", COLOR_CYAN},
	{"COLOR_WHITE", COLOR_WHITE},
	{"COLOR_LIGHTBLACK", COLOR_LIGHTBLACK},
	{"COLOR_LIGHTRED", COLOR_LIGHTRED},
	{"COLOR_LIGHTGREEN", COLOR_LIGHTGREEN},
	{"COLOR_LIGHTYELLOW", COLOR_LIGHTYELLOW},
	{"COLOR_LIGHTBLUE", COLOR_LIGHTBLUE},
	{"COLOR_LIGHTMAGENTA", COLOR_LIGHTMAGENTA},
	{"COLOR_LIGHTCYAN", COLOR_LIGHTCYAN},
	{"COLOR_LIGHTWHITE", COLOR_LIGHTWHITE},
};

static const Method *find_method(const char *name)
{
	size_t i;

	for (i = 0; i < sizeof methods / sizeof methods[0]; i++)
	{
		if (strcmp(methods[i].name, name) == 0)
			return &methods[i];
	}

	return NULL;
}

static const Constant *find_constant(const char *name)
{
	size_t i;

	for (i = 0; i < sizeof constants / sizeof constants[0]; i++)
	{
		if (strcmp(constants[i].name, name) == 0)
			return &constants[i];
	}

	return NULL;
}

/* ---- expressions --------------------------------------------------------- */

static ExprKind compile_expr(Comp *c);
static void emit_call_to_str(Comp *c, const char *type);
static void compile_index_or_zero(Comp *c);
static void compile_include(Comp *c, const char *path);
static void include_pass(Comp *c, const char *path, bool compiling);
static void store_fn(Comp *c, NodeRef *node, const char *field, int pc, bool has_arg);
static void compile_call(Comp *c);
static void compile_top_statement(Comp *c);
static bool at_compound(const Comp *c);
static char *read_file(const char *path, size_t *out_len);
static int compile_body(Comp *c, bool *has_arg);
static bool emit_packed_new0(Comp *c, const char *type);
static const PackedType *packed_of_name(Comp *c, const char *recv);
static bool type_name_kind(const char *name, FieldKind *fk, Op *op, MemberKind *mk);

static Tok look(const Comp *c)
{
	Lex lex = lex_probe(&c->lex);
	Tok t;
	static char *look_text;
	int i;

	t = lex_tok(&lex);
	/* Keep a persist-owned string; IDENT slices stay in src. */
	free(look_text);
	look_text = NULL;
	for (i = 0; i < lex.persist_count; i++)
	{
		if (lex.persist[i] == t.text)
		{
			look_text = t.text;
			lex.persist[i] = NULL;
			break;
		}
	}
	lex_probe_free(&lex);
	if (t.text == NULL)
	{
		t.text = "";
		t.len = 0;
	}
	return t;
}

static bool is_str_bytes(const char *field)
{
	return field != NULL && strcmp(field, "buffer") == 0;
}

static void emit_stack_to_str(Comp *c)
{
	if (c->expr_kind == EK_INT)
		emit_call_to_str(c, "int");
	else if (c->expr_kind == EK_OBJ && c->last_ret_type[0] != '\0'
		 && strcmp(c->last_ret_type, "String") != 0)
		emit_call_to_str(c, c->last_ret_type);
	else
		c->expr_kind = EK_OBJ;
}

/* Receiver value already on the stack. First param is bare type-name self. */
static bool emit_stacked_instance_call(Comp *c, const char *type_name, const char *method)
{
	const NodeRef *node;
	const ObjFn *fn = NULL;
	int i;
	int paren_argc = 0;
	int need_paren;

	if (c == NULL || type_name == NULL || type_name[0] == '\0' || method == NULL)
		return false;

	node = find_node(c, type_name);
	if (node == NULL)
		return false;

	for (i = 0; i < node->fn_count; i++)
	{
		if (strcmp(node->fns[i].name, method) == 0 && !node->fns[i].is_extern
		    && node->fns[i].self_param == 0)
		{
			fn = &node->fns[i];
			break;
		}
	}
	if (fn == NULL)
		return false;

	need_paren = fn->argc - 1;
	if (need_paren < 0)
		need_paren = 0;

	if (!at(c, TOK_LPAREN))
	{
		if (need_paren != 0)
		{
			fail(c, "expected '('");
			return true;
		}
	}
	else
	{
		advance(c);
		while (!c->failed && !at(c, TOK_RPAREN) && paren_argc < 65535)
		{
			if (paren_argc > 0)
				expect(c, TOK_COMMA, "expected ','");
			compile_expr(c);
			paren_argc++;
		}
		expect(c, TOK_RPAREN, "expected ')'");
	}

	if (paren_argc != need_paren)
	{
		failf(c, "wrong number of arguments: '%s.%s' takes %d, got %d",
		      type_name, method, need_paren, paren_argc);
		return true;
	}

	emit_push_node(c, node->id);
	{
		VmProg *s = stge_prog();

		emit_push(c, fn->pc);
		if (fn->pc < 0 && s != NULL && c->call_patch_count < STGE_MAX_CALL_PATCH)
		{
			c->call_patch_at[c->call_patch_count] = s->code_len - 4;
			c->call_patch_obj[c->call_patch_count] = node->id;
			strcpy(c->call_patch_fn[c->call_patch_count], method);
			c->call_patch_count++;
		}
	}
	emit_call(c, OP_CALL, fn->argc);
	if (fn->ret_type[0] != '\0')
	{
		if (strcmp(fn->ret_type, "String") == 0
		    || find_node(c, fn->ret_type) != NULL)
		{
			c->expr_kind = EK_OBJ;
			snprintf(c->last_ret_type, sizeof c->last_ret_type, "%s", fn->ret_type);
		}
		else
		{
			c->expr_kind = EK_INT;
			snprintf(c->last_ret_type, sizeof c->last_ret_type, "%s", fn->ret_type);
		}
	}
	else
		c->expr_kind = EK_INT;
	return true;
}

static void apply_field(Comp *c, const char *field)
{
	const PackedType *p = find_packed(c, c->last_ret_type);
	int idx;

	if (c->expr_kind == EK_OBJ && packed_is_composite(p))
	{
		idx = packed_alias_index(p, field);
		if (idx < 0)
		{
			failf(c, "unknown field '%s'", field);
			return;
		}

		emit_push_str(c, p->name);
		emit_push(c, idx);
		emit_u8(c, OP_GET_MEM);
		c->expr_kind = EK_INT;
		c->last_ret_type[0] = '\0';
		return;
	}

	if (is_str_bytes(field) && at(c, TOK_LBRACK))
	{
		emit_push_str(c, field);
		compile_index_or_zero(c);
		emit_u8(c, OP_GET_MEM);
		c->expr_kind = EK_INT;
		return;
	}

	if (is_str_bytes(field))
	{
		emit_push_str(c, field);
		emit_u8(c, OP_GET_BUF);
		c->expr_kind = EK_INT;
		return;
	}

	emit_push_str(c, field);
	emit_push(c, 0);
	emit_u8(c, OP_GET_MEM);
	c->expr_kind = EK_INT;
}

static void compile_value_dots(Comp *c)
{
	while (!c->failed && at(c, TOK_DOT))
	{
		char field[STGE_NAME_MAX];
		char type_name[STGE_NAME_MAX];
		Lex probe;
		Tok after_name;

		type_name[0] = '\0';
		if (c->last_ret_type[0] != '\0')
			snprintf(type_name, sizeof type_name, "%s", c->last_ret_type);
		else if (c->expr_kind == EK_INT)
			snprintf(type_name, sizeof type_name, "int");

		advance(c);
		if (!at(c, TOK_IDENT))
		{
			fail(c, "expected a field");
			return;
		}

		tok_copy(field, sizeof field, &c->tok);
		probe = lex_probe(&c->lex);
		after_name = lex_tok(&probe);
		lex_probe_free(&probe);
		advance(c);

		if (after_name.kind == TOK_LPAREN && type_name[0] != '\0'
		    && emit_stacked_instance_call(c, type_name, field))
			continue;

		if (strcmp(field, "to_str") == 0)
		{
			if (at(c, TOK_LPAREN))
			{
				advance(c);
				expect(c, TOK_RPAREN, "expected ')'");
			}

			emit_stack_to_str(c);
			continue;
		}

		apply_field(c, field);
	}
}

static bool word_is(const Comp *c, const char *name)
{
	return at(c, TOK_IDENT) && tok_eq(&c->tok, name);
}

static bool next_is_call(const Comp *c)
{
	Lex lex;
	Tok t;

	if (!at(c, TOK_IDENT))
		return false;

	lex = lex_probe(&c->lex);
	t = lex_tok(&lex);
	if (t.kind == TOK_LPAREN)
	{
		lex_probe_free(&lex);
		return true;
	}

	if (t.kind == TOK_DOT)
	{
		for (;;)
		{
			t = lex_tok(&lex);
			if (t.kind != TOK_IDENT)
			{
				lex_probe_free(&lex);
				return false;
			}

			t = lex_tok(&lex);
			if (t.kind == TOK_LPAREN)
			{
				lex_probe_free(&lex);
				return true;
			}
			if (t.kind != TOK_DOT)
			{
				lex_probe_free(&lex);
				return false;
			}
		}
	}

	if (t.kind == TOK_COLONCOLON)
	{
		t = lex_tok(&lex);
		if (t.kind != TOK_IDENT)
		{
			lex_probe_free(&lex);
			return false;
		}

		t = lex_tok(&lex);
		while (t.kind == TOK_COLONCOLON)
		{
			t = lex_tok(&lex);
			if (t.kind != TOK_IDENT)
			{
				lex_probe_free(&lex);
				return false;
			}
			t = lex_tok(&lex);
		}

		{
			bool ok = t.kind == TOK_LPAREN;

			lex_probe_free(&lex);
			return ok;
		}
	}

	lex_probe_free(&lex);
	return false;
}

static const char *field_owner(const Comp *c, const char *recv)
{
	if (c != NULL && c->has_param && recv != NULL && strcmp(recv, c->param) == 0 && c->param_type[0] != '\0')
		return c->param_type;

	return recv;
}

static void emit_recv(Comp *c, const char *name)
{
	const NodeRef *node;

	if (c->has_param && strcmp(name, c->param) == 0)
	{
		emit_push_arg(c, 0);
		return;
	}

	{
		int slot = find_slot_local(c, name);

		if (slot >= 0)
		{
			emit_get_local(c, slot);
			return;
		}
	}

	if (strcmp(name, "self") == 0 || is_local(c, name))
	{
		if (c->self < 0)
		{
			fail(c, "self outside a body");
			return;
		}

		emit_u8(c, OP_PUSH_SELF);
		return;
	}

	node = find_node(c, name);
	if (node == NULL)
	{
		failf(c, "unknown node '%s'", name);
		return;
	}

	if (c->self >= 0 && node->id == c->self)
	{
		emit_u8(c, OP_PUSH_SELF);
		return;
	}

	emit_push_node(c, node->id);
}

static void compile_index_checked(Comp *c, const char *recv, const char *member)
{
	int32_t arr_len;
	bool growable;

	if (!at(c, TOK_LBRACK))
	{
		emit_push(c, 0);
		return;
	}

	advance(c);
	arr_len = (recv != NULL && member != NULL) ? lookup_field_arr_len(c, recv, member) : 1;
	growable = (recv != NULL && member != NULL) && lookup_field_growable(c, recv, member);

	if (at(c, TOK_INT) && !growable && recv != NULL)
	{
		int32_t idx = c->tok.value;

		if (idx < 0 || (arr_len > 0 && idx >= arr_len))
		{
			char msg[96];

			snprintf(msg, sizeof msg, "index %d out of range for array[%d]",
				 (int)idx, (int)arr_len);
			fail(c, msg);
			return;
		}
	}

	if (at(c, TOK_MINUS) && look(c).kind == TOK_INT)
	{
		fail(c, "index -1 out of range");
		return;
	}

	compile_expr(c);
	expect(c, TOK_RBRACK, "expected ']'");
}

static void compile_index_or_zero(Comp *c)
{
	compile_index_checked(c, NULL, NULL);
}

static bool is_stge_host(const char *name)
{
	return strcmp(name, "stge") == 0 || strcmp(name, "Stge") == 0;
}

static void compile_stge_get(Comp *c, const char *field)
{
	if (strcmp(field, "tickrate") == 0)
	{
		emit_u8(c, OP_GET_TICKRATE);
		c->expr_kind = EK_INT;
		return;
	}

	if (strcmp(field, "print_out") == 0)
	{
		emit_u8(c, OP_GET_PRINT_OUT);
		c->expr_kind = EK_STR;
		return;
	}

	if (strcmp(field, "screen") == 0)
	{
		if (!at(c, TOK_DOT))
		{
			emit_u8(c, OP_GET_WIDTH);
			c->expr_kind = EK_INT;
			return;
		}

		advance(c);
		if (!at(c, TOK_IDENT))
		{
			fail(c, "expected a field");
			return;
		}

		if (tok_eq(&c->tok, "size"))
		{
			advance(c);
			if (at(c, TOK_DOT))
			{
				advance(c);
				if (!at(c, TOK_IDENT))
				{
					fail(c, "expected a field");
					return;
				}

				if (tok_eq(&c->tok, "x"))
					emit_u8(c, OP_GET_WIDTH);
				else if (tok_eq(&c->tok, "y"))
					emit_u8(c, OP_GET_HEIGHT);
				else
				{
					failf(c, "unknown field '%.*s' on 'stge.screen.size'", (int)c->tok.len, c->tok.text);
					return;
				}

				advance(c);
				c->expr_kind = EK_INT;
				return;
			}

			emit_u8(c, OP_GET_WIDTH);
			c->expr_kind = EK_INT;
			return;
		}

		if (tok_eq(&c->tok, "buffer"))
		{
			advance(c);
			emit_u8(c, OP_GET_SCREEN_BUF);
			c->expr_kind = EK_INT;
			return;
		}

		failf(c, "unknown field '%.*s' on 'stge.screen'", (int)c->tok.len, c->tok.text);
		return;
	}

	failf(c, "unknown field '%s' on 'stge'", field);
}

static void compile_camera_get(Comp *c, const char *field)
{
	if (strcmp(field, "x") == 0)
	{
		emit_push(c, 0);
		emit_u8(c, OP_GET_CAM);
		return;
	}

	if (strcmp(field, "y") == 0)
	{
		emit_push(c, 1);
		emit_u8(c, OP_GET_CAM);
		return;
	}

	if (strcmp(field, "pos") == 0)
	{
		compile_index_or_zero(c);
		emit_u8(c, OP_GET_CAM);
		return;
	}

	failf(c, "unknown field '%s' on 'Camera'", field);
}

static void emit_get_field(Comp *c, const char *field)
{
	emit_push_str(c, field);
	compile_index_or_zero(c);
	emit_u8(c, OP_GET_MEM);
}

static void count_init_spawn(Comp *c)
{
	if (c->self < 0)
		c->spawn_count++;
}

static void emit_to_str(Comp *c, ExprKind k)
{
	if (k == EK_STR)
		return;
	if (k == EK_FLOAT)
		emit_u8(c, OP_STR_F);
	else if (k == EK_DOUBLE)
		emit_u8(c, OP_STR_D);
	else if (k == EK_OBJ)
		emit_u8(c, OP_STR_OBJ);
	else
		emit_u8(c, OP_STR);
	c->expr_kind = EK_STR;
}

static bool compile_stringify_field(Comp *c)
{
	char name[STGE_NAME_MAX];
	char field[STGE_NAME_MAX];
	FieldKind fk;
	Lex saved_lex = c->lex;
	Tok saved_tok = c->tok;

	if (!at(c, TOK_IDENT) || look(c).kind != TOK_DOT)
		return false;

	tok_copy(name, sizeof name, &c->tok);
	advance(c);
	advance(c);
	if (!at(c, TOK_IDENT))
	{
		lex_keep_buf(&c->lex, &saved_lex);
		c->tok = saved_tok;
		return false;
	}

	tok_copy(field, sizeof field, &c->tok);
	advance(c);
	if (at(c, TOK_DOT) || at(c, TOK_LBRACK))
	{
		lex_keep_buf(&c->lex, &saved_lex);
		c->tok = saved_tok;
		return false;
	}

	fk = lookup_field_kind(c, name, field);
	if (field_is_vec(fk) || fk == FK_FLOAT || fk == FK_DOUBLE)
	{
		emit_recv(c, name);
		emit_push_str(c, field);
		emit_u8(c, OP_STR_MEM);
		c->expr_kind = EK_STR;
		return true;
	}

	if (fk == FK_OBJ)
	{
		emit_recv(c, name);
		emit_push_str(c, field);
		emit_push(c, 0);
		emit_u8(c, OP_GET_MEM);
		emit_u8(c, OP_STR_OBJ);
		c->expr_kind = EK_STR;
		return true;
	}

	lex_keep_buf(&c->lex, &saved_lex);
	c->tok = saved_tok;
	return false;
}

static bool compile_stringify_node(Comp *c)
{
	const NodeRef *node;
	Tok nxt;

	if (!at(c, TOK_IDENT))
		return false;

	nxt = look(c);
	if (nxt.kind == TOK_DOT || nxt.kind == TOK_LPAREN || nxt.kind == TOK_COLONCOLON)
		return false;

	node = find_node(c, tok_cstr(c, &c->tok));
	if (node == NULL)
		return false;

	if (nxt.kind != TOK_RPAREN && nxt.kind != TOK_COMMA && nxt.kind != TOK_EOL && nxt.kind != TOK_EOF)
		return false;

	emit_push_node(c, node->id);
	advance(c);
	emit_u8(c, OP_STR_OBJ);
	c->expr_kind = EK_STR;
	return true;
}

static void compile_stringify(Comp *c)
{
	Tok nxt;

	if (compile_stringify_field(c) || compile_stringify_node(c))
		return;

	/* A function arg is already an interned string or a raw value meant
	   for SET_DISPLAY. OP_STR would print its intern index ("1" for "@"). */
	if (c->has_param && at(c, TOK_IDENT) && tok_eq(&c->tok, c->param))
	{
		nxt = look(c);
		if (nxt.kind != TOK_DOT && nxt.kind != TOK_LPAREN && nxt.kind != TOK_LBRACK
		    && nxt.kind != TOK_COLONCOLON)
		{
			emit_push_arg(c, 0);
			advance(c);
			c->expr_kind = EK_STR;
			return;
		}
	}

	emit_to_str(c, compile_expr(c));
}

static void emit_string_obj(Comp *c, const char *text)
{
	emit_push_str(c, text != NULL ? text : "");
	emit_u8(c, OP_STRING_WRAP);
	c->expr_kind = EK_OBJ;
	snprintf(c->last_ret_type, sizeof c->last_ret_type, "String");
}

static void emit_extern1(Comp *c, const char *name)
{
	int id = stge_native_find(name);

	if (id < 0)
	{
		failf(c, "%s is missing", name);
		return;
	}

	/* One arg already on the stack. */
	emit_extern(c, id, 1);
}

static void emit_extern2(Comp *c, const char *name)
{
	int id = stge_native_find(name);

	if (id < 0)
	{
		failf(c, "%s is missing", name);
		return;
	}

	/* Two args already on the stack. */
	emit_extern(c, id, 2);
}

static void emit_string_new(Comp *c)
{
	int id = stge_native_find("stge_string_new");

	if (id < 0)
	{
		fail(c, "stge_string_new is missing");
		return;
	}

	emit_extern(c, id, 0);
	c->expr_kind = EK_OBJ;
	snprintf(c->last_ret_type, sizeof c->last_ret_type, "String");
}

static void emit_string_clear(Comp *c)
{
	emit_extern1(c, "stge_string_clear");
	c->expr_kind = EK_OBJ;
	snprintf(c->last_ret_type, sizeof c->last_ret_type, "String");
}

static void emit_string_append(Comp *c)
{
	/* stack: dest, src -> dest */
	emit_extern2(c, "stge_string_append");
	c->expr_kind = EK_OBJ;
	snprintf(c->last_ret_type, sizeof c->last_ret_type, "String");
}

static void emit_string_append_int(Comp *c)
{
	/* stack: dest, int -> dest */
	emit_extern2(c, "stge_string_append_int");
	c->expr_kind = EK_OBJ;
	snprintf(c->last_ret_type, sizeof c->last_ret_type, "String");
}

static void emit_string_concat(Comp *c)
{
	int id = stge_native_find("stge_string_concat");

	if (id < 0)
	{
		fail(c, "string concat is missing");
		return;
	}

	emit_extern(c, id, 2);
	c->expr_kind = EK_OBJ;
	snprintf(c->last_ret_type, sizeof c->last_ret_type, "String");
}

static void emit_as_string(Comp *c)
{
	if (c->failed)
		return;

	if (c->expr_kind == EK_OBJ && strcmp(c->last_ret_type, "String") == 0)
		return;

	if (c->expr_kind == EK_FLOAT)
	{
		emit_u8(c, OP_STR_F);
		emit_u8(c, OP_STRING_WRAP);
		c->expr_kind = EK_OBJ;
		snprintf(c->last_ret_type, sizeof c->last_ret_type, "String");
		return;
	}

	if (c->expr_kind == EK_DOUBLE)
	{
		emit_u8(c, OP_STR_D);
		emit_u8(c, OP_STRING_WRAP);
		c->expr_kind = EK_OBJ;
		snprintf(c->last_ret_type, sizeof c->last_ret_type, "String");
		return;
	}

	if (c->expr_kind == EK_OBJ && c->last_ret_type[0] != '\0')
	{
		emit_call_to_str(c, c->last_ret_type);
		return;
	}

	if (c->expr_kind == EK_INT)
	{
		emit_call_to_str(c, "int");
		return;
	}

	if (c->expr_kind == EK_STR)
	{
		emit_u8(c, OP_STRING_WRAP);
		c->expr_kind = EK_OBJ;
		snprintf(c->last_ret_type, sizeof c->last_ret_type, "String");
		return;
	}

	emit_u8(c, OP_STR_OBJ);
	c->expr_kind = EK_OBJ;
	snprintf(c->last_ret_type, sizeof c->last_ret_type, "String");
}

/* Dest String id is already on the stack; clear and append parts into it. */
static void compile_string_into(Comp *c)
{
	emit_string_clear(c);

	while (!c->failed && at(c, TOK_STRING))
	{
		char *text;
		bool hole;

		text = strdup(c->tok.text != NULL ? c->tok.text : "");
		if (text == NULL)
		{
			fail(c, "out of memory");
			return;
		}

		hole = c->tok.hole;
		advance(c);
		if (c->failed)
		{
			free(text);
			return;
		}

		if (text[0] != '\0')
		{
			emit_string_obj(c, text);
			emit_string_append(c);
		}
		free(text);

		if (!hole)
			break;

		compile_expr(c);
		if (c->failed)
			return;

		if (c->expr_kind == EK_INT)
			emit_string_append_int(c);
		else
		{
			emit_as_string(c);
			emit_string_append(c);
		}

		if (!at(c, TOK_RBRACE))
		{
			fail(c, "expected '}'");
			return;
		}

		advance(c);
	}

	c->expr_kind = EK_OBJ;
	snprintf(c->last_ret_type, sizeof c->last_ret_type, "String");
}

static void compile_string(Comp *c)
{
	emit_string_new(c);
	compile_string_into(c);
}

static void compile_color_ctor(Comp *c)
{
	Color col;

	advance(c);
	expect(c, TOK_LPAREN, "expected '('");
	if (!at(c, TOK_STRING) || c->tok.hole)
	{
		fail(c, c->tok.hole ? "string interpolation is not allowed here" : "expected a color name");
		return;
	}

	if (!color_parse(tok_cstr(c, &c->tok), &col))
	{
		failf(c, "unknown color '%.*s'", (int)c->tok.len, c->tok.text);
		return;
	}

	emit_push(c, (int32_t)col);
	advance(c);
	expect(c, TOK_RPAREN, "expected ')'");
}

static void compile_atom(Comp *c)
{
	const Constant *k;
	const NodeRef *node;

	if (c->failed)
		return;

	c->expr_kind = EK_INT;
	c->last_ret_type[0] = '\0';

	if (at(c, TOK_PLUS))
	{
		advance(c);
		compile_atom(c);
		return;
	}

	if (at(c, TOK_MINUS))
	{
		advance(c);
		emit_push(c, 0);
		compile_atom(c);
		if (c->expr_kind == EK_FLOAT)
		{
			emit_u8(c, OP_I2F);
			emit_u8(c, OP_SUB_F);
		}
		else if (c->expr_kind == EK_DOUBLE)
		{
			emit_u8(c, OP_I2D);
			emit_u8(c, OP_SUB_D);
		}
		else
			emit_u8(c, OP_SUB);
		return;
	}

	if (at(c, TOK_IDENT) && tok_eq(&c->tok, "not"))
	{
		advance(c);
		compile_atom(c);
		emit_u8(c, OP_NOT);
		c->expr_kind = EK_INT;
		return;
	}

	if (at(c, TOK_LPAREN))
	{
		advance(c);
		compile_expr(c);
		expect(c, TOK_RPAREN, "expected ')'");
		return;
	}

	if (at(c, TOK_LBRACE))
	{
		advance(c);
		expect(c, TOK_RBRACE, "expected '}'");
		emit_push_str(c, "");
		emit_u8(c, OP_SPAWN_MAP);
		count_init_spawn(c);
		return;
	}

	if (at(c, TOK_INT))
	{
		emit_push(c, c->tok.value);
		c->expr_kind = EK_INT;
		snprintf(c->last_ret_type, sizeof c->last_ret_type, "int");
		advance(c);
		compile_value_dots(c);
		return;
	}

	if (at(c, TOK_FLOAT))
	{
		if ((double)(float)c->tok.dvalue == c->tok.dvalue)
		{
			emit_push_f(c, (float)c->tok.dvalue);
			c->expr_kind = EK_FLOAT;
		}
		else
		{
			emit_push_d(c, c->tok.dvalue);
			c->expr_kind = EK_DOUBLE;
		}
		advance(c);
		compile_value_dots(c);
		return;
	}

	if (at(c, TOK_CHAR))
	{
		emit_push(c, (int32_t)(uint8_t)c->tok.text[0]);
		c->expr_kind = EK_INT;
		advance(c);
		return;
	}

	if (at(c, TOK_STRING))
	{
		compile_string(c);
		return;
	}

	if (!at(c, TOK_IDENT))
	{
		fail(c, "expected a value");
		return;
	}

	if (word_is(c, "color") && look(c).kind == TOK_LPAREN)
	{
		compile_color_ctor(c);
		return;
	}

	if (word_is(c, "sto") && look(c).kind == TOK_LPAREN)
	{
		fail(c, "sto() is gone; use sprite.new or {}");
		return;
	}

	if (next_is_call(c))
	{
		compile_call(c);
		if (at(c, TOK_DOT))
			compile_value_dots(c);
		return;
	}

	if (look(c).kind == TOK_COLONCOLON)
	{
		char path[STGE_NAME_MAX * 2];
		char last[STGE_NAME_MAX];
		const NsRef *ns;
		const NsConst *cn;
		const ObjFn *sf;
		const NodeRef *node;

		tok_copy(path, STGE_NAME_MAX, &c->tok);
		path[STGE_NAME_MAX] = '\0';
		last[0] = '\0';
		advance(c);
		while (!c->failed && at(c, TOK_COLONCOLON))
		{
			advance(c);
			if (!at(c, TOK_IDENT))
			{
				fail(c, "expected a name");
				return;
			}

			tok_copy(last, sizeof last, &c->tok);
			advance(c);
			if (at(c, TOK_COLONCOLON))
			{
				char next_path[STGE_NAME_MAX * 2];

				if (!join_ns(next_path, sizeof next_path, path, last))
				{
					fail(c, "name too long");
					return;
				}

				memcpy(path, next_path, sizeof next_path);
			}
		}

		if (at(c, TOK_DOT))
		{
			char field[STGE_NAME_MAX];
			char qname[STGE_NAME_MAX * 2];

			advance(c);
			if (!at(c, TOK_IDENT))
			{
				fail(c, "expected a field");
				return;
			}

			tok_copy(field, sizeof field, &c->tok);
			advance(c);
			if (!join_ns(qname, sizeof qname, path, last))
			{
				fail(c, "name too long");
				return;
			}

			ns = find_ns(c, qname);
			cn = ns_find_const(ns, field);
			if (cn != NULL)
			{
				emit_push(c, cn->value);
				c->expr_kind = EK_INT;
				if (strlen(qname) >= sizeof c->last_ret_type)
				{
					fail(c, "name too long");
					return;
				}

				memcpy(c->last_ret_type, qname, strlen(qname) + 1);
				return;
			}

			ns = find_ns(c, path);
			sf = ns_find_fn(ns, last);
			if (sf != NULL && sf->is_extern)
			{
				emit_extern_getter(c, sf);
				apply_field(c, field);
				return;
			}

			failf(c, "unknown name '%s' on '%s'", field, path);
			return;
		}

		ns = find_ns(c, path);
		sf = ns_find_fn(ns, last);
		if (sf != NULL && sf->is_extern)
		{
			emit_extern_getter(c, sf);
			return;
		}

		cn = ns_find_const(ns, last);
		if (cn != NULL)
		{
			emit_push(c, cn->value);
			c->expr_kind = EK_INT;
			snprintf(c->last_ret_type, sizeof c->last_ret_type, "%s", path);
			return;
		}

		cn = find_enum_const(c, last);
		if (cn != NULL)
		{
			emit_push(c, cn->value);
			c->expr_kind = EK_INT;
			return;
		}

		node = find_node(c, path);
		if (node == NULL || last[0] == '\0')
		{
			failf(c, "unknown name '%s::%s'", path, last);
			return;
		}

		emit_push_node(c, node->id);
		emit_push_str(c, last);
		emit_push(c, 0);
		emit_u8(c, OP_GET_MEM);
		c->expr_kind = EK_INT;
		{
			const char *ft = lookup_field_type(c, path, last);

			if (ft != NULL && ft[0] != '\0')
				snprintf(c->last_ret_type, sizeof c->last_ret_type, "%s", ft);
		}
		return;
	}

	if (look(c).kind == TOK_DOT)
	{
		int slot;
		char name[STGE_NAME_MAX];
		char field[STGE_NAME_MAX];

		if (c->has_param && tok_eq(&c->tok, c->param))
		{
			emit_push_arg(c, 0);
			c->expr_kind = EK_OBJ;
			apply_param_type(c);
			advance(c);
			compile_value_dots(c);
			return;
		}

		slot = find_slot_local(c, tok_cstr(c, &c->tok));
		if (slot >= 0)
		{
			emit_get_local(c, slot);
			c->expr_kind = c->slot_kinds[slot] == FK_OBJ ? EK_OBJ : EK_INT;
			apply_slot_type(c, slot);
			advance(c);
			compile_value_dots(c);
			return;
		}

		tok_copy(name, sizeof name, &c->tok);
		advance(c);
		advance(c);

		if (!at(c, TOK_IDENT))
		{
			fail(c, "expected a field");
			return;
		}

		tok_copy(field, sizeof field, &c->tok);
		advance(c);

		if (strcmp(name, "Window") == 0)
		{
			if (strcmp(field, "width") == 0)
				emit_u8(c, OP_GET_WIDTH);
			else if (strcmp(field, "height") == 0)
				emit_u8(c, OP_GET_HEIGHT);
			else
				failf(c, "unknown field '%s' on 'Window'", field);
			return;
		}

		if (strcmp(name, "Camera") == 0)
		{
			compile_camera_get(c, field);
			return;
		}

		if (is_stge_host(name))
		{
			compile_stge_get(c, field);
			return;
		}

		{
			const char *owner = field_owner(c, name);
			FieldKind fk = lookup_field_kind(c, owner, field);
			bool growable = lookup_field_growable(c, owner, field);
			const PackedType *pf = find_packed(c, lookup_field_type(c, owner, field));

			if (packed_is_composite(pf) && at(c, TOK_DOT))
			{
				int idx;

				advance(c);
				if (!at(c, TOK_IDENT))
				{
					fail(c, "expected a field");
					return;
				}

				idx = packed_alias_index(pf, tok_cstr(c, &c->tok));
				if (idx < 0)
				{
					failf(c, "unknown field '%.*s' on '%s.%s'", (int)c->tok.len, c->tok.text, name, field);
					return;
				}

				advance(c);
				emit_recv(c, name);
				emit_push_str(c, field);
				emit_push(c, idx);
				emit_u8(c, OP_GET_MEM);
				c->expr_kind = EK_INT;
				return;
			}

			if (packed_is_composite(pf) && at(c, TOK_LBRACK))
			{
				emit_recv(c, name);
				emit_push_str(c, field);
				compile_index_checked(c, owner, field);
				emit_u8(c, OP_GET_MEM);
				c->expr_kind = EK_INT;
				return;
			}

			if (packed_is_composite(pf))
			{
				if (!emit_packed_new0(c, pf->name))
				{
					failf(c, "unknown function '%s::new'", pf->name);
					return;
				}

				emit_push_str(c, pf->name);
				emit_recv(c, name);
				emit_push_str(c, field);
				emit_u8(c, OP_MEM_COPY);
				c->expr_kind = EK_OBJ;
				snprintf(c->last_ret_type, sizeof c->last_ret_type, "%s", pf->name);
				if (at(c, TOK_DOT))
					compile_value_dots(c);
				return;
			}

			if (at(c, TOK_DOT))
			{
				Lex saved_lex = c->lex;
				Tok saved_tok = c->tok;

				advance(c);
				if (at(c, TOK_IDENT) && tok_eq(&c->tok, "len"))
				{
					advance(c);
					emit_recv(c, name);
					emit_push_str(c, field);
					emit_u8(c, OP_GET_LEN);
					c->expr_kind = EK_INT;
					return;
				}

				lex_keep_buf(&c->lex, &saved_lex);
				c->tok = saved_tok;
			}

			emit_recv(c, name);
			emit_push_str(c, field);
			if ((growable || is_str_bytes(field)) && !at(c, TOK_LBRACK))
			{
				emit_u8(c, OP_GET_BUF);
				c->expr_kind = EK_INT;
				return;
			}

			if (at(c, TOK_DOT))
			{
				char nextf[STGE_NAME_MAX];
				const char *otype = lookup_field_type(c, owner, field);
				FieldKind nk;
				const PackedType *np;

				advance(c);
				if (!at(c, TOK_IDENT))
				{
					fail(c, "expected a field");
					return;
				}

				tok_copy(nextf, sizeof nextf, &c->tok);
				nk = otype[0] != '\0' ? lookup_field_kind(c, otype, nextf)
					: lookup_field_kind(c, field, nextf);
				advance(c);
				np = find_packed(c, otype[0] != '\0'
					? lookup_field_type(c, otype, nextf)
					: lookup_field_type(c, field, nextf));
				compile_index_checked(c, owner, field);
				emit_u8(c, (uint8_t)mem_get_op(fk));
				emit_get_field(c, nextf);
				if (packed_is_composite(np) || nk == FK_OBJ)
				{
					const char *nt = otype[0] != '\0'
						? lookup_field_type(c, otype, nextf)
						: lookup_field_type(c, field, nextf);

					c->expr_kind = EK_OBJ;
					if (nt != NULL && nt[0] != '\0')
						snprintf(c->last_ret_type, sizeof c->last_ret_type, "%s", nt);
				}
				else
					c->expr_kind = EK_INT;
				if (at(c, TOK_DOT))
					compile_value_dots(c);
				return;
			}

			compile_index_checked(c, owner, field);
			emit_u8(c, (uint8_t)mem_get_op(fk));
			c->expr_kind = fk == FK_OBJ ? EK_OBJ : field_expr_kind(fk);
			if (fk == FK_OBJ)
			{
				const char *ot = lookup_field_type(c, owner, field);

				if (ot != NULL && ot[0] != '\0')
					snprintf(c->last_ret_type, sizeof c->last_ret_type, "%s", ot);
			}
			if (at(c, TOK_DOT))
				compile_value_dots(c);
			return;
		}
	}

	if (tok_eq(&c->tok, "self"))
	{
		emit_recv(c, "self");
		c->expr_kind = EK_OBJ;
		advance(c);
		return;
	}

	if (c->has_param && tok_eq(&c->tok, c->param))
	{
		emit_push_arg(c, 0);
		advance(c);
		if (at(c, TOK_DOT))
		{
			c->expr_kind = EK_OBJ;
			apply_param_type(c);
			compile_value_dots(c);
		}
		else
			c->expr_kind = EK_INT;
		return;
	}

	k = find_constant(tok_cstr(c, &c->tok));
	if (k != NULL)
	{
		emit_push(c, k->value);
		advance(c);
		return;
	}

	{
		const NsConst *cn = find_enum_const(c, tok_cstr(c, &c->tok));

		if (cn != NULL)
		{
			emit_push(c, cn->value);
			advance(c);
			return;
		}
	}

	{
		int slot = find_slot_local(c, tok_cstr(c, &c->tok));

		if (slot >= 0)
		{
			emit_get_local(c, slot);
			c->expr_kind = c->slot_kinds[slot] == FK_OBJ ? EK_OBJ : EK_INT;
			apply_slot_type(c, slot);
			if (c->last_ret_type[0] == '\0' && c->slot_kinds[slot] == FK_INT)
				snprintf(c->last_ret_type, sizeof c->last_ret_type, "int");
			advance(c);
			compile_value_dots(c);
			return;
		}
	}

	node = find_node(c, tok_cstr(c, &c->tok));
	if (node != NULL)
	{
		Tok nxt = look(c);

		if (c->self >= 0 && node->id == c->self
		    && nxt.kind != TOK_COLONCOLON && nxt.kind != TOK_LPAREN)
		{
			emit_u8(c, OP_PUSH_SELF);
			c->expr_kind = EK_OBJ;
			advance(c);
			if (at(c, TOK_DOT))
				compile_value_dots(c);
			return;
		}

		emit_push_node(c, node->id);
		c->expr_kind = EK_OBJ;
		advance(c);
		return;
	}

	if (is_local(c, tok_cstr(c, &c->tok)))
	{
		if (c->self < 0)
		{
			fail(c, "self outside a body");
			return;
		}

		if (is_local_buf(c, tok_cstr(c, &c->tok)))
		{
			emit_u8(c, OP_PUSH_SELF);
			emit_push_str(c, tok_cstr(c, &c->tok));
			emit_u8(c, OP_GET_BUF);
			c->expr_kind = EK_INT;
			advance(c);
			return;
		}

		emit_u8(c, OP_PUSH_SELF);
		c->expr_kind = EK_OBJ;
		advance(c);
		return;
	}

	failf(c, "unknown name '%.*s'", (int)c->tok.len, c->tok.text);
}

static void coerce_pair(Comp *c, ExprKind left, ExprKind right, ExprKind out)
{
	if (right != out)
		emit_coerce(c, right, out);
	if (left != out)
	{
		emit_u8(c, OP_SWAP);
		emit_coerce(c, left, out);
		emit_u8(c, OP_SWAP);
	}
}

static void compile_sum(Comp *c)
{
	ExprKind left;
	char left_type[STGE_NAME_MAX];

	compile_atom(c);
	left = c->expr_kind;
	snprintf(left_type, sizeof left_type, "%s", c->last_ret_type);

	while (!c->failed && (at(c, TOK_PLUS) || at(c, TOK_MINUS)))
	{
		int add = at(c, TOK_PLUS);
		ExprKind right;
		ExprKind out;

		if (left == EK_OBJ && strcmp(left_type, "String") == 0)
		{
			if (!add)
			{
				fail(c, "unknown operator '-' on 'String'");
				return;
			}

			advance(c);
			compile_atom(c);
			emit_as_string(c);
			emit_string_concat(c);
			left = EK_OBJ;
			snprintf(left_type, sizeof left_type, "String");
			continue;
		}

		advance(c);
		compile_atom(c);
		right = c->expr_kind;
		out = promote_kind(left, right);
		coerce_pair(c, left, right, out);
		if (out == EK_FLOAT)
			emit_u8(c, (uint8_t)(add ? OP_ADD_F : OP_SUB_F));
		else if (out == EK_DOUBLE)
			emit_u8(c, (uint8_t)(add ? OP_ADD_D : OP_SUB_D));
		else
			emit_u8(c, (uint8_t)(add ? OP_ADD : OP_SUB));
		left = out;
		left_type[0] = '\0';
	}

	c->expr_kind = left;
}

static void compile_cmp(Comp *c)
{
	Op icmp;
	bool invert = false;
	ExprKind left;

	compile_sum(c);
	left = c->expr_kind;

	if (c->failed)
		return;

	if (at(c, TOK_EQEQ))
		icmp = OP_EQ;
	else if (at(c, TOK_LTE))
	{
		icmp = OP_GT;
		invert = true;
	}
	else if (at(c, TOK_GTE))
	{
		icmp = OP_LT;
		invert = true;
	}
	else if (at(c, TOK_LT))
		icmp = OP_LT;
	else if (at(c, TOK_GT))
		icmp = OP_GT;
	else
		return;

	advance(c);
	if (icmp == OP_EQ && at(c, TOK_STRING) && !c->tok.hole && c->tok.len == 1
	    && left != EK_FLOAT && left != EK_DOUBLE && left != EK_OBJ)
	{
		emit_push(c, (unsigned char)c->tok.text[0]);
		advance(c);
		emit_u8(c, OP_EQ);
		c->expr_kind = EK_INT;
		return;
	}

	compile_sum(c);
	{
		ExprKind right = c->expr_kind;
		ExprKind out = promote_kind(left, right);

		coerce_pair(c, left, right, out);
		if (out == EK_FLOAT)
		{
			if (icmp == OP_EQ)
				emit_u8(c, OP_EQ_F);
			else if (icmp == OP_LT)
				emit_u8(c, OP_LT_F);
			else
				emit_u8(c, OP_GT_F);
		}
		else if (out == EK_DOUBLE)
		{
			if (icmp == OP_EQ)
				emit_u8(c, OP_EQ_D);
			else if (icmp == OP_LT)
				emit_u8(c, OP_LT_D);
			else
				emit_u8(c, OP_GT_D);
		}
		else
			emit_u8(c, (uint8_t)icmp);
	}

	if (invert)
		emit_u8(c, OP_NOT);
	c->expr_kind = EK_INT;
}

static ExprKind compile_expr(Comp *c)
{
	compile_cmp(c);

	while (!c->failed && at(c, TOK_IDENT) && (tok_eq(&c->tok, "and") || tok_eq(&c->tok, "or")))
	{
		Op op = tok_eq(&c->tok, "and") ? OP_AND : OP_OR;

		advance(c);
		compile_cmp(c);
		emit_u8(c, (uint8_t)op);
		c->expr_kind = EK_INT;
	}

	return c->expr_kind;
}

static void compile_args_and_op(Comp *c, const Method *m, int argc_extra)
{
	int argc = 0;
	int want;

	expect(c, TOK_LPAREN, "expected '('");

	while (!c->failed && !at(c, TOK_RPAREN))
	{
		if (argc > 0)
			expect(c, TOK_COMMA, "expected ','");

		compile_expr(c);
		argc++;
	}

	expect(c, TOK_RPAREN, "expected ')'");

	if (c->failed)
		return;

	want = m->argc + argc_extra;

	if (argc != want)
	{
		failf(c, "wrong number of arguments: '%s' takes %d, got %d",
		      m->name, want, argc);
		return;
	}

	emit_u8(c, (uint8_t)m->op);
}

static void emit_get_axis(Comp *c, const char *name, const char *axis, int32_t i)
{
	if (strcmp(name, "Camera") == 0)
	{
		emit_push(c, i);
		emit_u8(c, OP_GET_CAM);
		return;
	}

	emit_recv(c, name);
	emit_push_str(c, axis);
	emit_push(c, i);
	emit_u8(c, OP_GET_MEM);
}

/* Camera.pos is cam_pos[2]. A sto's .pos is pos[0]/pos[1], or x/y if pos is missing. */
static void emit_pos_xy(Comp *c, const char *name, bool prefer_pos)
{
	if (strcmp(name, "Camera") == 0)
	{
		emit_get_axis(c, name, "pos", 0);
		emit_get_axis(c, name, "pos", 1);
		return;
	}

	if (prefer_pos)
	{
		emit_get_axis(c, name, "pos", 0);
		emit_get_axis(c, name, "pos", 1);
		return;
	}

	emit_get_axis(c, name, "x", 0);
	emit_get_axis(c, name, "y", 0);
}

static void compile_center_on_arg(Comp *c)
{
	char name[STGE_NAME_MAX];

	expect(c, TOK_LPAREN, "expected '('");
	if (!at(c, TOK_IDENT))
	{
		fail(c, "expected a position");
		return;
	}

	tok_copy(name, sizeof name, &c->tok);
	advance(c);

	if (at(c, TOK_DOT))
	{
		advance(c);
		if (!at(c, TOK_IDENT) || !tok_eq(&c->tok, "pos"))
		{
			fail(c, "expected a pos");
			return;
		}

		advance(c);
		emit_pos_xy(c, name, true);
	}
	else
		emit_pos_xy(c, name, false);

	expect(c, TOK_RPAREN, "expected ')'");
}

static void emit_call_to_str(Comp *c, const char *type)
{
	const NsRef *ns = find_ns(c, type);
	const ObjFn *sf = ns != NULL ? ns_find_fn(ns, "to_str") : NULL;
	int tid = 0;

	if (sf != NULL && sf->is_extern && sf->native_id >= 0)
	{
		emit_extern(c, sf->native_id, 1);
		c->expr_kind = EK_OBJ;
		snprintf(c->last_ret_type, sizeof c->last_ret_type, "String");
		return;
	}

	if (ns != NULL && ns->obj_id != -1)
		tid = ns->obj_id;
	else
	{
		const NodeRef *node = find_node(c, type);

		if (node != NULL)
			tid = node->id;
	}

	emit_push(c, 0);
	emit_push_node(c, tid);
	emit_push(c, sf != NULL ? sf->pc : -1);
	if (sf != NULL && sf->pc < 0 && c->call_patch_count < STGE_MAX_CALL_PATCH)
	{
		VmProg *s = stge_prog();

		if (s != NULL)
		{
			c->call_patch_at[c->call_patch_count] = s->code_len - 4;
			c->call_patch_obj[c->call_patch_count] = tid;
			strcpy(c->call_patch_fn[c->call_patch_count], "to_str");
			c->call_patch_count++;
		}
	}

	emit_call(c, OP_CALL_NS, 1);
	c->expr_kind = EK_OBJ;
	snprintf(c->last_ret_type, sizeof c->last_ret_type, "String");
}

static void emit_script_call(Comp *c, int obj_id, int pc, bool has_arg, bool ns,
			    const char *fn, const char *recv, const char *ret_type)
{
	int self_param = -1;
	int full_argc = -1;
	int ni;

	(void)has_arg;
	for (ni = 0; ni < c->node_count; ni++)
	{
		if (c->nodes[ni].id == obj_id)
		{
			int f;

			for (f = 0; f < c->nodes[ni].fn_count; f++)
			{
				if (fn != NULL && strcmp(c->nodes[ni].fns[f].name, fn) == 0
				    && (c->nodes[ni].fns[f].pc == pc || pc < 0))
				{
					self_param = c->nodes[ni].fns[f].self_param;
					full_argc = c->nodes[ni].fns[f].argc;
					if (c->nodes[ni].fns[f].pc == pc)
						break;
				}
			}
			break;
		}
	}

	expect(c, TOK_LPAREN, "expected '('");
	c->last_ret_type[0] = '\0';

	{
		int argc = 0;
		int need_paren = (self_param == 0 && full_argc >= 0) ? full_argc - 1 : -1;

		/* Self is always first: push receiver value as arg0 when present. */
		if (self_param == 0)
		{
			if (recv != NULL)
				emit_recv(c, recv);
			else
				emit_push_node(c, obj_id);
		}

		while (!c->failed && !at(c, TOK_RPAREN) && argc < 65535)
		{
			if (argc > 0)
				expect(c, TOK_COMMA, "expected ','");
			compile_expr(c);
			argc++;
		}

		if (!c->failed && !at(c, TOK_RPAREN))
		{
			fail(c, "too many locals");
			return;
		}

		expect(c, TOK_RPAREN, "expected ')'");
		if (need_paren >= 0 && argc != need_paren)
		{
			failf(c, "wrong number of arguments: '%s' takes %d, got %d",
			      fn != NULL ? fn : "call", need_paren, argc);
			return;
		}

		if (self_param != 0)
		{
			if (recv != NULL)
				emit_recv(c, recv);
			else
				emit_push_node(c, obj_id);
		}
		else
			emit_push_node(c, obj_id);

		{
			VmProg *s = stge_prog();

			emit_push(c, pc);
			if (pc < 0 && fn != NULL && s != NULL
			    && c->call_patch_count < STGE_MAX_CALL_PATCH)
			{
				c->call_patch_at[c->call_patch_count] = s->code_len - 4;
				c->call_patch_obj[c->call_patch_count] = obj_id;
				strcpy(c->call_patch_fn[c->call_patch_count], fn);
				c->call_patch_count++;
			}
		}
		if (fn != NULL && strcmp(fn, "new") == 0)
		{
			const NodeRef *type = NULL;
			int i;

			emit_call(c, OP_CALL_NEW, argc);
			c->expr_kind = EK_OBJ;
			for (i = 0; i < c->node_count; i++)
			{
				if (c->nodes[i].id == obj_id)
				{
					type = &c->nodes[i];
					break;
				}
			}

			if (type != NULL)
				snprintf(c->last_ret_type, sizeof c->last_ret_type, "%s", type->name);
			else if (ret_type != NULL && ret_type[0] != '\0')
				snprintf(c->last_ret_type, sizeof c->last_ret_type, "%s", ret_type);
		}
		else
		{
			FieldKind fk;
			Op op;
			MemberKind mk;
			int is_obj = 0;
			int call_argc = (self_param == 0 && full_argc >= 0) ? full_argc : argc;

			emit_call(c, ns ? OP_CALL_NS : OP_CALL, call_argc);
			if (fn != NULL && strcmp(fn, "to_str") == 0)
			{
				c->expr_kind = EK_OBJ;
				snprintf(c->last_ret_type, sizeof c->last_ret_type, "String");
			}
			else if (ret_type != NULL && ret_type[0] != '\0')
			{
				if (strcmp(ret_type, "String") == 0
				    || find_node(c, ret_type) != NULL
				    || find_packed(c, ret_type) != NULL)
					is_obj = 1;
				else if (!type_name_kind(ret_type, &fk, &op, &mk))
					is_obj = 1;

				if (is_obj)
				{
					c->expr_kind = EK_OBJ;
					snprintf(c->last_ret_type, sizeof c->last_ret_type,
						 "%s", ret_type);
				}
				else
					c->expr_kind = EK_INT;
			}
			else
				c->expr_kind = EK_INT;
		}
		return;
	}
}

static bool split_colon(const char *spec, char *ns, char *fn)
{
	const char *sep = strstr(spec, "::");
	size_t nlen;

	if (sep == NULL || sep == spec || sep[2] == '\0')
		return false;

	nlen = (size_t)(sep - spec);
	if (nlen >= STGE_NAME_MAX || strlen(sep + 2) >= STGE_NAME_MAX)
		return false;

	memcpy(ns, spec, nlen);
	ns[nlen] = '\0';
	strcpy(fn, sep + 2);
	return true;
}

static void import_trait(Comp *c, const char *spec)
{
	char ns_name[STGE_NAME_MAX];
	char fn[STGE_NAME_MAX];
	NsRef *ns;

	if (!split_colon(spec, ns_name, fn))
	{
		fail(c, "bad trait");
		return;
	}

	if (strcmp(ns_name, "core") != 0 || strcmp(fn, "add_child") != 0)
	{
		failf(c, "unknown trait '%s:%s'", ns_name, fn);
		return;
	}

	ns = ensure_ns(c, "core");
	if (ns == NULL)
		return;

	if (ns_find_fn(ns, "add_child") == NULL && !ns_add_fn(ns, "add_child", -1, true))
		fail(c, "too many functions");

	c->file_add_child = true;
}

static void skip_balanced(Comp *c, TokKind open, TokKind close)
{
	int depth = 1;

	if (!at(c, open))
	{
		fail(c, open == TOK_LBRACE ? "expected '{'" : "expected '('");
		return;
	}

	advance(c);
	while (!c->failed && depth > 0 && !at(c, TOK_EOF))
	{
		if (at(c, open))
			depth++;
		else if (at(c, close) && !(close == TOK_RBRACE && c->tok.closes_str))
			depth--;
		advance(c);
	}
}

/* Consume `[n]` or `[]`. Star is invalid. need_int requires a size. */
static bool take_brackets(Comp *c, bool need_int, int32_t *out_len)
{
	if (!at(c, TOK_LBRACK))
		return false;

	advance(c);
	if (at(c, TOK_STAR))
	{
		fail(c, "use T[] not T[*]");
		return true;
	}

	if (at(c, TOK_INT))
	{
		if (out_len != NULL)
			*out_len = (int32_t)c->tok.value;
		advance(c);
	}
	else
	{
		if (need_int)
		{
			fail(c, "expected a size");
			return true;
		}

		if (out_len != NULL)
			*out_len = -1;
	}

	expect(c, TOK_RBRACK, "expected ']'");
	return true;
}

static void parse_packed_aliases(Comp *c, PackedType *p)
{
	expect(c, TOK_LBRACE, "expected '{'");
	for (;;)
	{
		char recv[STGE_NAME_MAX * 2];
		char alias[STGE_NAME_MAX];
		char tname[STGE_NAME_MAX];
		int32_t idx;

		skip_eol(c);
		if (c->failed || at(c, TOK_RBRACE) || at(c, TOK_EOF))
			break;

		if (!take_qname(c, recv, sizeof recv))
		{
			fail(c, "expected a field");
			return;
		}

		if (p == NULL || !name_eq(recv, p->name))
		{
			fail(c, "expected the type name");
			return;
		}

		expect(c, TOK_DOT, "expected '.'");
		if (!at(c, TOK_IDENT))
		{
			fail(c, "expected a field");
			return;
		}

		tok_copy(alias, sizeof alias, &c->tok);
		advance(c);
		expect(c, TOK_EQUAL, "expected '='");
		if (!at(c, TOK_IDENT))
		{
			fail(c, "expected the type name");
			return;
		}

		tok_copy(tname, sizeof tname, &c->tok);
		advance(c);
		if (p == NULL || !name_eq(tname, p->name))
		{
			fail(c, "expected the type name");
			return;
		}

		expect(c, TOK_LBRACK, "expected '['");
		if (!at(c, TOK_INT))
		{
			fail(c, "expected an index");
			return;
		}

		idx = (int32_t)c->tok.value;
		if (p != NULL && (idx < 0 || idx >= p->len))
		{
			failf(c, "index %d out of range for '%s'", (int)idx, p->name);
			return;
		}

		advance(c);
		expect(c, TOK_RBRACK, "expected ']'");
		packed_add_alias(p, alias, idx);
	}

	expect(c, TOK_RBRACE, "expected '}'");
}

static PackedType *record_packed_layout(Comp *c, const char *name,
					const char *elem, int32_t len)
{
	PackedType *p = ensure_packed(c, name);
	FieldKind fk;
	Op op;
	MemberKind mk;

	if (p == NULL)
		return NULL;

	if (elem != NULL)
		snprintf(p->elem, sizeof p->elem, "%s", elem);
	p->len = len;
	if (elem != NULL && type_name_kind(elem, &fk, &op, &mk))
		p->elem_kind = fk;
	else if (elem != NULL && elem[0] != '\0')
	{
		const PackedType *inner = find_packed(c, elem);

		p->elem_kind = inner != NULL ? inner->elem_kind : FK_INT;
	}

	return p;
}

static void emit_packed_template(Comp *c, const NodeRef *node, const PackedType *p)
{
	if (node == NULL || !packed_is_composite(p) || p->len < 1)
		return;

	emit_push_node(c, node->id);
	emit_push_str(c, p->name);
	emit_push(c, p->len);
	emit_u8(c, (uint8_t)packed_new_op(p->elem_kind));
	emit_u8(c, OP_POP);
}

static void skip_to_eol(Comp *c)
{
	int paren = 0;
	int brace = 0;

	while (!c->failed && !at(c, TOK_EOF))
	{
		if (paren == 0 && brace == 0 && at(c, TOK_EOL))
			return;

		if (at(c, TOK_LPAREN))
			paren++;
		else if (at(c, TOK_RPAREN))
			paren--;
		else if (at(c, TOK_LBRACE))
			brace++;
		else if (at(c, TOK_RBRACE) && !c->tok.closes_str)
			brace--;
		advance(c);
	}
}

static void skip_fn_tail(Comp *c)
{
	skip_balanced(c, TOK_LPAREN, TOK_RPAREN);
	if (at(c, TOK_ARROW))
	{
		advance(c);
		if (at(c, TOK_IDENT))
			advance(c);
		if (at(c, TOK_LBRACK))
			take_brackets(c, false, NULL);
	}

	{
		Lex saved_lex = c->lex;
		Tok saved_tok = c->tok;

		skip_eol(c);
		if (at(c, TOK_LBRACE))
			skip_balanced(c, TOK_LBRACE, TOK_RBRACE);
		else
		{
			lex_keep_buf(&c->lex, &saved_lex);
			c->tok = saved_tok;
		}
	}
}

static void declare_binding(Comp *c, const char *name)
{
	if (at(c, TOK_IDENT) && tok_eq(&c->tok, "extern"))
	{
		advance(c);
		if (at(c, TOK_IDENT))
			advance(c);
		if (at(c, TOK_LBRACE))
			skip_balanced(c, TOK_LBRACE, TOK_RBRACE);
		add_node(c, name);
		return;
	}

	if (word_is(c, "enum"))
	{
		advance(c);
		if (at(c, TOK_LBRACE))
			skip_balanced(c, TOK_LBRACE, TOK_RBRACE);
		add_node(c, name);
		return;
	}

	if (at(c, TOK_LBRACE))
	{
		skip_balanced(c, TOK_LBRACE, TOK_RBRACE);
		add_node(c, name);
		return;
	}

	if (at(c, TOK_LBRACK))
	{
		int32_t len = 1;

		take_brackets(c, true, &len);
		add_node(c, name);
		return;
	}

	if (at(c, TOK_IDENT) && look(c).kind == TOK_LBRACK)
	{
		int32_t len = 1;

		advance(c);
		take_brackets(c, true, &len);
		add_node(c, name);
		{
			Lex saved_lex = c->lex;
			Tok saved_tok = c->tok;

			skip_eol(c);
			if (at(c, TOK_LBRACE))
				skip_balanced(c, TOK_LBRACE, TOK_RBRACE);
			else
			{
				lex_keep_buf(&c->lex, &saved_lex);
				c->tok = saved_tok;
			}
		}
		return;
	}

	skip_to_eol(c);
}

static void declare_ns_def(Comp *c, const char *name)
{
	char qname[STGE_NAME_MAX * 2];
	char last[STGE_NAME_MAX];
	char parent[STGE_NAME_MAX * 2];

	if (!take_colon_path(c, name, qname, sizeof qname, last, sizeof last))
	{
		if (!c->failed)
			fail(c, "expected '::'");
		return;
	}

	if (!qual_parent(qname, parent, sizeof parent))
	{
		if (strlen(name) >= sizeof parent)
		{
			fail(c, "name too long");
			return;
		}

		strcpy(parent, name);
	}

	if (!at(c, TOK_EQUAL))
	{
		fail(c, "expected '='");
		return;
	}

	advance(c);
	if (at(c, TOK_IDENT) && tok_eq(&c->tok, "extern"))
	{
		advance(c);
		if (at(c, TOK_IDENT))
			advance(c);
		if (at(c, TOK_LPAREN))
		{
			record_fn_name(c, parent, last, peek_argc(c));
			skip_fn_tail(c);
			return;
		}

		if (at(c, TOK_LBRACE))
		{
			add_node(c, qname);
			skip_balanced(c, TOK_LBRACE, TOK_RBRACE);
		}

		return;
	}

	if (at(c, TOK_LPAREN))
	{
		record_fn_name(c, parent, last, peek_argc(c));
		skip_fn_tail(c);
		return;
	}

	if (word_is(c, "enum"))
	{
		ensure_ns(c, qname);
		add_node(c, qname);
		advance(c);
		if (at(c, TOK_LBRACE))
			skip_balanced(c, TOK_LBRACE, TOK_RBRACE);
		return;
	}

	if (at(c, TOK_LBRACE))
	{
		skip_balanced(c, TOK_LBRACE, TOK_RBRACE);
		add_node(c, qname);
		return;
	}

	skip_to_eol(c);
}

static void declare_field(Comp *c, const char *name)
{
	char field[STGE_NAME_MAX];

	if (!at(c, TOK_IDENT))
	{
		fail(c, "expected a field");
		return;
	}

	tok_copy(field, sizeof field, &c->tok);
	advance(c);
	if (at(c, TOK_EQUAL))
	{
		advance(c);
		if (at(c, TOK_LPAREN))
		{
			record_fn_name(c, name, field, peek_argc(c));
			skip_fn_tail(c);
			return;
		}

		if (at(c, TOK_IDENT) && tok_eq(&c->tok, "extern"))
		{
			advance(c);
			if (at(c, TOK_IDENT))
				advance(c);
			if (at(c, TOK_LPAREN))
			{
				record_fn_name(c, name, field, peek_argc(c));
				skip_fn_tail(c);
				return;
			}
		}
	}

	skip_to_eol(c);
}

static bool use_path_readable(const char *path)
{
	FILE *f;

	if (path == NULL || path[0] == '\0')
		return false;

	f = fopen(path, "rb");
	if (f == NULL)
		return false;

	fclose(f);
	return true;
}

static char *resolve_use_path_dup(const Comp *c, const char *rel)
{
	const char *slash;
	size_t dir_len;
	size_t rel_len;
	char *next;

	if (rel == NULL || rel[0] == '\0')
		return NULL;

	if (rel[0] == '/')
		return strdup(rel);

	slash = (c != NULL && c->path != NULL) ? strrchr(c->path, '/') : NULL;
	if (slash == NULL)
		return strdup(rel);

	dir_len = (size_t)(slash - c->path + 1);
	rel_len = strlen(rel);
	next = malloc(dir_len + rel_len + 1);
	if (next == NULL)
		return NULL;

	memcpy(next, c->path, dir_len);
	memcpy(next + dir_len, rel, rel_len + 1);
	if (use_path_readable(next) || !use_path_readable(rel))
		return next;

	free(next);
	return strdup(rel);
}

static bool mark_used(Comp *c, const char *path)
{
	int i;
	char *copy;

	if (path == NULL || path[0] == '\0')
		return false;

	for (i = 0; i < c->used_count; i++)
	{
		if (c->used[i].path != NULL && strcmp(c->used[i].path, path) == 0)
			return false;
	}

	if (c->used_count >= STGE_MAX_USE)
	{
		fail(c, "too many use files");
		return false;
	}

	copy = strdup(path);
	if (copy == NULL)
	{
		fail(c, "out of memory");
		return false;
	}

	c->used[c->used_count].path = copy;
	c->used_count++;
	return true;
}

static void compile_use(Comp *c)
{
	char *path;

	if (!at(c, TOK_STRING) || c->tok.hole)
	{
		fail(c, c->tok.hole ? "string interpolation is not allowed here" : "expected a string");
		return;
	}

	path = resolve_use_path_dup(c, c->tok.text);
	advance(c);
	if (path == NULL)
	{
		fail(c, "out of memory");
		return;
	}

	if (!mark_used(c, path))
	{
		free(path);
		return;
	}

	include_pass(c, path, false);
	free(path);
}

static void declare_top_statement(Comp *c)
{
	char name[STGE_NAME_MAX];
	Lex saved_lex;
	Tok saved_tok;

	if (!at(c, TOK_IDENT))
	{
		fail(c, "expected a name");
		return;
	}

	if (word_is(c, "use") && look(c).kind == TOK_STRING)
	{
		advance(c);
		compile_use(c);
		return;
	}

	if (word_is(c, "while"))
	{
		advance(c);
		while (!c->failed && !at(c, TOK_EOF) && !at(c, TOK_LBRACE))
		{
			if (at(c, TOK_EOL))
			{
				skip_eol(c);
				break;
			}
			if (at(c, TOK_LPAREN))
				skip_balanced(c, TOK_LPAREN, TOK_RPAREN);
			else
				advance(c);
		}
		skip_eol(c);
		if (at(c, TOK_LBRACE))
			skip_balanced(c, TOK_LBRACE, TOK_RBRACE);
		else
			skip_to_eol(c);
		return;
	}

	if (next_is_call(c))
	{
		skip_to_eol(c);
		return;
	}

	saved_lex = c->lex;
	saved_tok = c->tok;
	tok_copy(name, sizeof name, &c->tok);
	advance(c);

	if (at(c, TOK_EQUAL))
	{
		advance(c);
		declare_binding(c, name);
		return;
	}

	if (at_compound(c))
	{
		skip_to_eol(c);
		return;
	}

	if (at(c, TOK_COLONCOLON))
	{
		lex_keep_buf(&c->lex, &saved_lex);
		c->tok = saved_tok;
		advance(c);
		declare_ns_def(c, name);
		return;
	}

	if (at(c, TOK_DOT))
	{
		advance(c);
		declare_field(c, name);
		return;
	}

	fail(c, "expected '=', '.' or '::'");
}

static void declare_statements(Comp *c)
{
	for (;;)
	{
		skip_eol(c);
		if (c->failed || at(c, TOK_EOF))
			break;

		declare_top_statement(c);
		if (c->failed)
			break;

		if (!at(c, TOK_EOF))
			expect(c, TOK_EOL, "expected end of line");
	}
}

static void compile_statements(Comp *c)
{
	for (;;)
	{
		skip_eol(c);
		if (c->failed || at(c, TOK_EOF))
			break;

		compile_top_statement(c);
		if (c->failed)
			break;

		if (!at(c, TOK_EOF))
			expect(c, TOK_EOL, "expected end of line");
	}
}

static void compile_file_body(Comp *c)
{
	/* Entry already has the first token in c->tok; lex.pos is past it. */
	size_t start_pos = c->tok.start;
	int start_line = c->tok.line > 0 ? c->tok.line : 1;
	int i;

	declare_statements(c);
	if (c->failed)
		return;

	for (i = 0; i < c->used_count; i++)
	{
		free(c->used[i].path);
		c->used[i].path = NULL;
	}
	c->used_count = 0;

	/* Replay from the first token; keep the growable lex buf. */
	c->lex.pos = start_pos;
	c->lex.line = start_line;
	c->lex.cont_string = false;
	c->lex.interp_sp = 0;
	advance(c);
	compile_statements(c);
}

static void compile_include(Comp *c, const char *path)
{
	char *src;
	size_t len = 0;
	Lex saved_lex;
	Tok saved_tok;
	const char *saved_path;

	if (c->include_depth >= STGE_MAX_INCLUDE)
	{
		fail(c, "too many includes");
		return;
	}

	src = read_file(path, &len);
	if (src == NULL)
	{
		failf(c, "could not read include '%s'", path);
		return;
	}

	saved_lex = c->lex;
	saved_tok = c->tok;
	saved_path = c->path;

	c->path = path;
	c->lex.src = src;
	c->lex.len = len;
	c->lex.pos = 0;
	c->lex.line = 1;
	c->lex.cont_string = false;
	c->lex.interp_sp = 0;
	c->lex.buf = NULL;
	c->lex.buf_cap = 0;
	c->lex.buf_len = 0;
	c->lex.persist = NULL;
	c->lex.persist_count = 0;
	c->lex.persist_cap = 0;
	c->include_depth++;

	advance(c);
	compile_file_body(c);

	c->include_depth--;
	free(c->lex.buf);
	lex_persist_free(&c->lex);
	c->lex = saved_lex;
	c->tok = saved_tok;
	c->path = saved_path;
	free(src);
}

static void include_pass(Comp *c, const char *path, bool compiling)
{
	char *src;
	size_t len = 0;
	Lex saved_lex;
	Tok saved_tok;
	const char *saved_path;

	if (c->include_depth >= STGE_MAX_INCLUDE)
	{
		fail(c, "too many includes");
		return;
	}

	src = read_file(path, &len);
	if (src == NULL)
	{
		failf(c, "could not read include '%s'", path);
		return;
	}

	saved_lex = c->lex;
	saved_tok = c->tok;
	saved_path = c->path;

	c->path = path;
	c->lex.src = src;
	c->lex.len = len;
	c->lex.pos = 0;
	c->lex.line = 1;
	c->lex.cont_string = false;
	c->lex.interp_sp = 0;
	c->lex.buf = NULL;
	c->lex.buf_cap = 0;
	c->lex.buf_len = 0;
	c->lex.persist = NULL;
	c->lex.persist_count = 0;
	c->lex.persist_cap = 0;
	c->include_depth++;

	advance(c);
	if (compiling)
		compile_statements(c);
	else
		declare_statements(c);

	c->include_depth--;
	free(c->lex.buf);
	lex_persist_free(&c->lex);
	c->lex = saved_lex;
	c->tok = saved_tok;
	c->path = saved_path;
	free(src);
}

static bool is_load_name(const char *name)
{
	return strcmp(name, "load") == 0 || strcmp(name, "stge_load") == 0;
}

static void compile_load_call(Comp *c)
{
	char *first;

	expect(c, TOK_LPAREN, "expected '('");
	if (!at(c, TOK_STRING) || c->tok.hole)
	{
		fail(c, c->tok.hole ? "string interpolation is not allowed here" : "expected a string");
		return;
	}

	first = strdup(c->tok.text != NULL ? c->tok.text : "");
	advance(c);
	if (first == NULL)
	{
		fail(c, "out of memory");
		return;
	}

	if (at(c, TOK_COMMA))
	{
		free(first);
		fail(c, "runtime load of another program is not supported");
		return;
	}

	expect(c, TOK_RPAREN, "expected ')'");

	if (strstr(first, "::") != NULL)
		import_trait(c, first);
	else
		fail(c, "use \"path\" to import a script");
	free(first);
}

static bool is_ns_builtin(const NsRef *ns, const char *fn)
{
	const ObjFn *f = ns_find_fn(ns, fn);

	return f != NULL && f->pc < 0;
}

static bool emit_construct(Comp *c, const char *type_name)
{
	const NsRef *ns = find_ns(c, type_name);
	const NodeRef *type = find_node(c, type_name);
	const ObjFn *ctor;
	int obj_id = -1;
	int argc;

	if (type != NULL)
		obj_id = type->id;
	else if (ns != NULL)
		obj_id = ns->obj_id;

	argc = peek_argc(c);
	ctor = ns != NULL ? ns_find_fn_arity(ns, "new", argc) : NULL;
	if (ctor == NULL && ns != NULL && ns_find_fn(ns, "new") != NULL)
	{
		failf(c, "wrong number of arguments: '%s' constructor", type_name);
		return true;
	}

	if (ctor == NULL || obj_id == -1)
		return false;

	emit_script_call(c, obj_id, ctor->pc, ctor->has_arg, true, "new", NULL,
			 ctor->ret_type);
	return true;
}

static void compile_call(Comp *c)
{
	char recv[STGE_NAME_MAX];
	Tok recv_tok;
	const Method *m;
	bool is_static;

	if (c->failed)
		return;

	if (!at(c, TOK_IDENT))
	{
		fail(c, "expected a receiver");
		return;
	}

	recv_tok = c->tok;
	tok_copy(recv, sizeof recv, &c->tok);
	advance(c);

	if (at(c, TOK_LPAREN))
	{
		if (strcmp(recv, "byte") == 0 || strcmp(recv, "uint") == 0
		    || strcmp(recv, "int") == 0)
		{
			expect(c, TOK_LPAREN, "expected '('");
			if (at(c, TOK_RPAREN))
				emit_push(c, 0);
			else
				compile_expr(c);
			expect(c, TOK_RPAREN, "expected ')'");
			c->expr_kind = EK_INT;
			snprintf(c->last_ret_type, sizeof c->last_ret_type, "%s", recv);
			return;
		}

		if (is_load_name(recv) || (strncmp(recv, "stge_", 5) == 0 && is_load_name(recv + 5)))
		{
			compile_load_call(c);
			return;
		}

		if (strcmp(recv, "to_str") == 0)
		{
			expect(c, TOK_LPAREN, "expected '('");
			compile_stringify(c);
			expect(c, TOK_RPAREN, "expected ')'");
			c->expr_kind = EK_STR;
			return;
		}

		if (strcmp(recv, "print") == 0 || strcmp(recv, "stge_print") == 0)
		{
			expect(c, TOK_LPAREN, "expected '('");
			compile_stringify(c);
			expect(c, TOK_RPAREN, "expected ')'");
			emit_u8(c, OP_PRINT);
			c->expr_kind = EK_INT;
			return;
		}

		if (emit_construct(c, recv))
			return;

		if (c->file_add_child && strcmp(recv, "add_child") == 0)
		{
			if (c->self < 0)
			{
				fail(c, "add_child needs a node");
				return;
			}

			emit_u8(c, OP_PUSH_SELF);
			compile_args_and_op(c, find_method("add_child"), 0);
			return;
		}

		m = find_method(recv);
		if (m == NULL && strncmp(recv, "stge_", 5) == 0)
			m = find_method(recv + 5);

		if (m == NULL)
		{
			failf(c, "unknown function '%s'", recv);
			return;
		}

		compile_args_and_op(c, m, m->is_static ? 0 : 1);
		return;
	}

	if (at(c, TOK_COLONCOLON))
	{
		char ns_name[STGE_NAME_MAX * 2];
		char fn[STGE_NAME_MAX];
		const NsRef *ns;
		const ObjFn *sf;

		memcpy(ns_name, recv, STGE_NAME_MAX);
		ns_name[STGE_NAME_MAX] = '\0';
		for (;;)
		{
			char part[STGE_NAME_MAX];
			Tok nxt;

			advance(c);
			if (!at(c, TOK_IDENT))
			{
				fail(c, "expected a name");
				return;
			}

			tok_copy(part, sizeof part, &c->tok);
			advance(c);
			nxt = look(c);
			if (at(c, TOK_LPAREN) || nxt.kind == TOK_LPAREN)
			{
				memcpy(fn, part, sizeof fn);
				break;
			}

			if (!at(c, TOK_COLONCOLON))
			{
				fail(c, "expected '('");
				return;
			}

			{
				char next_path[STGE_NAME_MAX * 2];

				if (!join_ns(next_path, sizeof next_path, ns_name, part))
				{
					fail(c, "name too long");
					return;
				}

				memcpy(ns_name, next_path, sizeof next_path);
			}
		}

		if (strcmp(fn, "new") == 0)
		{
			failf(c, "construct with %s()", ns_name);
			return;
		}

		{
			char type_name[STGE_NAME_MAX * 2];

			if (join_ns(type_name, sizeof type_name, ns_name, fn)
			    && emit_construct(c, type_name))
				return;
		}

		ns = find_ns(c, ns_name);
		sf = ns != NULL ? ns_find_fn(ns, fn) : NULL;
		if (sf == NULL)
		{
			failf(c, "unknown function '%s::%s'", ns_name, fn);
			return;
		}

		if (sf->is_extern)
		{
			int argc = 0;

			expect(c, TOK_LPAREN, "expected '('");
			while (!c->failed && !at(c, TOK_RPAREN) && argc < 65535)
			{
				if (argc > 0)
					expect(c, TOK_COMMA, "expected ','");
				compile_expr(c);
				argc++;
			}
			expect(c, TOK_RPAREN, "expected ')'");
			if (sf->native_id == stge_native_find("stge_write"))
				emit_u8(c, OP_WRITE);
			else
				emit_extern(c, sf->native_id, argc);
			if (strcmp(fn, "to_str") == 0 || strcmp(fn, "concat") == 0)
			{
				c->expr_kind = EK_OBJ;
				snprintf(c->last_ret_type, sizeof c->last_ret_type, "String");
			}
			else
				c->expr_kind = EK_INT;
			return;
		}

		if (ns->obj_id == -1)
		{
			failf(c, "unknown function '%s::%s'", ns_name, fn);
			return;
		}

		emit_script_call(c, ns->obj_id, sf->pc, sf->has_arg, true, fn, NULL,
				 sf->ret_type);
		return;
	}

	expect(c, TOK_DOT, "expected '.'");

	if (c->failed || !at(c, TOK_IDENT))
	{
		fail(c, "expected a method");
		return;
	}

	if (look(c).kind == TOK_DOT)
	{
		char fields[STGE_MAX_PATH][STGE_NAME_MAX];
		char method[STGE_NAME_MAX];
		char owner[STGE_NAME_MAX];
		const NodeRef *node;
		const ObjFn *sf = NULL;
		int n = 0;
		int i;
		int pc = -1;

		tok_copy(fields[0], sizeof fields[0], &c->tok);
		n = 1;
		advance(c);
		while (at(c, TOK_DOT))
		{
			advance(c);
			if (!at(c, TOK_IDENT))
			{
				fail(c, "expected a method");
				return;
			}

			if (look(c).kind == TOK_DOT)
			{
				if (n >= STGE_MAX_PATH)
				{
					fail(c, "name too long");
					return;
				}

				tok_copy(fields[n], sizeof fields[0], &c->tok);
				n++;
				advance(c);
				continue;
			}

			tok_copy(method, sizeof method, &c->tok);
			advance(c);
			break;
		}

		snprintf(owner, sizeof owner, "%s", recv);
		for (i = 0; i < n; i++)
		{
			const char *t = lookup_field_type(c, owner, fields[i]);

			if (t != NULL && t[0] != '\0')
				snprintf(owner, sizeof owner, "%s", t);
		}

		node = find_node(c, owner);
		if (node != NULL)
		{
			for (i = 0; i < node->fn_count; i++)
			{
				if (strcmp(node->fns[i].name, method) == 0)
				{
					sf = &node->fns[i];
					break;
				}
			}
		}

		if (sf == NULL)
		{
			const NsRef *ns = find_ns(c, owner);

			sf = ns != NULL ? ns_find_fn(ns, method) : NULL;
		}

		if (sf == NULL)
		{
			if (!name_in_scope(c, recv))
			{
				c->tok = recv_tok;
				failf(c, "unknown name '%s'", recv);
			}
			else
				failf(c, "unknown method '%s' on '%s'", method, owner);
			return;
		}

		pc = sf->pc;
		{
			int argc = 0;

			expect(c, TOK_LPAREN, "expected '('");
			while (!c->failed && !at(c, TOK_RPAREN) && argc < 65535)
			{
				if (argc > 0)
					expect(c, TOK_COMMA, "expected ','");
				compile_expr(c);
				argc++;
			}
			expect(c, TOK_RPAREN, "expected ')'");
			emit_recv(c, recv);
			for (i = 0; i < n; i++)
				emit_get_field(c, fields[i]);
			emit_push(c, pc);
			if (pc < 0 && c->call_patch_count < STGE_MAX_CALL_PATCH)
			{
				VmProg *s = stge_prog();

				if (s != NULL)
				{
					c->call_patch_at[c->call_patch_count] = s->code_len - 4;
					c->call_patch_obj[c->call_patch_count] = node != NULL ? node->id : 0;
					strcpy(c->call_patch_fn[c->call_patch_count], method);
					c->call_patch_count++;
				}
			}

			emit_call(c, OP_CALL, argc);
			c->expr_kind = EK_INT;
			return;
		}
	}

	m = find_method(tok_cstr(c, &c->tok));
	if (m == NULL)
	{
		char fn[STGE_NAME_MAX];
		const NodeRef *node;
		int i;

		tok_copy(fn, sizeof fn, &c->tok);
		advance(c);

		{
			const NsRef *ns = find_ns(c, recv);
			const ObjFn *sf = ns != NULL ? ns_find_fn(ns, fn) : NULL;

			if (sf != NULL && sf->is_extern && sf->native_id >= 0)
			{
				int argc = 0;

				expect(c, TOK_LPAREN, "expected '('");
				while (!c->failed && !at(c, TOK_RPAREN) && argc < 65535)
				{
					if (argc > 0)
						expect(c, TOK_COMMA, "expected ','");
					compile_expr(c);
					argc++;
				}
				expect(c, TOK_RPAREN, "expected ')'");
				if (sf->native_id == stge_native_find("stge_write"))
					emit_u8(c, OP_WRITE);
				else
					emit_extern(c, sf->native_id, argc);
				c->expr_kind = EK_INT;
				return;
			}
		}

		if (strcmp(fn, "append") == 0 || strcmp(fn, "add") == 0)
		{
			if (is_local(c, recv))
			{
				expect(c, TOK_LPAREN, "expected '('");
				if (c->self < 0)
				{
					fail(c, "self outside a body");
					return;
				}

				emit_u8(c, OP_PUSH_SELF);
				emit_push_str(c, recv);
				compile_expr(c);
				expect(c, TOK_RPAREN, "expected ')'");
				emit_u8(c, OP_DYN_ADD);
				c->expr_kind = EK_INT;
				return;
			}
		}

		if (at(c, TOK_DOT))
		{
			Lex saved_lex = c->lex;
			Tok saved_tok = c->tok;

			advance(c);
			if (at(c, TOK_IDENT)
			    && (tok_eq(&c->tok, "add") || tok_eq(&c->tok, "append")))
			{
				advance(c);
				expect(c, TOK_LPAREN, "expected '('");
				emit_recv(c, recv);
				emit_push_str(c, fn);
				compile_expr(c);
				expect(c, TOK_RPAREN, "expected ')'");
				emit_u8(c, OP_DYN_ADD);
				c->expr_kind = EK_INT;
				return;
			}

			lex_keep_buf(&c->lex, &saved_lex);
			c->tok = saved_tok;
		}

		if (c->has_param && strcmp(recv, c->param) == 0 && c->param_type[0] != '\0')
		{
			const NodeRef *ptype = find_node(c, c->param_type);
			int i;

			if (ptype != NULL)
			{
				for (i = 0; i < ptype->fn_count; i++)
				{
					if (strcmp(ptype->fns[i].name, fn) == 0 && !ptype->fns[i].is_extern)
					{
						int argc = 0;

						expect(c, TOK_LPAREN, "expected '('");
						while (!c->failed && !at(c, TOK_RPAREN)
						       && argc < 65535)
						{
							if (argc > 0)
								expect(c, TOK_COMMA, "expected ','");
							compile_expr(c);
							argc++;
						}
						expect(c, TOK_RPAREN, "expected ')'");
						emit_push_arg(c, 0);
						emit_push(c, ptype->fns[i].pc);
						emit_call(c, OP_CALL, argc);
						c->expr_kind = EK_INT;
						return;
					}
				}
			}
		}

		if (strcmp(recv, "self") == 0)
		{
			node = NULL;
			for (i = 0; i < c->node_count; i++)
			{
				if (c->nodes[i].id == c->self)
				{
					node = &c->nodes[i];
					break;
				}
			}
		}
		else
			node = find_node(c, recv);

		if (node == NULL)
		{
			int slot = find_slot_local(c, recv);
			const NodeRef *stype = NULL;

			if (slot >= 0 && c->slot_types[slot][0] != '\0')
				stype = find_node(c, c->slot_types[slot]);
			if (stype != NULL)
			{
				for (i = 0; i < stype->fn_count; i++)
				{
					if (strcmp(stype->fns[i].name, fn) == 0
					    && !stype->fns[i].is_extern
					    && stype->fns[i].self_param == 0)
					{
						emit_get_local(c, slot);
						c->expr_kind = EK_INT;
						snprintf(c->last_ret_type, sizeof c->last_ret_type,
							 "%s", stype->name);
						if (emit_stacked_instance_call(c, stype->name, fn))
							return;
						failf(c, "unknown method '%s' on '%s'", fn, recv);
						return;
					}
				}
				node = stype;
			}
		}

		if (node == NULL)
		{
			if (strcmp(fn, "to_str") == 0)
			{
				expect(c, TOK_LPAREN, "expected '('");
				expect(c, TOK_RPAREN, "expected ')'");
				emit_recv(c, recv);
				emit_u8(c, OP_STR_OBJ);
				c->expr_kind = EK_OBJ;
				snprintf(c->last_ret_type, sizeof c->last_ret_type, "String");
				return;
			}

			if (!name_in_scope(c, recv))
			{
				c->tok = recv_tok;
				failf(c, "unknown name '%s'", recv);
			}
			else
				failf(c, "unknown method '%s' on '%s'", fn, recv);
			return;
		}

		for (i = 0; i < node->fn_count; i++)
		{
			if (strcmp(node->fns[i].name, fn) == 0 && !node->fns[i].is_extern)
			{
				emit_script_call(c, node->id, node->fns[i].pc, node->fns[i].has_arg,
						 false, fn, recv, node->fns[i].ret_type);
				return;
			}
		}

		if (strcmp(fn, "to_str") == 0)
		{
			expect(c, TOK_LPAREN, "expected '('");
			expect(c, TOK_RPAREN, "expected ')'");
			emit_recv(c, recv);
			emit_u8(c, OP_STR_OBJ);
			c->expr_kind = EK_OBJ;
			snprintf(c->last_ret_type, sizeof c->last_ret_type, "String");
			return;
		}

		{
			const NsRef *ns = find_ns(c, recv);
			const ObjFn *sf = ns != NULL ? ns_find_fn(ns, fn) : NULL;

			if (sf != NULL && ns->obj_id != -1 && !is_ns_builtin(ns, fn))
			{
				if (sf->is_extern)
				{
					fail(c, "not visible outside the object");
					return;
				}

				emit_script_call(c, ns->obj_id, sf->pc, sf->has_arg, false, fn, recv,
						 sf->ret_type);
				return;
			}
		}

		failf(c, "unknown method '%s' on '%s'", fn, recv);
		return;
	}
	advance(c);

	is_static = strcmp(recv, "Stge") == 0 || strcmp(recv, "stge") == 0
		|| strcmp(recv, "Game") == 0 || strcmp(recv, "Camera") == 0;

	if (is_static != m->is_static)
	{
		fail(c, is_static ? "method needs a node" : "method is not on a node");
		return;
	}

	if (!is_static)
	{
		if (strcmp(recv, "self") == 0)
		{
			if (c->self < 0)
			{
				fail(c, "self outside a body");
				return;
			}

			emit_u8(c, OP_PUSH_SELF);
		}
		else
		{
			const NodeRef *node = find_node(c, recv);

			if (node == NULL)
			{
				failf(c, "unknown node '%s'", recv);
				return;
			}

			emit_push_node(c, node->id);
		}
	}

	if (m->op == OP_CAM_CENTER_ON)
	{
		compile_center_on_arg(c);
		emit_u8(c, OP_CAM_CENTER_ON);
		return;
	}

	compile_args_and_op(c, m, 0);
}

/* ---- statements ---------------------------------------------------------- */

static int compile_body_statement(Comp *c);

static void compile_condition(Comp *c)
{
	compile_expr(c);
}

static void compile_while(Comp *c)
{
	size_t loop;
	size_t jz;
	size_t back;

	advance(c);
	loop = stge_prog() != NULL ? stge_prog()->code_len : 0;
	compile_condition(c);
	skip_eol(c);
	jz = emit_jump(c, OP_JZ);
	if (compile_body_statement(c))
		emit_u8(c, OP_POP);
	emit_u8(c, OP_WAIT_TICK);
	back = emit_jump(c, OP_JMP);
	patch_jump_to(c, back, loop);
	patch_jump(c, jz);
}

static void compile_block(Comp *c)
{
	expect(c, TOK_LBRACE, "expected '{'");

	for (;;)
	{
		skip_eol(c);

		if (c->failed)
			return;

		if (at(c, TOK_RBRACE))
			break;

		if (at(c, TOK_EOF))
		{
			fail(c, "unterminated body");
			return;
		}

		if (compile_body_statement(c))
			emit_u8(c, OP_POP);

		if (c->failed)
			return;

		if (!at(c, TOK_RBRACE))
			expect(c, TOK_EOL, "expected end of line");
	}

	advance(c);
}

static bool at_ctor(const Comp *c, const char *name)
{
	return at(c, TOK_IDENT) && tok_eq(&c->tok, name) && look(c).kind == TOK_LPAREN;
}

static bool type_name_kind(const char *name, FieldKind *fk, Op *op, MemberKind *mk)
{
	if (strcmp(name, "int") == 0)
	{
		*fk = FK_INT;
		*op = OP_NEW_INT;
		*mk = MEMBER_INT;
		return true;
	}
	if (strcmp(name, "uint") == 0)
	{
		*fk = FK_UINT;
		*op = OP_NEW_UINT;
		*mk = MEMBER_UINT;
		return true;
	}
	if (strcmp(name, "float") == 0)
	{
		*fk = FK_FLOAT;
		*op = OP_NEW_FLOAT;
		*mk = MEMBER_FLOAT;
		return true;
	}
	if (strcmp(name, "double") == 0)
	{
		*fk = FK_DOUBLE;
		*op = OP_NEW_DOUBLE;
		*mk = MEMBER_DOUBLE;
		return true;
	}
	if (strcmp(name, "str") == 0)
	{
		*fk = FK_STR;
		*op = OP_NEW_STR;
		*mk = MEMBER_STRING;
		return true;
	}
	if (strcmp(name, "byte") == 0)
	{
		*fk = FK_BYTE;
		*op = OP_NEW_BYTE;
		*mk = MEMBER_BYTE;
		return true;
	}

	return false;
}

static bool compile_array_ctor(Comp *c, const char *recv, const char *member)
{
	FieldKind fk;
	Op op;
	MemberKind mk;
	char elem[STGE_NAME_MAX];
	Lex saved_lex;
	Tok saved_tok;

	if (!at(c, TOK_IDENT))
		return false;

	if (look(c).kind != TOK_LBRACK && look(c).kind != TOK_COLONCOLON)
		return false;

	saved_lex = c->lex;
	saved_tok = c->tok;
	tok_copy(elem, sizeof elem, &c->tok);
	advance(c);
	while (at(c, TOK_COLONCOLON))
	{
		advance(c);
		if (!at(c, TOK_IDENT))
		{
			lex_keep_buf(&c->lex, &saved_lex);
			c->tok = saved_tok;
			return false;
		}

		tok_copy(elem, sizeof elem, &c->tok);
		advance(c);
	}

	if (!at(c, TOK_LBRACK))
	{
		lex_keep_buf(&c->lex, &saved_lex);
		c->tok = saved_tok;
		return false;
	}

	if (!type_name_kind(elem, &fk, &op, &mk))
	{
		fk = FK_OBJ;
		op = OP_NEW_ARRAY;
		mk = MEMBER_ARRAY;
	}

	advance(c);
	if (at(c, TOK_STAR))
	{
		fail(c, "use T[] not T[*]");
		return true;
	}

	if (at(c, TOK_RBRACK))
	{
		advance(c);

		emit_recv(c, recv);
		emit_push_str(c, member);
		emit_u8(c, OP_NEW_DYN);
		emit_u8(c, (uint8_t)mk);
		emit_u8(c, OP_POP);
		record_field_array(c, recv, member, fk, -1, true);
		return true;
	}

	if (at(c, TOK_INT) && c->tok.value < 0)
	{
		fail(c, "array size must be >= 0");
		return true;
	}

	emit_recv(c, recv);
	emit_push_str(c, member);
	if (at(c, TOK_INT))
	{
		int32_t n = c->tok.value;

		emit_push(c, n);
		emit_u8(c, (uint8_t)op);
		emit_u8(c, OP_POP);
		record_field_array(c, recv, member, fk, n, false);
		advance(c);
	}
	else
	{
		compile_expr(c);
		emit_u8(c, (uint8_t)op);
		emit_u8(c, OP_POP);
		record_field_array(c, recv, member, fk, -1, false);
	}

	expect(c, TOK_RBRACK, "expected ']'");
	return true;
}

static void compile_len_ctor(Comp *c, const char *recv, const char *member, Op op, FieldKind fk)
{
	advance(c);
	expect(c, TOK_LPAREN, "expected '('");
	emit_recv(c, recv);
	emit_push_str(c, member);
	emit_push(c, 1);
	emit_u8(c, (uint8_t)op);
	emit_u8(c, OP_POP);
	if (!at(c, TOK_RPAREN))
	{
		ExprKind got;

		emit_recv(c, recv);
		emit_push_str(c, member);
		emit_push(c, 0);
		got = compile_expr(c);
		emit_coerce(c, got, field_expr_kind(fk));
		emit_u8(c, (uint8_t)mem_set_op(fk));
		emit_u8(c, OP_POP);
	}

	expect(c, TOK_RPAREN, "expected ')'");
	record_field_kind(c, recv, member, fk);
}

static bool take_ns_value(Comp *c)
{
	char ns_name[STGE_NAME_MAX];
	char fn[STGE_NAME_MAX];
	const NsRef *ns;
	Lex saved_lex;
	Tok saved_tok;

	if (!at(c, TOK_IDENT) || look(c).kind != TOK_DOT)
		return false;

	saved_lex = c->lex;
	saved_tok = c->tok;

	tok_copy(ns_name, sizeof ns_name, &c->tok);
	advance(c);
	advance(c);
	if (!at(c, TOK_IDENT))
	{
		lex_keep_buf(&c->lex, &saved_lex);
		c->tok = saved_tok;
		return false;
	}

	tok_copy(fn, sizeof fn, &c->tok);
	advance(c);

	if (at(c, TOK_LPAREN))
	{
		lex_keep_buf(&c->lex, &saved_lex);
		c->tok = saved_tok;
		return false;
	}

	ns = find_ns(c, ns_name);
	if (ns == NULL || !is_ns_builtin(ns, fn))
	{
		lex_keep_buf(&c->lex, &saved_lex);
		c->tok = saved_tok;
		return false;
	}

	return true;
}

static void compile_member_assign(Comp *c, const char *recv, const char *member)
{
	if (c->failed)
		return;

	if (strcmp(recv, "Camera") == 0)
	{
		if (strcmp(member, "x") == 0)
			emit_push(c, 0);
		else if (strcmp(member, "y") == 0)
			emit_push(c, 1);
		else if (strcmp(member, "pos") == 0)
			compile_index_or_zero(c);
		else
		{
			failf(c, "unknown field '%s' on 'Camera'", member);
			return;
		}

		expect(c, TOK_EQUAL, "expected '='");
		compile_expr(c);
		emit_u8(c, OP_SET_CAM);
		emit_u8(c, OP_POP);
		return;
	}

	if (is_stge_host(recv))
	{
		if (strcmp(member, "tickrate") == 0)
		{
			expect(c, TOK_EQUAL, "expected '='");
			compile_expr(c);
			emit_u8(c, OP_SET_TICKRATE);
			emit_u8(c, OP_POP);
			return;
		}

		if (strcmp(member, "print_out") == 0)
		{
			expect(c, TOK_EQUAL, "expected '='");
			compile_expr(c);
			emit_u8(c, OP_SET_PRINT_OUT);
			emit_u8(c, OP_POP);
			return;
		}

		failf(c, "unknown field '%s' on 'stge'", member);
		return;
	}

	{
		const PackedType *p = packed_of_name(c, recv);
		int idx = packed_alias_index(p, member);

		if (packed_is_composite(p) && idx >= 0)
		{
			expect(c, TOK_EQUAL, "expected '='");
			emit_recv(c, recv);
			emit_push_str(c, p->name);
			emit_push(c, idx);
			compile_expr(c);
			emit_u8(c, OP_SET_MEM);
			emit_u8(c, OP_POP);
			return;
		}
	}

	if (at(c, TOK_LBRACK))
	{
		FieldKind fk = lookup_field_kind(c, recv, member);
		ExprKind got;

		emit_recv(c, recv);
		emit_push_str(c, member);
		compile_index_checked(c, recv, member);
		expect(c, TOK_EQUAL, "expected '='");
		got = compile_expr(c);
		emit_coerce(c, got, field_expr_kind(fk));
		emit_u8(c, (uint8_t)mem_set_op(fk));
		emit_u8(c, OP_POP);
		return;
	}

	expect(c, TOK_EQUAL, "expected '='");
	if (c->failed)
		return;

	if (is_str_bytes(member) && at(c, TOK_IDENT) && look(c).kind == TOK_DOT)
	{
		char src[STGE_NAME_MAX];
		char srcf[STGE_NAME_MAX];

		tok_copy(src, sizeof src, &c->tok);
		advance(c);
		advance(c);
		if (!at(c, TOK_IDENT) || !is_str_bytes(tok_cstr(c, &c->tok)))
		{
			fail(c, "expected buffer");
			return;
		}

		tok_copy(srcf, sizeof srcf, &c->tok);
		advance(c);
		emit_recv(c, recv);
		emit_push_str(c, member);
		emit_u8(c, OP_NEW_DYN);
		emit_u8(c, (uint8_t)MEMBER_BYTE);
		emit_u8(c, OP_POP);
		emit_recv(c, recv);
		emit_push_str(c, member);
		if (c->has_param && strcmp(src, c->param) == 0)
			emit_push_arg(c, 0);
		else
			emit_recv(c, src);
		emit_push_str(c, srcf);
		emit_u8(c, OP_DYN_COPY);
		emit_u8(c, OP_POP);
		record_field_array(c, recv, member, FK_BYTE, -1, true);
		return;
	}

	if (take_ns_value(c))
		return;

	if (compile_array_ctor(c, recv, member))
		return;

	if (at_ctor(c, "byte"))
	{
		compile_len_ctor(c, recv, member, OP_NEW_BYTE, FK_BYTE);
		return;
	}

	if (at_ctor(c, "int"))
	{
		compile_len_ctor(c, recv, member, OP_NEW_INT, FK_INT);
		return;
	}

	if (at_ctor(c, "uint"))
	{
		compile_len_ctor(c, recv, member, OP_NEW_UINT, FK_UINT);
		return;
	}

	if (at_ctor(c, "float"))
	{
		compile_len_ctor(c, recv, member, OP_NEW_FLOAT, FK_FLOAT);
		return;
	}

	if (at_ctor(c, "double"))
	{
		compile_len_ctor(c, recv, member, OP_NEW_DOUBLE, FK_DOUBLE);
		return;
	}

	if (at_ctor(c, "str"))
	{
		compile_len_ctor(c, recv, member, OP_NEW_STR, FK_STR);
		return;
	}

	if (strcmp(member, "display") == 0)
	{
		emit_recv(c, recv);
		compile_atom(c);
		emit_u8(c, OP_SET_DISPLAY);
		emit_u8(c, OP_POP);
		return;
	}

	{
		FieldKind fk = lookup_field_kind(c, recv, member);
		ExprKind got;

		if (field_is_vec(fk) && at(c, TOK_IDENT) && look(c).kind == TOK_DOT)
		{
			char src[STGE_NAME_MAX];
			char srcf[STGE_NAME_MAX];
			FieldKind sk;
			int i;
			int n = vec_len(fk);

			tok_copy(src, sizeof src, &c->tok);
			advance(c);
			advance(c);
			if (!at(c, TOK_IDENT))
			{
				fail(c, "expected a field");
				return;
			}

			tok_copy(srcf, sizeof srcf, &c->tok);
			sk = lookup_field_kind(c, src, srcf);
			if (sk == fk)
			{
				advance(c);
				for (i = 0; i < n; i++)
				{
					emit_recv(c, recv);
					emit_push_str(c, member);
					emit_push(c, i);
					emit_recv(c, src);
					emit_push_str(c, srcf);
					emit_push(c, i);
					emit_u8(c, (uint8_t)mem_get_op(fk));
					emit_u8(c, (uint8_t)mem_set_op(fk));
					emit_u8(c, OP_POP);
				}
				return;
			}

			fail(c, "expected a value");
			return;
		}

		emit_recv(c, recv);
		emit_push_str(c, member);
		got = compile_expr(c);
		{
			const PackedType *rp = find_packed(c, c->last_ret_type);

			if (packed_is_composite(rp))
			{
				emit_push_str(c, rp->name);
				emit_u8(c, OP_MEM_COPY);
				emit_u8(c, OP_POP);
				record_field_packed(c, recv, member, rp);
				return;
			}
		}

		if (got == EK_OBJ)
		{
			if (c->last_ret_type[0] != '\0')
				record_field_obj(c, recv, member, c->last_ret_type);
			emit_push(c, 0);
			emit_u8(c, OP_SWAP);
			emit_u8(c, OP_SET_OBJ);
			emit_u8(c, OP_POP);
			return;
		}

		emit_push(c, 0);
		emit_u8(c, OP_SWAP);
		emit_coerce(c, got, field_expr_kind(fk));
		emit_u8(c, (uint8_t)mem_set_op(fk));
		emit_u8(c, OP_POP);
	}
}

static void compile_store_chain(Comp *c, const char *recv, char fields[][STGE_NAME_MAX], int n)
{
	int i;

	if (n <= 0)
		return;

	if (n == 1)
	{
		compile_member_assign(c, recv, fields[0]);
		return;
	}

	if (n == 2)
	{
		const PackedType *pf = find_packed(c, lookup_field_type(c, recv, fields[0]));
		int idx = packed_is_composite(pf) ? packed_alias_index(pf, fields[1]) : -1;

		if (idx >= 0)
		{
			ExprKind got;

			emit_recv(c, recv);
			emit_push_str(c, fields[0]);
			emit_push(c, idx);
			expect(c, TOK_EQUAL, "expected '='");
			got = compile_expr(c);
			emit_coerce(c, got, EK_INT);
			emit_u8(c, OP_SET_MEM);
			emit_u8(c, OP_POP);
			return;
		}
	}

	emit_recv(c, recv);
	for (i = 0; i < n - 1; i++)
		emit_get_field(c, fields[i]);

	if (at(c, TOK_LBRACK))
	{
		emit_push_str(c, fields[n - 1]);
		compile_index_or_zero(c);
		expect(c, TOK_EQUAL, "expected '='");
		compile_expr(c);
		emit_u8(c, OP_SET_MEM);
		emit_u8(c, OP_POP);
		return;
	}

	expect(c, TOK_EQUAL, "expected '='");
	if (c->failed)
		return;

	if (strcmp(fields[n - 1], "display") == 0)
	{
		compile_atom(c);
		emit_u8(c, OP_SET_DISPLAY);
		emit_u8(c, OP_POP);
		return;
	}

	emit_push_str(c, fields[n - 1]);
	emit_push(c, 0);
	compile_expr(c);
	emit_u8(c, OP_SET_MEM);
	emit_u8(c, OP_POP);
}

static bool at_compound(const Comp *c)
{
	return at(c, TOK_PLUSEQ) || at(c, TOK_MINUSEQ)
		|| at(c, TOK_STAREQ) || at(c, TOK_SLASHEQ);
}

static const char *compound_name(const Comp *c)
{
	if (at(c, TOK_PLUSEQ))
		return "+=";
	if (at(c, TOK_MINUSEQ))
		return "-=";
	if (at(c, TOK_STAREQ))
		return "*=";
	if (at(c, TOK_SLASHEQ))
		return "/=";
	return "+=";
}

static const char *field_type_any(Comp *c, const char *recv, const char *field)
{
	const char *t;
	int i;

	t = lookup_field_type(c, recv, field);
	if (t != NULL && t[0] != '\0')
		return t;

	for (i = 0; i < c->node_count; i++)
	{
		t = lookup_field_type(c, c->nodes[i].name, field);
		if (t != NULL && t[0] != '\0')
			return t;
	}

	return "";
}

static FieldKind field_kind_any(Comp *c, const char *recv, const char *field)
{
	FieldKind fk = lookup_field_kind(c, recv, field);
	int i;

	if (fk != FK_NONE)
		return fk;

	for (i = 0; i < c->node_count; i++)
	{
		fk = lookup_field_kind(c, c->nodes[i].name, field);
		if (fk != FK_NONE)
			return fk;
	}

	return FK_NONE;
}

static void emit_type_op_call(Comp *c, const char *type, const char *op)
{
	const NsRef *ns = find_ns(c, type);
	const ObjFn *sf = ns_find_fn(ns, op);
	int obj_id;

	if (sf == NULL)
	{
		failf(c, "unknown operator '%s' on '%s'", op, type);
		return;
	}

	obj_id = ns != NULL ? ns->obj_id : -1;
	if (obj_id == -1)
	{
		const NodeRef *node = find_node(c, type);

		obj_id = node != NULL ? node->id : 0;
	}

	if (obj_id != -1)
		emit_push_node(c, obj_id);
	else
		emit_push(c, 0);
	{
		VmProg *s = stge_prog();

		emit_push(c, sf->pc);
		if (sf->pc < 0 && s != NULL && c->call_patch_count < STGE_MAX_CALL_PATCH)
		{
			c->call_patch_at[c->call_patch_count] = s->code_len - 4;
			c->call_patch_obj[c->call_patch_count] = obj_id;
			strcpy(c->call_patch_fn[c->call_patch_count], op);
			c->call_patch_count++;
		}
	}
	emit_call(c, OP_CALL_NS, 2);
	if (find_packed(c, type) != NULL)
	{
		c->expr_kind = EK_OBJ;
		snprintf(c->last_ret_type, sizeof c->last_ret_type, "%s", type);
	}
	else
		c->expr_kind = EK_INT;
}

static bool emit_packed_new0(Comp *c, const char *type)
{
	const NsRef *ns = find_ns(c, type);
	const NodeRef *node = find_node(c, type);
	const ObjFn *ctor = ns != NULL ? ns_find_fn_arity(ns, "new", 0) : NULL;
	int obj_id = node != NULL ? node->id : (ns != NULL ? ns->obj_id : -1);

	if (ctor == NULL || obj_id == -1)
		return false;

	emit_push_node(c, obj_id);
	{
		VmProg *s = stge_prog();

		emit_push(c, ctor->pc);
		if (ctor->pc < 0 && s != NULL && c->call_patch_count < STGE_MAX_CALL_PATCH)
		{
			c->call_patch_at[c->call_patch_count] = s->code_len - 4;
			c->call_patch_obj[c->call_patch_count] = obj_id;
			strcpy(c->call_patch_fn[c->call_patch_count], "new");
			c->call_patch_count++;
		}
	}
	emit_call(c, OP_CALL_NEW, 0);
	c->expr_kind = EK_OBJ;
	snprintf(c->last_ret_type, sizeof c->last_ret_type, "%s", type);
	return true;
}

static const PackedType *packed_of_name(Comp *c, const char *recv)
{
	const PackedType *p = find_packed(c, recv);
	int slot;

	if (packed_is_composite(p))
		return p;
	if (c->has_param && recv != NULL && strcmp(recv, c->param) == 0)
		return find_packed(c, c->param_type);
	slot = find_slot_local(c, recv);
	if (slot >= 0)
		return find_packed(c, c->slot_types[slot]);
	return NULL;
}

static void compile_compound_store(Comp *c, const char *recv,
				  char fields[][STGE_NAME_MAX], int n)
{
	const char *op = compound_name(c);
	FieldKind fk;
	const PackedType *pr;
	const PackedType *pf;
	int alias_idx;

	advance(c);
	pr = packed_of_name(c, recv);
	alias_idx = (n == 1) ? packed_alias_index(pr, fields[0]) : -1;
	if (alias_idx >= 0)
	{
		Op arith = strcmp(op, "-=") == 0 ? OP_SUB
			: strcmp(op, "*=") == 0 ? OP_MUL
			: strcmp(op, "/=") == 0 ? OP_DIV : OP_ADD;

		emit_recv(c, recv);
		emit_push_str(c, pr->name);
		emit_push(c, alias_idx);
		emit_u8(c, OP_GET_MEM);
		compile_expr(c);
		emit_u8(c, (uint8_t)arith);
		emit_recv(c, recv);
		emit_u8(c, OP_SWAP);
		emit_push_str(c, pr->name);
		emit_u8(c, OP_SWAP);
		emit_push(c, alias_idx);
		emit_u8(c, OP_SWAP);
		emit_u8(c, OP_SET_MEM);
		emit_u8(c, OP_POP);
		return;
	}

	pf = (n >= 1) ? find_packed(c, lookup_field_type(c, recv, fields[0])) : NULL;
	if (n == 1 && packed_is_composite(pf))
	{
		if (!emit_packed_new0(c, pf->name))
		{
			failf(c, "unknown function '%s::new'", pf->name);
			return;
		}
		emit_push_str(c, pf->name);
		emit_recv(c, recv);
		emit_push_str(c, fields[0]);
		emit_u8(c, OP_MEM_COPY);
		compile_expr(c);
		emit_type_op_call(c, pf->name, op);
		emit_recv(c, recv);
		emit_u8(c, OP_SWAP);
		emit_push_str(c, fields[0]);
		emit_u8(c, OP_SWAP);
		emit_push_str(c, pf->name);
		emit_u8(c, OP_MEM_COPY);
		emit_u8(c, OP_POP);
		return;
	}

	if (n == 2 && packed_is_composite(pf))
	{
		int idx = packed_alias_index(pf, fields[1]);
		Op arith = strcmp(op, "-=") == 0 ? OP_SUB
			: strcmp(op, "*=") == 0 ? OP_MUL
			: strcmp(op, "/=") == 0 ? OP_DIV : OP_ADD;

		if (idx < 0)
		{
			failf(c, "unknown field '%s'", fields[1]);
			return;
		}

		emit_recv(c, recv);
		emit_push_str(c, fields[0]);
		emit_push(c, idx);
		emit_u8(c, OP_GET_MEM);
		compile_expr(c);
		emit_u8(c, (uint8_t)arith);
		emit_recv(c, recv);
		emit_u8(c, OP_SWAP);
		emit_push_str(c, fields[0]);
		emit_u8(c, OP_SWAP);
		emit_push(c, idx);
		emit_u8(c, OP_SWAP);
		emit_u8(c, OP_SET_MEM);
		emit_u8(c, OP_POP);
		return;
	}

	fk = field_kind_any(c, recv, fields[n - 1]);
	if (fk == FK_INT || fk == FK_UINT || fk == FK_BYTE)
	{
		Op arith = strcmp(op, "-=") == 0 ? OP_SUB
			: strcmp(op, "*=") == 0 ? OP_MUL
			: strcmp(op, "/=") == 0 ? OP_DIV : OP_ADD;
		int i;

		emit_recv(c, recv);
		for (i = 0; i < n - 1; i++)
			emit_get_field(c, fields[i]);
		emit_push_str(c, fields[n - 1]);
		emit_push(c, 0);
		emit_u8(c, OP_GET_MEM);
		compile_expr(c);
		emit_u8(c, (uint8_t)arith);
		emit_recv(c, recv);
		for (i = 0; i < n - 1; i++)
			emit_get_field(c, fields[i]);
		emit_u8(c, OP_SWAP);
		emit_push_str(c, fields[n - 1]);
		emit_u8(c, OP_SWAP);
		emit_push(c, 0);
		emit_u8(c, OP_SWAP);
		emit_u8(c, OP_SET_MEM);
		emit_u8(c, OP_POP);
		return;
	}

	if (fk == FK_OBJ)
	{
		const char *otype = field_type_any(c, recv, fields[n - 1]);
		int i;

		if (otype[0] == '\0')
			otype = recv;

		emit_recv(c, recv);
		for (i = 0; i < n - 1; i++)
			emit_get_field(c, fields[i]);
		emit_push_str(c, fields[n - 1]);
		emit_push(c, 0);
		emit_u8(c, OP_GET_MEM);
		compile_expr(c);
		emit_type_op_call(c, otype, op);
		emit_recv(c, recv);
		for (i = 0; i < n - 1; i++)
			emit_get_field(c, fields[i]);
		emit_u8(c, OP_SWAP);
		emit_push_str(c, fields[n - 1]);
		emit_u8(c, OP_SWAP);
		emit_push(c, 0);
		emit_u8(c, OP_SWAP);
		emit_u8(c, OP_SET_MEM);
		emit_u8(c, OP_POP);
		return;
	}

	failf(c, "unknown operator '%s'", op);
}

static bool compile_assign(Comp *c)
{
	char name[STGE_NAME_MAX];
	char fields[STGE_MAX_PATH][STGE_NAME_MAX];
	int n = 0;
	Lex saved_lex;
	Tok saved_tok;

	if (!at(c, TOK_IDENT))
		return false;

	saved_lex = c->lex;
	saved_tok = c->tok;

	tok_copy(name, sizeof name, &c->tok);
	advance(c);

	if (!at(c, TOK_DOT))
	{
		lex_keep_buf(&c->lex, &saved_lex);
		c->tok = saved_tok;
		return false;
	}

	while (at(c, TOK_DOT) && n < STGE_MAX_PATH)
	{
		advance(c);
		if (!at(c, TOK_IDENT))
		{
			lex_keep_buf(&c->lex, &saved_lex);
			c->tok = saved_tok;
			return false;
		}

		tok_copy(fields[n], sizeof fields[0], &c->tok);
		n++;
		advance(c);
	}

	if (n == 0 || (!at(c, TOK_EQUAL) && !at(c, TOK_LBRACK) && !at_compound(c)))
	{
		lex_keep_buf(&c->lex, &saved_lex);
		c->tok = saved_tok;
		return false;
	}

	if (at_compound(c))
	{
		compile_compound_store(c, name, fields, n);
		return true;
	}

	if (at(c, TOK_EQUAL))
	{
		Lex body_lex = c->lex;
		Tok body_tok = c->tok;

		advance(c);
		if (n == 1 && at(c, TOK_LPAREN))
		{
			NodeRef *node = find_node(c, name);
			int saved_self = c->self;
			int pc;
			bool has_arg = false;

			if (node == NULL)
			{
				failf(c, "unknown node '%s'", name);
				return true;
			}

			lex_keep_buf(&c->lex, &body_lex);
			c->tok = body_tok;
			advance(c);
			c->self = node->id;
			pc = compile_body(c, &has_arg);
			c->self = saved_self;
			if (c->failed)
				return true;

			store_fn(c, node, fields[0], pc, has_arg);
			return true;
		}

		lex_keep_buf(&c->lex, &body_lex);
		c->tok = body_tok;
	}

	compile_store_chain(c, name, fields, n);
	return true;
}

static bool take_else(Comp *c)
{
	Lex saved_lex = c->lex;
	Tok saved_tok = c->tok;

	skip_eol(c);
	if (at(c, TOK_IDENT) && tok_eq(&c->tok, "else"))
	{
		advance(c);
		return true;
	}

	lex_keep_buf(&c->lex, &saved_lex);
	c->tok = saved_tok;
	return false;
}

static void compile_slot_stmt(Comp *c)
{
	int slot;
	FieldKind kind;
	const PackedType *p;
	char name[STGE_NAME_MAX];

	slot = find_slot_local(c, tok_cstr(c, &c->tok));
	if (slot < 0)
	{
		failf(c, "unknown name '%.*s'", (int)c->tok.len, c->tok.text);
		return;
	}

	kind = c->slot_kinds[slot];
	p = find_packed(c, c->slot_types[slot]);
	tok_copy(name, sizeof name, &c->tok);
	advance(c);
	if (at(c, TOK_DOT))
	{
		char field[STGE_NAME_MAX];
		int idx;

		advance(c);
		if (!at(c, TOK_IDENT))
		{
			fail(c, "expected a field");
			return;
		}

		tok_copy(field, sizeof field, &c->tok);
		advance(c);
		if (is_str_bytes(field) && at(c, TOK_DOT))
		{
			advance(c);
			if (!at(c, TOK_IDENT) || (!tok_eq(&c->tok, "append")
			    && !tok_eq(&c->tok, "add")))
			{
				fail(c, "expected append");
				return;
			}

			advance(c);
			expect(c, TOK_LPAREN, "expected '('");
			emit_get_local(c, slot);
			emit_push_str(c, "buffer");
			compile_expr(c);
			expect(c, TOK_RPAREN, "expected ')'");
			emit_u8(c, OP_DYN_ADD);
			emit_u8(c, OP_POP);
			return;
		}

		if (packed_is_composite(p))
		{
			idx = packed_alias_index(p, field);
			if (idx < 0)
			{
				failf(c, "unknown field '%s' on '%s'", field, name);
				return;
			}

			if (at(c, TOK_PLUSEQ) || at(c, TOK_MINUSEQ)
			    || at(c, TOK_STAREQ) || at(c, TOK_SLASHEQ))
			{
				Op arith = at(c, TOK_PLUSEQ) ? OP_ADD
					: at(c, TOK_MINUSEQ) ? OP_SUB
					: at(c, TOK_STAREQ) ? OP_MUL : OP_DIV;

				advance(c);
				emit_get_local(c, slot);
				emit_push_str(c, p->name);
				emit_push(c, idx);
				emit_u8(c, OP_GET_MEM);
				compile_expr(c);
				emit_u8(c, (uint8_t)arith);
				emit_get_local(c, slot);
				emit_u8(c, OP_SWAP);
				emit_push_str(c, p->name);
				emit_u8(c, OP_SWAP);
				emit_push(c, idx);
				emit_u8(c, OP_SWAP);
				emit_u8(c, OP_SET_MEM);
				emit_u8(c, OP_POP);
				return;
			}

			expect(c, TOK_EQUAL, "expected '='");
			emit_get_local(c, slot);
			emit_push_str(c, p->name);
			emit_push(c, idx);
			compile_expr(c);
			emit_u8(c, OP_SET_MEM);
			emit_u8(c, OP_POP);
			return;
		}

		if (at(c, TOK_PLUSEQ) || at(c, TOK_MINUSEQ)
		    || at(c, TOK_STAREQ) || at(c, TOK_SLASHEQ))
		{
			Op arith = at(c, TOK_PLUSEQ) ? OP_ADD
				: at(c, TOK_MINUSEQ) ? OP_SUB
				: at(c, TOK_STAREQ) ? OP_MUL : OP_DIV;

			advance(c);
			emit_get_local(c, slot);
			emit_push_str(c, field);
			emit_push(c, 0);
			emit_u8(c, OP_GET_MEM);
			compile_expr(c);
			emit_u8(c, (uint8_t)arith);
			emit_get_local(c, slot);
			emit_u8(c, OP_SWAP);
			emit_push_str(c, field);
			emit_u8(c, OP_SWAP);
			emit_push(c, 0);
			emit_u8(c, OP_SWAP);
			emit_u8(c, OP_SET_MEM);
			emit_u8(c, OP_POP);
			return;
		}

		expect(c, TOK_EQUAL, "expected '='");
		emit_get_local(c, slot);
		emit_push_str(c, field);
		emit_push(c, 0);
		compile_expr(c);
		emit_u8(c, OP_SET_MEM);
		emit_u8(c, OP_POP);
		return;
	}

	if (at(c, TOK_PLUSEQ) || at(c, TOK_MINUSEQ)
	    || at(c, TOK_STAREQ) || at(c, TOK_SLASHEQ))
	{
		const char *opn = compound_name(c);
		Op arith = at(c, TOK_PLUSEQ) ? OP_ADD
			: at(c, TOK_MINUSEQ) ? OP_SUB
			: at(c, TOK_STAREQ) ? OP_MUL : OP_DIV;

		advance(c);
		if (packed_is_composite(p) || kind == FK_OBJ)
		{
			const char *otype = c->slot_types[slot];

			if (otype[0] == '\0')
				otype = packed_is_composite(p) ? p->name : "object";
			emit_get_local(c, slot);
			compile_expr(c);
			emit_type_op_call(c, otype, opn);
			emit_set_local(c, slot);
			emit_u8(c, OP_POP);
			return;
		}

		emit_get_local(c, slot);
		compile_expr(c);
		emit_u8(c, (uint8_t)arith);
		emit_set_local(c, slot);
		emit_u8(c, OP_POP);
		return;
	}

	expect(c, TOK_EQUAL, "expected '='");
	if (at(c, TOK_STRING)
	    && c->slot_kinds[slot] == FK_OBJ
	    && strcmp(c->slot_types[slot], "String") == 0)
	{
		emit_get_local(c, slot);
		compile_string_into(c);
		emit_set_local(c, slot);
		emit_u8(c, OP_POP);
		return;
	}

	compile_expr(c);
	if (c->expr_kind == EK_OBJ)
	{
		c->slot_kinds[slot] = FK_OBJ;
		set_slot_type(c, slot, c->last_ret_type);
	}
	else if (c->expr_kind == EK_INT)
	{
		c->slot_kinds[slot] = FK_INT;
		if (c->last_ret_type[0] != '\0')
			set_slot_type(c, slot, c->last_ret_type);
		else
			set_slot_type(c, slot, "int");
	}
	emit_set_local(c, slot);
	emit_u8(c, OP_POP);
}

static int compile_body_statement(Comp *c)
{
	if (c->failed)
		return 0;

	if (at(c, TOK_LBRACE))
	{
		compile_block(c);
		return 0;
	}

	if (at(c, TOK_IDENT) && look(c).kind == TOK_IDENT)
	{
		Lex peek_lex = lex_probe(&c->lex);
		Tok after_name;

		(void)lex_tok(&peek_lex);
		after_name = lex_tok(&peek_lex);
		lex_probe_free(&peek_lex);
		if (after_name.kind == TOK_EQUAL)
		{
			char type[STGE_NAME_MAX];
			char lname[STGE_NAME_MAX];
			int slot;
			FieldKind kind;

			tok_copy(type, sizeof type, &c->tok);
			advance(c);
			tok_copy(lname, sizeof lname, &c->tok);
			advance(c);
			expect(c, TOK_EQUAL, "expected '='");
			compile_expr(c);
			if (strcmp(type, "object") != 0 && c->last_ret_type[0] != '\0'
			    && strcmp(type, c->last_ret_type) != 0)
			{
				failf(c, "type mismatch: '%s' is not '%s'", c->last_ret_type, type);
				return 0;
			}

			kind = c->expr_kind == EK_OBJ ? FK_OBJ : FK_INT;
			slot = add_slot_local(c, lname, kind);
			if (slot < 0)
			{
				fail(c, "too many locals");
				return 0;
			}

			if (kind == FK_OBJ)
				set_slot_type(c, slot, type[0] != '\0' ? type : c->last_ret_type);
			emit_set_local(c, slot);
			emit_u8(c, OP_POP);
			return 0;
		}
	}

	if (at(c, TOK_IDENT) && look(c).kind == TOK_EQUAL
	    && find_slot_local(c, tok_cstr(c, &c->tok)) < 0)
	{
		char lname[STGE_NAME_MAX];
		int slot;
		FieldKind kind;

		tok_copy(lname, sizeof lname, &c->tok);
		advance(c);
		expect(c, TOK_EQUAL, "expected '='");
		compile_expr(c);
		kind = c->expr_kind == EK_OBJ ? FK_OBJ : FK_INT;
		slot = add_slot_local(c, lname, kind);
		if (slot < 0)
		{
			fail(c, "too many locals");
			return 0;
		}

		if (kind == FK_OBJ)
			set_slot_type(c, slot, c->last_ret_type);
		else if (c->expr_kind == EK_INT)
			set_slot_type(c, slot,
				      c->last_ret_type[0] != '\0' ? c->last_ret_type : "int");
		emit_set_local(c, slot);
		emit_u8(c, OP_POP);
		return 0;
	}

	if (at(c, TOK_IDENT) && find_slot_local(c, tok_cstr(c, &c->tok)) >= 0
	    && (look(c).kind == TOK_EQUAL || look(c).kind == TOK_PLUSEQ
		|| look(c).kind == TOK_MINUSEQ || look(c).kind == TOK_STAREQ
		|| look(c).kind == TOK_SLASHEQ || look(c).kind == TOK_DOT))
	{
		compile_slot_stmt(c);
		return 0;
	}

	if (at(c, TOK_IDENT) && tok_eq(&c->tok, "return"))
	{
		advance(c);
		if (at(c, TOK_EOL) || at(c, TOK_RBRACE) || at(c, TOK_EOF))
			emit_push(c, 0);
		else
			compile_expr(c);

		emit_u8(c, OP_HALT);
		return 0;
	}

	if (at(c, TOK_IDENT) && look(c).kind == TOK_LBRACK)
	{
		FieldKind fk;
		Op op;
		MemberKind mk;
		char lname[STGE_NAME_MAX];

		if (!type_name_kind(tok_cstr(c, &c->tok), &fk, &op, &mk))
		{
			fail(c, "expected a type");
			return 0;
		}

		advance(c);
		expect(c, TOK_LBRACK, "expected '['");
		if (at(c, TOK_STAR))
		{
			fail(c, "use T[] not T[*]");
			return 0;
		}
		expect(c, TOK_RBRACK, "expected ']'");
		if (!at(c, TOK_IDENT))
		{
			fail(c, "expected a name");
			return 0;
		}

		tok_copy(lname, sizeof lname, &c->tok);
		advance(c);
		if (!add_local_kind(c, lname, true))
		{
			fail(c, "too many locals");
			return 0;
		}

		expect(c, TOK_EQUAL, "expected '='");
		if (c->self < 0)
		{
			fail(c, "self outside a body");
			return 0;
		}

		emit_u8(c, OP_PUSH_SELF);
		emit_push_str(c, lname);
		emit_u8(c, OP_NEW_DYN);
		emit_u8(c, (uint8_t)mk);
		emit_u8(c, OP_POP);
		if (at(c, TOK_IDENT) && look(c).kind == TOK_DOT)
		{
			char src[STGE_NAME_MAX];
			char srcf[STGE_NAME_MAX];

			tok_copy(src, sizeof src, &c->tok);
			advance(c);
			advance(c);
			if (!at(c, TOK_IDENT))
			{
				fail(c, "expected a field");
				return 0;
			}

			tok_copy(srcf, sizeof srcf, &c->tok);
			advance(c);
			emit_u8(c, OP_PUSH_SELF);
			emit_push_str(c, lname);
			emit_recv(c, src);
			emit_push_str(c, srcf);
			emit_u8(c, OP_DYN_COPY);
			emit_u8(c, OP_POP);
			return 0;
		}

		compile_expr(c);
		emit_u8(c, OP_POP);
		return 0;
	}

	if (at(c, TOK_IDENT) && tok_eq(&c->tok, "if"))
	{
		size_t jz;

		advance(c);
		compile_condition(c);
		skip_eol(c);
		jz = emit_jump(c, OP_JZ);
		if (compile_body_statement(c))
			emit_u8(c, OP_POP);

		if (take_else(c))
		{
			size_t over = emit_jump(c, OP_JMP);

			patch_jump(c, jz);
			skip_eol(c);
			if (compile_body_statement(c))
				emit_u8(c, OP_POP);
			patch_jump(c, over);
		}
		else
		{
			patch_jump(c, jz);
		}

		return 0;
	}

	if (at(c, TOK_IDENT) && tok_eq(&c->tok, "while"))
	{
		compile_while(c);
		return 0;
	}

	if (compile_assign(c))
		return 0;

	if (at(c, TOK_INT) || at(c, TOK_STRING) || at(c, TOK_FLOAT)
	    || at(c, TOK_MINUS))
	{
		compile_expr(c);
		return 1;
	}

	if (at(c, TOK_IDENT))
	{
		Tok nxt = look(c);

		if (nxt.kind != TOK_DOT && nxt.kind != TOK_LPAREN
		    && nxt.kind != TOK_COLONCOLON && nxt.kind != TOK_LBRACK)
		{
			compile_expr(c);
			return 1;
		}
	}

	compile_call(c);
	return 1;
}

static int compile_body(Comp *c, bool *has_arg)
{
	size_t over;
	int entry;
	char saved_param[STGE_NAME_MAX];
	bool saved_has = c->has_param;
	int saved_self = c->self;
	char saved_param_type[STGE_NAME_MAX];
	int saved_locals = c->local_count;
	int saved_slots = c->slot_count;
	int saved_argc = c->fn_argc;
	int bind_slots = 0;
	int *param_slots = NULL;
	int param_slots_cap = 0;

	memcpy(saved_param, c->param, sizeof saved_param);
	memcpy(saved_param_type, c->param_type, sizeof saved_param_type);
	c->has_param = false;
	c->param[0] = '\0';
	c->param_type[0] = '\0';
	c->local_count = 0;
	/* Keep outer/file-scope slots visible so nested writes use the same
	   flat vm.locals indices (e.g. test_quit). Params/locals append after. */
	c->fn_argc = 0;
	c->fn_self_param = -1;
	c->fn_ret_type[0] = '\0';

	expect(c, TOK_LPAREN, "expected '('");
	if (at(c, TOK_IDENT))
	{
		const char *owner_name = NULL;
		int oi;

		for (oi = 0; oi < c->node_count; oi++)
		{
			if (c->nodes[oi].id == c->self)
			{
				owner_name = c->nodes[oi].name;
				break;
			}
		}

		while (!c->failed)
		{
			char type[STGE_NAME_MAX * 2];
			char pname[STGE_NAME_MAX];
			FieldKind kind;
			int slot;
			bool bare_type_name;

			type[0] = '\0';
			if (!take_qname(c, type, sizeof type))
			{
				fail(c, "expected a name");
				goto body_fail;
			}

			if (at(c, TOK_IDENT))
			{
				if (c->tok.len >= sizeof pname)
				{
					fail(c, "name too long");
					goto body_fail;
				}

				tok_copy(pname, sizeof pname, &c->tok);
				advance(c);
				bare_type_name = false;
			}
			else
			{
				if (strlen(type) >= sizeof pname)
				{
					fail(c, "name too long");
					goto body_fail;
				}

				memcpy(pname, type, sizeof pname);
				type[0] = '\0';
				bare_type_name = true;
			}

			kind = FK_INT;
			if (type[0] != '\0')
			{
				FieldKind tfk;
				Op top;
				MemberKind tmk;

				if (type_name_kind(type, &tfk, &top, &tmk))
					kind = tfk;
				else
					kind = FK_OBJ;
			}
			else if (bare_type_name && owner_name != NULL
				 && strcmp(pname, owner_name) == 0)
			{
				FieldKind tfk;
				Op top;
				MemberKind tmk;

				if (type_name_kind(owner_name, &tfk, &top, &tmk))
					kind = tfk;
				else
					kind = FK_OBJ;
			}

			slot = add_slot_local(c, pname, kind);
			if (slot < 0)
			{
				fail(c, "too many locals");
				goto body_fail;
			}

			if (c->fn_argc >= param_slots_cap)
			{
				int ncap = param_slots_cap == 0 ? 8 : param_slots_cap * 2;
				int *nslots = realloc(param_slots, (size_t)ncap * sizeof *nslots);

				if (nslots == NULL)
				{
					fail(c, "out of memory");
					goto body_fail;
				}

				param_slots = nslots;
				param_slots_cap = ncap;
			}

			param_slots[c->fn_argc] = slot;
			if (type[0] != '\0')
				set_slot_type(c, slot, type);
			else if (bare_type_name && owner_name != NULL
				 && strcmp(pname, owner_name) == 0)
				set_slot_type(c, slot, owner_name);

			if (c->fn_argc == 0 && bare_type_name && owner_name != NULL
			    && strcmp(pname, owner_name) == 0)
				c->fn_self_param = 0;

			if (c->fn_argc == 0)
			{
				snprintf(c->param, sizeof c->param, "%s", pname);
				snprintf(c->param_type, sizeof c->param_type, "%s",
					 type[0] != '\0' ? type
					 : (c->fn_self_param == 0 && owner_name != NULL
					    ? owner_name : ""));
				c->has_param = c->param_type[0] != '\0';
			}

			c->fn_argc++;
			if (at(c, TOK_COMMA))
			{
				advance(c);
				if (!at(c, TOK_IDENT))
				{
					fail(c, "expected a name");
					goto body_fail;
				}

				continue;
			}

			break;
		}

		bind_slots = c->fn_argc;
		c->has_param = false;
	}

	expect(c, TOK_RPAREN, "expected ')'");
	if (at(c, TOK_ARROW))
	{
		advance(c);
		if (!at(c, TOK_IDENT))
		{
			fail(c, "expected a return type");
			goto body_fail;
		}

		if (c->tok.len >= sizeof c->fn_ret_type)
		{
			fail(c, "return type name too long");
			goto body_fail;
		}

		tok_copy(c->fn_ret_type, sizeof c->fn_ret_type, &c->tok);
		advance(c);
		if (at(c, TOK_LBRACK))
			take_brackets(c, false, NULL);
	}

	skip_eol(c);
	expect(c, TOK_LBRACE, "expected '{'");

	over = emit_jump(c, OP_JMP);
	entry = (int)(stge_prog() != NULL ? stge_prog()->code_len : 0);
	if (bind_slots > 0)
	{
		int si;

		for (si = 0; si < bind_slots; si++)
		{
			emit_push_arg(c, si);
			emit_set_local(c, param_slots[si]);
			emit_u8(c, OP_POP);
		}
	}

	{
		int pending = 0;

		for (;;)
		{
			skip_eol(c);

			if (c->failed)
				goto body_fail;

			if (at(c, TOK_RBRACE))
				break;

			if (at(c, TOK_EOF))
			{
				fail(c, "unterminated body");
				goto body_fail;
			}

			if (pending)
			{
				emit_u8(c, OP_POP);
				pending = 0;
			}

			pending = compile_body_statement(c);

			if (c->failed)
				goto body_fail;

			if (!at(c, TOK_RBRACE))
				expect(c, TOK_EOL, "expected end of line");
		}
	}

	advance(c);
	emit_u8(c, OP_HALT);
	patch_jump(c, over);

	if (has_arg != NULL)
		*has_arg = c->fn_argc > 0;

	{
		int result_argc = c->fn_argc;
		int i;

		/* Drop function-only names from the table but keep slot indices
		   reserved so later slots do not collide on shared vm locals. */
		for (i = saved_slots; i < c->slot_count; i++)
			c->slot_names[i][0] = '\0';

		c->has_param = saved_has;
		memcpy(c->param, saved_param, sizeof c->param);
		memcpy(c->param_type, saved_param_type, sizeof c->param_type);
		c->local_count = saved_locals;
		c->fn_argc = result_argc;
		c->self = saved_self;
	}
	free(param_slots);
	return c->failed ? -1 : entry;

body_fail:
	free(param_slots);
	{
		int i;

		for (i = saved_slots; i < c->slot_count; i++)
			c->slot_names[i][0] = '\0';
	}
	c->has_param = saved_has;
	memcpy(c->param, saved_param, sizeof c->param);
	memcpy(c->param_type, saved_param_type, sizeof c->param_type);
	c->local_count = saved_locals;
	c->fn_argc = saved_argc;
	c->self = saved_self;
	return -1;
}

static void store_fn(Comp *c, NodeRef *node, const char *field, int pc, bool has_arg)
{
	int i;
	int argc = c != NULL ? c->fn_argc : (has_arg ? 1 : 0);
	int self_param = c != NULL ? c->fn_self_param : -1;
	const char *ret_type = c != NULL ? c->fn_ret_type : NULL;

	for (i = 0; i < node->fn_count; i++)
	{
		if (strcmp(node->fns[i].name, field) == 0
		    && node->fns[i].argc == argc
		    && !node->fns[i].is_extern)
		{
			node->fns[i].pc = pc;
			node->fns[i].has_arg = has_arg;
			node->fns[i].argc = argc;
			node->fns[i].self_param = self_param;
			node->fns[i].is_extern = false;
			if (ret_type != NULL && ret_type[0] != '\0')
				snprintf(node->fns[i].ret_type, sizeof node->fns[i].ret_type,
					 "%s", ret_type);
			else
				node->fns[i].ret_type[0] = '\0';
			patch_fn_calls(c, node->id, field, pc);
			return;
		}
	}

	if (!ensure_fns_cap(&node->fns, &node->fn_count, &node->fns_cap))
	{
		fail(c, "out of memory");
		return;
	}

	memset(&node->fns[node->fn_count], 0, sizeof node->fns[0]);
	memcpy(node->fns[node->fn_count].name, field, sizeof node->fns[0].name);
	node->fns[node->fn_count].pc = pc;
	node->fns[node->fn_count].has_arg = has_arg;
	node->fns[node->fn_count].argc = argc;
	node->fns[node->fn_count].self_param = self_param;
	if (ret_type != NULL && ret_type[0] != '\0')
		snprintf(node->fns[node->fn_count].ret_type,
			 sizeof node->fns[node->fn_count].ret_type, "%s", ret_type);
	node->fn_count++;
	patch_fn_calls(c, node->id, field, pc);
}

static void compile_enum(Comp *c, const char *qname)
{
	NsRef *ns;
	NodeRef *node;
	int32_t next_val = 0;

	advance(c);
	expect(c, TOK_LBRACE, "expected '{'");
	if (qname == NULL || qname[0] == '\0')
	{
		fail(c, "expected a name");
		return;
	}

	node = bind_type_node(c, qname);
	ns = ensure_ns(c, qname);
	if (node == NULL || ns == NULL)
		return;

	ns->obj_id = node->id;

	for (;;)
	{
		skip_eol(c);
		if (c->failed || at(c, TOK_RBRACE) || at(c, TOK_EOF))
			break;

		if (!at(c, TOK_IDENT))
		{
			fail(c, "expected an enumerator");
			return;
		}

		{
			char member[STGE_NAME_MAX];
			int i;

			tok_copy(member, sizeof member, &c->tok);
			advance(c);
			if (at(c, TOK_EQUAL))
			{
				int neg = 0;

				advance(c);
				if (at(c, TOK_MINUS))
				{
					neg = 1;
					advance(c);
				}

				if (!at(c, TOK_INT))
				{
					fail(c, "expected an integer");
					return;
				}

				next_val = neg ? -c->tok.value : c->tok.value;
				advance(c);
			}

			if (!ns_add_const(ns, member, next_val))
			{
				fail(c, "too many constants");
				return;
			}

			emit_push_node(c, node->id);
			emit_push_str(c, member);
			emit_push(c, 1);
			emit_u8(c, OP_NEW_INT);
			emit_u8(c, OP_POP);
			emit_push_node(c, node->id);
			emit_push_str(c, member);
			emit_push(c, 0);
			emit_push(c, next_val);
			emit_u8(c, OP_SET_MEM);
			emit_u8(c, OP_POP);
			record_field_kind(c, qname, member, FK_INT);
			for (i = 0; i < node->field_count; i++)
			{
				if (strcmp(node->fields[i].name, member) == 0)
				{
					snprintf(node->fields[i].type, sizeof node->fields[i].type,
						 "%s", qname);
					break;
				}
			}

			next_val++;
		}

		if (at(c, TOK_COMMA))
			advance(c);
	}

	expect(c, TOK_RBRACE, "expected '}'");
}

static void compile_extern_struct(Comp *c, const char *name)
{
	char cname[STGE_NAME_MAX];
	NodeRef *node;

	if (c->path == NULL || strstr(c->path, "src/lang/") == NULL)
	{
		fail(c, "extern is only allowed in lang files");
		return;
	}

	advance(c);
	if (!at(c, TOK_IDENT))
	{
		fail(c, "expected a C symbol");
		return;
	}

	tok_copy(cname, sizeof cname, &c->tok);
	advance(c);
	if (stge_host_type_find(cname) < 0)
	{
		failf(c, "unknown C symbol '%s'", cname);
		return;
	}

	expect(c, TOK_LBRACE, "expected '{'");
	node = bind_type_node(c, name);
	if (node == NULL)
		return;

	for (;;)
	{
		char recv[STGE_NAME_MAX * 2];
		char field[STGE_NAME_MAX];
		FieldKind fk;
		Op op;
		MemberKind mk;

		skip_eol(c);
		if (c->failed || at(c, TOK_RBRACE) || at(c, TOK_EOF))
			break;

		if (!take_qname(c, recv, sizeof recv))
		{
			fail(c, "expected a field");
			return;
		}

		if (!name_eq(recv, name))
		{
			fail(c, "expected the type name");
			return;
		}

		expect(c, TOK_DOT, "expected '.'");
		if (!at(c, TOK_IDENT))
		{
			fail(c, "expected a field");
			return;
		}

		tok_copy(field, sizeof field, &c->tok);
		advance(c);
		expect(c, TOK_EQUAL, "expected '='");
		if (!at(c, TOK_IDENT) || !type_name_kind(tok_cstr(c, &c->tok), &fk, &op, &mk))
		{
			fail(c, "expected a type");
			return;
		}

		(void)op;

		advance(c);
		if (at(c, TOK_LPAREN))
		{
			advance(c);
			expect(c, TOK_RPAREN, "expected ')'");
			record_field_kind(c, name, field, fk);
		}
		else if (at(c, TOK_LBRACK))
		{
			int32_t arr_len = -1;

			take_brackets(c, false, &arr_len);
			record_field_array(c, name, field, fk, arr_len, arr_len < 0);
		}
		else
		{
			fail(c, "expected a type");
			return;
		}
	}

	expect(c, TOK_RBRACE, "expected '}'");
}

static void compile_binding(Comp *c, const char *name)
{
	if (at(c, TOK_IDENT) && tok_eq(&c->tok, "extern"))
	{
		compile_extern_struct(c, name);
		return;
	}

	if (word_is(c, "enum"))
	{
		compile_enum(c, name);
		return;
	}

	if (at(c, TOK_LBRACE))
	{
		advance(c);
		if (bind_type_node(c, name) == NULL)
			return;

		for (;;)
		{
			skip_eol(c);
			if (c->failed || at(c, TOK_RBRACE))
				break;

			if (at(c, TOK_EOF))
			{
				fail(c, "unterminated object");
				return;
			}

			compile_top_statement(c);
			if (c->failed)
				return;

			if (!at(c, TOK_RBRACE) && !at(c, TOK_EOF))
				expect(c, TOK_EOL, "expected end of line");
		}

		expect(c, TOK_RBRACE, "expected '}'");
		return;
	}

	if (at(c, TOK_LBRACK))
	{
		int32_t len = 1;
		PackedType *p;

		take_brackets(c, true, &len);
		if (bind_type_node(c, name) == NULL)
			return;
		p = record_packed_layout(c, name, "", len);
		(void)p;
		return;
	}

	if (at(c, TOK_IDENT) && look(c).kind == TOK_LBRACK)
	{
		char elem[STGE_NAME_MAX];
		int32_t len = 1;
		PackedType *p;
		NodeRef *node;

		tok_copy(elem, sizeof elem, &c->tok);
		advance(c);
		take_brackets(c, true, &len);
		node = bind_type_node(c, name);
		if (node == NULL)
			return;
		p = record_packed_layout(c, name, elem, len);
		{
			Lex saved_lex = c->lex;
			Tok saved_tok = c->tok;

			skip_eol(c);
			if (at(c, TOK_LBRACE))
				parse_packed_aliases(c, p);
			else
			{
				lex_keep_buf(&c->lex, &saved_lex);
				c->tok = saved_tok;
			}
		}
		emit_packed_template(c, node, p);
		return;
	}

	if (word_is(c, "sto") && look(c).kind == TOK_LPAREN)
	{
		fail(c, "sto() is gone; use sprite.new or {}");
		return;
	}

	if (find_node(c, name) != NULL)
	{
		failf(c, "name '%s' is already defined", name);
		return;
	}

	{
		ExprKind kind = compile_expr(c);
		int slot;
		FieldKind fk;

		if (c->failed)
			return;

		fk = kind == EK_OBJ ? FK_OBJ : FK_INT;
		slot = add_slot_local(c, name, fk);
		if (slot < 0)
		{
			fail(c, "too many locals");
			return;
		}

		if (fk == FK_OBJ)
			set_slot_type(c, slot, c->last_ret_type);
		else if (kind == EK_INT)
			set_slot_type(c, slot,
				      c->last_ret_type[0] != '\0' ? c->last_ret_type : "int");
		emit_set_local(c, slot);
		emit_u8(c, OP_POP);
	}
}

static void bind_extern_fn(Comp *c, const char *owner, const char *fn,
			   int native_id, bool has_arg)
{
	NsRef *ns;
	const ObjFn *existing;
	NodeRef *node;
	int i;

	ns = ensure_ns(c, owner);
	if (ns == NULL)
		return;

	existing = ns_find_fn(ns, fn);
	if (existing != NULL)
	{
		ObjFn *slot = (ObjFn *)existing;

		if (existing->is_extern && existing->native_id >= 0)
		{
			failf(c, "name '%s::%s' is already defined", owner, fn);
			return;
		}

		if (!existing->is_extern && existing->pc >= 0)
		{
			failf(c, "name '%s::%s' is already defined", owner, fn);
			return;
		}

		slot->pc = -2;
		slot->has_arg = has_arg;
		slot->is_extern = true;
		slot->native_id = native_id;
	}
	else if (!ns_add_extern(ns, fn, native_id, has_arg))
	{
		fail(c, "too many functions");
		return;
	}

	node = find_node(c, owner);
	if (node == NULL)
		return;

	for (i = 0; i < node->fn_count; i++)
	{
		if (strcmp(node->fns[i].name, fn) == 0)
		{
			node->fns[i].pc = -2;
			node->fns[i].has_arg = has_arg;
			node->fns[i].is_extern = true;
			node->fns[i].native_id = native_id;
			return;
		}
	}

	if (!ensure_fns_cap(&node->fns, &node->fn_count, &node->fns_cap))
	{
		fail(c, "out of memory");
		return;
	}

	memset(&node->fns[node->fn_count], 0, sizeof node->fns[0]);
	memcpy(node->fns[node->fn_count].name, fn, sizeof node->fns[0].name);
	node->fns[node->fn_count].pc = -2;
	node->fns[node->fn_count].has_arg = has_arg;
	node->fns[node->fn_count].is_extern = true;
	node->fns[node->fn_count].native_id = native_id;
	node->fn_count++;
}

static void compile_ns_def(Comp *c, const char *name)
{
	char qname[STGE_NAME_MAX * 2];
	char last[STGE_NAME_MAX];
	char parent[STGE_NAME_MAX * 2];
	NsRef *ns;
	int saved_self;
	int pc;
	bool has_arg = false;
	Lex saved_lex;
	Tok saved_tok;

	if (!take_colon_path(c, name, qname, sizeof qname, last, sizeof last))
	{
		if (!c->failed)
			fail(c, "expected '::'");
		return;
	}

	if (!qual_parent(qname, parent, sizeof parent))
	{
		if (strlen(name) >= sizeof parent)
		{
			fail(c, "name too long");
			return;
		}

		strcpy(parent, name);
	}

	if (!at(c, TOK_EQUAL))
	{
		fail(c, "expected '='");
		return;
	}

	saved_lex = c->lex;
	saved_tok = c->tok;
	advance(c);
	if (at(c, TOK_IDENT) && tok_eq(&c->tok, "extern"))
	{
		char cname[STGE_NAME_MAX];
		int native_id = -1;
		bool has_arg = false;

		if (c->path == NULL || strstr(c->path, "src/lang/") == NULL)
		{
			fail(c, "extern is only allowed in lang files");
			return;
		}

		advance(c);
		if (!at(c, TOK_IDENT))
		{
			fail(c, "expected a C symbol");
			return;
		}

		tok_copy(cname, sizeof cname, &c->tok);
		advance(c);
		if (at(c, TOK_LBRACE))
		{
			lex_keep_buf(&c->lex, &saved_lex);
			c->tok = saved_tok;
			advance(c);
			compile_extern_struct(c, qname);
			return;
		}

		native_id = stge_native_find(cname);
		if (native_id < 0)
		{
			failf(c, "unknown C symbol '%s'", cname);
			return;
		}

		expect(c, TOK_LPAREN, "expected '('");
		if (!at(c, TOK_RPAREN))
		{
			has_arg = true;
			while (!c->failed && !at(c, TOK_RPAREN) && !at(c, TOK_EOF))
			{
				if (at(c, TOK_IDENT))
				{
					advance(c);
					while (at(c, TOK_COLONCOLON))
					{
						advance(c);
						if (at(c, TOK_IDENT))
							advance(c);
					}
				}
				if (at(c, TOK_LBRACK))
					take_brackets(c, false, NULL);
				if (at(c, TOK_IDENT))
					advance(c);

				if (at(c, TOK_COMMA))
					advance(c);
				else
					break;
			}
		}

		expect(c, TOK_RPAREN, "expected ')'");
		if (at(c, TOK_ARROW))
		{
			advance(c);
			if (!at(c, TOK_IDENT))
			{
				fail(c, "expected a return type");
				return;
			}

			advance(c);
			if (at(c, TOK_LBRACK))
				take_brackets(c, false, NULL);
		}

		bind_extern_fn(c, parent, last, native_id, has_arg);
		return;
	}

	lex_keep_buf(&c->lex, &saved_lex);
	c->tok = saved_tok;
	advance(c);
	if (at(c, TOK_LPAREN))
	{
		lex_keep_buf(&c->lex, &saved_lex);
		c->tok = saved_tok;

		ns = ensure_ns(c, parent);
		if (ns == NULL)
			return;

		if (ns->obj_id == -1)
		{
			NodeRef *type = ensure_node(c, parent);

			if (type != NULL)
				ns->obj_id = type->id;
		}

		expect(c, TOK_EQUAL, "expected '='");
		saved_self = c->self;
		c->self = ns->obj_id;
		pc = compile_body(c, &has_arg);
		c->self = saved_self;
		if (c->failed)
			return;

		if (!ns_set_fn_arity(ns, last, pc, has_arg, c->fn_argc, c->fn_ret_type))
		{
			failf(c, "name '%s::%s' is already defined", parent, last);
			return;
		}

		{
			NodeRef *node = find_node(c, parent);

			if (node != NULL)
				store_fn(c, node, last, pc, has_arg);
		}

		patch_fn_calls(c, ns->obj_id, last, pc);
		return;
	}

	lex_keep_buf(&c->lex, &saved_lex);
	c->tok = saved_tok;
	advance(c);
	if (at(c, TOK_LBRACE))
	{
		compile_binding(c, qname);
		return;
	}

	if (word_is(c, "enum"))
	{
		compile_enum(c, qname);
		return;
	}

	lex_keep_buf(&c->lex, &saved_lex);
	c->tok = saved_tok;
	compile_member_assign(c, parent, last);
}

static void compile_field(Comp *c, const char *name)
{
	NodeRef *node;
	char field[STGE_NAME_MAX];

	if (!at(c, TOK_IDENT))
	{
		fail(c, "expected a field");
		return;
	}

	tok_copy(field, sizeof field, &c->tok);
	advance(c);

	if (strcmp(name, "Camera") == 0 || is_stge_host(name))
	{
		compile_member_assign(c, name, field);
		return;
	}

	node = find_node(c, name);
	if (node == NULL)
	{
		failf(c, "unknown node '%s'", name);
		return;
	}

	if (at(c, TOK_DOT))
	{
		char fields[STGE_MAX_PATH][STGE_NAME_MAX];
		int n = 1;

		memcpy(fields[0], field, sizeof fields[0]);
		while (at(c, TOK_DOT) && n < STGE_MAX_PATH)
		{
			advance(c);
			if (!at(c, TOK_IDENT))
			{
				fail(c, "expected a field");
				return;
			}

			tok_copy(fields[n], sizeof fields[0], &c->tok);
			n++;
			advance(c);
		}

		if (n >= 2
		    && (strcmp(fields[n - 1], "add") == 0 || strcmp(fields[n - 1], "append") == 0)
		    && at(c, TOK_LPAREN))
		{
			expect(c, TOK_LPAREN, "expected '('");
			emit_recv(c, name);
			emit_push_str(c, fields[n - 2]);
			compile_expr(c);
			expect(c, TOK_RPAREN, "expected ')'");
			emit_u8(c, OP_DYN_ADD);
			emit_u8(c, OP_POP);
			return;
		}

		compile_store_chain(c, name, fields, n);
		return;
	}

	if (at(c, TOK_EQUAL))
	{
		Lex saved_lex = c->lex;
		Tok saved_tok = c->tok;

		advance(c);
		if (at(c, TOK_IDENT) && tok_eq(&c->tok, "extern"))
		{
			char cname[STGE_NAME_MAX];
			int native_id;
			bool has_arg = false;

			if (c->path == NULL || strstr(c->path, "src/lang/") == NULL)
			{
				fail(c, "extern is only allowed in lang files");
				return;
			}

			advance(c);
			if (!at(c, TOK_IDENT))
			{
				fail(c, "expected a C symbol");
				return;
			}

			tok_copy(cname, sizeof cname, &c->tok);
			advance(c);
			native_id = stge_native_find(cname);
			if (native_id < 0)
			{
				failf(c, "unknown C symbol '%s'", cname);
				return;
			}

			expect(c, TOK_LPAREN, "expected '('");
			if (!at(c, TOK_RPAREN))
			{
				has_arg = true;
				while (!c->failed && !at(c, TOK_RPAREN) && !at(c, TOK_EOF))
				{
					if (at(c, TOK_IDENT))
					{
						advance(c);
						while (at(c, TOK_COLONCOLON))
						{
							advance(c);
							if (at(c, TOK_IDENT))
								advance(c);
						}
					}
					if (at(c, TOK_LBRACK))
						take_brackets(c, false, NULL);
					if (at(c, TOK_IDENT))
						advance(c);

					if (at(c, TOK_COMMA))
						advance(c);
					else
						break;
				}
			}

			expect(c, TOK_RPAREN, "expected ')'");
			if (at(c, TOK_ARROW))
			{
				advance(c);
				if (!at(c, TOK_IDENT))
				{
					fail(c, "expected a return type");
					return;
				}

				advance(c);
				if (at(c, TOK_LBRACK))
					take_brackets(c, false, NULL);
			}

			bind_extern_fn(c, name, field, native_id, has_arg);
			return;
		}

		if (at(c, TOK_LPAREN))
		{
			int saved_self = c->self;
			int pc;
			bool has_arg = false;

			c->self = node->id;
			pc = compile_body(c, &has_arg);
			c->self = saved_self;
			if (c->failed)
				return;

			store_fn(c, node, field, pc, has_arg);
			return;
		}

		lex_keep_buf(&c->lex, &saved_lex);
		c->tok = saved_tok;
	}

	if (strcmp(field, "parent") == 0)
	{
		const NodeRef *other;

		expect(c, TOK_EQUAL, "expected '='");
		if (!at(c, TOK_IDENT) || (other = find_node(c, tok_cstr(c, &c->tok))) == NULL)
		{
			fail(c, "expected a node name");
			return;
		}

		emit_push_node(c, other->id);
		emit_push_node(c, node->id);
		emit_u8(c, OP_ADD_CHILD);
		emit_u8(c, OP_POP);
		advance(c);
		return;
	}

	compile_member_assign(c, name, field);
}

static void compile_top_statement(Comp *c)
{
	char name[STGE_NAME_MAX];
	Lex saved_lex;
	Tok saved_tok;

	if (!at(c, TOK_IDENT))
	{
		fail(c, "expected a name");
		return;
	}

	if (word_is(c, "while"))
	{
		compile_while(c);
		return;
	}

	if (word_is(c, "use") && look(c).kind == TOK_STRING)
	{
		char *path;

		advance(c);
		if (!at(c, TOK_STRING) || c->tok.hole)
		{
			fail(c, c->tok.hole ? "string interpolation is not allowed here" : "expected a string");
			return;
		}

		path = resolve_use_path_dup(c, c->tok.text);
		advance(c);
		if (path == NULL)
		{
			fail(c, "out of memory");
			return;
		}

		if (!mark_used(c, path))
		{
			free(path);
			return;
		}

		include_pass(c, path, true);
		free(path);
		return;
	}

	if (next_is_call(c))
	{
		compile_call(c);
		emit_u8(c, OP_POP);
		return;
	}

	saved_lex = c->lex;
	saved_tok = c->tok;
	tok_copy(name, sizeof name, &c->tok);
	advance(c);

	if (at(c, TOK_EQUAL))
	{
		advance(c);
		compile_binding(c, name);
		return;
	}

	if (at_compound(c) || at(c, TOK_DOT))
	{
		if (find_slot_local(c, name) >= 0)
		{
			lex_keep_buf(&c->lex, &saved_lex);
			c->tok = saved_tok;
			compile_slot_stmt(c);
			return;
		}

		if (at_compound(c))
		{
			failf(c, "unknown name '%s'", name);
			return;
		}

		advance(c);
		compile_field(c, name);
		return;
	}

	if (at(c, TOK_COLONCOLON))
	{
		lex_keep_buf(&c->lex, &saved_lex);
		c->tok = saved_tok;
		advance(c);
		compile_ns_def(c, name);
		return;
	}

	fail(c, "expected '=', '.' or '::'");
}

static void include_lang_file(Comp *c, const char *file)
{
	static const char *roots[] = {
		"src/lang/",
		"../src/lang/",
		"../../src/lang/",
	};
	size_t i;
	const char *base;

	if (c == NULL || file == NULL)
		return;

	base = c->path != NULL ? strrchr(c->path, '/') : NULL;
	base = base != NULL ? base + 1 : c->path;
	if (base != NULL && strcmp(base, file) == 0)
		return;

	for (i = 0; i < sizeof roots / sizeof roots[0]; i++)
	{
		char path[256];
		FILE *f;

		snprintf(path, sizeof path, "%s%s", roots[i], file);
		f = fopen(path, "rb");
		if (f == NULL)
			continue;

		fclose(f);
		compile_include(c, path);
		return;
	}

	failf(c, "could not read include '%s'", file);
}

static void maybe_include_lang(Comp *c)
{
	if (c == NULL || c->include_depth > 0)
		return;

	if (find_node(c, "int") == NULL && find_node(c, "String") == NULL
	    && find_ns(c, "std") == NULL)
		include_lang_file(c, "core/core.stge");
}

static void compile_program(Comp *c)
{
	advance(c);
	maybe_include_lang(c);
	compile_file_body(c);
	emit_u8(c, OP_HALT);
}

/* ---- load ------------------------------------------------------------------ */

static char *read_file(const char *path, size_t *out_len)
{
	FILE *file;
	long size;
	char *buf;

	file = fopen(path, "rb");
	if (file == NULL)
		return NULL;

	if (fseek(file, 0, SEEK_END) != 0)
	{
		fclose(file);
		return NULL;
	}

	size = ftell(file);
	if (size < 0)
	{
		fclose(file);
		return NULL;
	}

	rewind(file);
	buf = malloc((size_t)size + 1);
	if (buf == NULL)
	{
		fclose(file);
		return NULL;
	}

	if (fread(buf, 1, (size_t)size, file) != (size_t)size)
	{
		free(buf);
		fclose(file);
		return NULL;
	}

	buf[size] = '\0';
	fclose(file);

	if (out_len != NULL)
		*out_len = (size_t)size;

	return buf;
}

bool parser_compile(const char *path)
{
	char *src;
	size_t len = 0;
	Comp *c;
	VmProg *s;
	int i;

	if (path == NULL)
		return false;

	g_parser_error = false;
	s = stge_prog();
	if (s == NULL)
		return false;

	src = read_file(path, &len);
	if (src == NULL)
	{
		fprintf(stderr, "stge: could not read %s\n", path);
		return false;
	}

	c = calloc(1, sizeof *c);
	if (c == NULL)
	{
		free(src);
		return false;
	}

	c->path = path;
	c->lex.src = src;
	c->lex.len = len;
	c->lex.pos = 0;
	c->lex.line = 1;
	c->self = -1;

	compile_program(c);
	free(c->lex.buf);
	c->lex.buf = NULL;
	lex_persist_free(&c->lex);
	for (i = 0; i < c->used_count; i++)
	{
		free(c->used[i].path);
		c->used[i].path = NULL;
	}
	free(src);

	if (c->failed)
	{
		free(c->name_scratch);
		comp_free_tables(c);
		free(c);
		return false;
	}

	s->init_pc = 0;
	for (i = 0; i < c->node_count; i++)
	{
		const NodeRef *node = &c->nodes[i];
		int id = node->id;
		int f;
		Obj *type_obj;

		type_obj = stge_obj(id);
		if (type_obj == NULL)
			continue;

		for (f = 0; f < node->fn_count; f++)
		{
			const ObjFn *fn = &node->fns[f];

			obj_fn_set(type_obj, fn->name, fn->pc, fn->argc, fn->has_arg,
				   fn->is_extern, fn->native_id);
		}
	}

	free(c->name_scratch);
	comp_free_tables(c);
	free(c);
	return true;
}

bool parser_load(const char *path)
{
	return parser_compile(path);
}
