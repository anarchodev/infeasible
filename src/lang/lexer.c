#include "lang/lexer.h"

#include <stdbool.h>
#include <string.h>

void lexer_init(lexer *lx, const char *src)
{
    lx->p = src;
    lx->line = 1;
    lx->col = 1;
}

/* Advance one byte, maintaining line/col. */
static char adv(lexer *lx)
{
    char c = *lx->p++;
    if (c == '\n') { lx->line++; lx->col = 1; }
    else           { lx->col++; }
    return c;
}

static bool is_ident_start(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}
static bool is_ident_cont(char c)
{
    return is_ident_start(c) || (c >= '0' && c <= '9');
}
static bool is_digit(char c) { return c >= '0' && c <= '9'; }

/* Skip whitespace and both comment forms. Returns after the last skipped
 * char, positioned on the next significant byte. */
static void skip_trivia(lexer *lx)
{
    for (;;) {
        char c = *lx->p;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            adv(lx);
        } else if (c == '/' && lx->p[1] == '/') {
            while (*lx->p && *lx->p != '\n') adv(lx);
        } else if (c == '/' && lx->p[1] == '*') {
            adv(lx); adv(lx);
            while (*lx->p && !(*lx->p == '*' && lx->p[1] == '/')) adv(lx);
            if (*lx->p) { adv(lx); adv(lx); }   /* consume closing star-slash */
        } else {
            return;
        }
    }
}

static const struct { const char *word; tok_kind kind; } keywords[] = {
    { "sort", TK_SORT },       { "entity", TK_ENTITY }, { "state", TK_STATE },
    { "init", TK_INIT },       { "provider", TK_PROVIDER },
    { "rule", TK_RULE },       { "action", TK_ACTION }, { "requires", TK_REQUIRES },
    { "causes", TK_CAUSES },   { "unless", TK_UNLESS }, { "module", TK_MODULE },
    { "extend", TK_EXTEND },   { "scene", TK_SCENE },   { "in", TK_IN },
};

static tok_kind keyword_lookup(const char *s, int len)
{
    for (size_t i = 0; i < sizeof keywords / sizeof keywords[0]; i++)
        if ((int)strlen(keywords[i].word) == len &&
            memcmp(keywords[i].word, s, (size_t)len) == 0)
            return keywords[i].kind;
    return TK_IDENT;
}

token lexer_next(lexer *lx)
{
    skip_trivia(lx);

    token t;
    t.start = lx->p;
    t.line = lx->line;
    t.col = lx->col;
    t.len = 0;
    t.ival = 0;

    char c = *lx->p;
    if (c == '\0') { t.kind = TK_EOF; return t; }

    if (is_ident_start(c)) {
        while (is_ident_cont(*lx->p)) adv(lx);
        t.len = (int)(lx->p - t.start);
        t.kind = keyword_lookup(t.start, t.len);
        return t;
    }

    if (is_digit(c)) {
        long v = 0;
        while (is_digit(*lx->p)) { v = v * 10 + (*lx->p - '0'); adv(lx); }
        t.len = (int)(lx->p - t.start);
        t.kind = TK_INT;
        t.ival = v;
        return t;
    }

    /* operators and punctuation, maximal munch on the two-char forms */
    adv(lx);
    switch (c) {
    case '(': t.kind = TK_LPAREN; break;
    case ')': t.kind = TK_RPAREN; break;
    case '{': t.kind = TK_LBRACE; break;
    case '}': t.kind = TK_RBRACE; break;
    case ':': t.kind = TK_COLON;  break;
    case ',': t.kind = TK_COMMA;  break;
    case '&': t.kind = TK_AMP;    break;
    case '~': if (*lx->p == '>') { adv(lx); t.kind = TK_SQARROW; }
              else                 t.kind = TK_TILDE;
              break;
    case '-': if (*lx->p == '>') { adv(lx); t.kind = TK_ARROW; }
              else                 t.kind = TK_MINUS;
              break;
    case '=': if (*lx->p == '>') { adv(lx); t.kind = TK_FATARROW; }
              else                 t.kind = TK_EQ;
              break;
    case '>': if (*lx->p == '=') { adv(lx); t.kind = TK_GE; }
              else                 t.kind = TK_GT;
              break;
    case '<': if (*lx->p == '=') { adv(lx); t.kind = TK_LE; }
              else                 t.kind = TK_LT;
              break;
    default:  t.kind = TK_ERROR;  break;
    }
    t.len = (int)(lx->p - t.start);
    return t;
}

const char *tok_kind_name(tok_kind k)
{
    switch (k) {
    case TK_EOF:      return "end of input";
    case TK_ERROR:    return "unexpected character";
    case TK_IDENT:    return "identifier";
    case TK_INT:      return "integer";
    case TK_SORT:     return "'sort'";
    case TK_ENTITY:   return "'entity'";
    case TK_STATE:    return "'state'";
    case TK_INIT:     return "'init'";
    case TK_PROVIDER: return "'provider'";
    case TK_RULE:     return "'rule'";
    case TK_ACTION:   return "'action'";
    case TK_REQUIRES: return "'requires'";
    case TK_CAUSES:   return "'causes'";
    case TK_UNLESS:   return "'unless'";
    case TK_MODULE:   return "'module'";
    case TK_EXTEND:   return "'extend'";
    case TK_SCENE:    return "'scene'";
    case TK_IN:       return "'in'";
    case TK_LPAREN:   return "'('";
    case TK_RPAREN:   return "')'";
    case TK_LBRACE:   return "'{'";
    case TK_RBRACE:   return "'}'";
    case TK_COLON:    return "':'";
    case TK_COMMA:    return "','";
    case TK_AMP:      return "'&'";
    case TK_TILDE:    return "'~'";
    case TK_ARROW:    return "'->'";
    case TK_FATARROW: return "'=>'";
    case TK_SQARROW:  return "'~>'";
    case TK_GT:       return "'>'";
    case TK_LT:       return "'<'";
    case TK_GE:       return "'>='";
    case TK_LE:       return "'<='";
    case TK_EQ:       return "'='";
    case TK_MINUS:    return "'-'";
    }
    return "?";
}
