#ifndef INF_LANG_LEXER_H
#define INF_LANG_LEXER_H

/* Hand-written lexer for the .story surface language (DESIGN.md §10).
 *
 * Tokens point into the source buffer (start/len), which must outlive them.
 * Maximal munch on multi-char operators (->, =>, ~>, >=, <=). Both line and
 * block comments are skipped. Line/col are 1-based and track the token's
 * first character, for author-facing diagnostics. */

typedef enum {
    TK_EOF, TK_ERROR,
    TK_IDENT, TK_INT,

    /* reserved words — the settled structural keywords of §6, reserved now
     * so the lexer is stable even though this slice's parser handles only a
     * subset (state/init/rule/action/requires/causes/unless). */
    TK_SORT, TK_ENTITY, TK_STATE, TK_INIT, TK_PROVIDER,
    TK_RULE, TK_ACTION, TK_REQUIRES, TK_CAUSES, TK_UNLESS,
    TK_MODULE, TK_EXTEND, TK_SCENE, TK_IN,

    TK_LPAREN, TK_RPAREN, TK_LBRACE, TK_RBRACE,
    TK_COLON, TK_COMMA, TK_AMP, TK_TILDE,
    TK_ARROW,      /* ->  strict rule     */
    TK_FATARROW,   /* =>  defeasible rule */
    TK_SQARROW,    /* ~>  defeater        */
    TK_GT,         /* >   superiority     */
    TK_LT,         /* <                   */
    TK_GE,         /* >=                  */
    TK_LE,         /* <=                  */
    TK_EQ,         /* =                   */
    TK_MINUS,      /* -                   */

    /* numeric effects and expressions (§5.8 write side) */
    TK_ASSIGN,     /* :=  absolute effect */
    TK_PLUSEQ,     /* +=  additive effect */
    TK_MINUSEQ,    /* -=  additive effect */
    TK_PLUS,       /* +                   */
    TK_STAR,       /* *                   */
    TK_DOTDOT,     /* ..  range           */
    TK_PRIME       /* '   next-state mark (ramification bodies, §5.4) */
} tok_kind;

typedef struct {
    tok_kind    kind;
    const char *start;   /* into the source buffer */
    int         len;
    int         line, col;
    long        ival;    /* valid for TK_INT */
} token;

typedef struct {
    const char *p;
    int         line, col;
} lexer;

void  lexer_init(lexer *lx, const char *src);
token lexer_next(lexer *lx);

/* Human-readable spelling for diagnostics ("expected ':', found …"). */
const char *tok_kind_name(tok_kind k);

#endif
