#include <gtk/gtk.h>
#include <gtksourceview/gtksource.h>
#include <cairo-pdf.h>
#include <string.h>
#include <stdlib.h>
#include "gui_internal.h"

/* ── Themed PDF export ──
 * Typora-style export templates rendered directly onto a
 * cairo_pdf_surface (not GtkPrintOperation), which buys us: PDF outline
 * bookmarks from headings, document metadata, and exact page control.
 *
 * The "متن" (Matn) theme emulates the GENRE of classical Arabic matn
 * typesetting — naskh body (Amiri), double-rule frame, rubrication
 * (structural emphasis in accent ink), Eastern Arabic folios — it is our
 * own take on the tradition, not a reproduction of any publisher's
 * template. */

extern void qirtas_export_editor_look(AppGui *gui); /* legacy compositor path */

/* ── Theme definition: adding a theme is data, not code ── */
typedef struct {
    const char *id;
    const char *name;       /* shown in chooser */
    const char *sample;     /* preview line */
    /* page (points; A4 = 595.276 × 841.890) */
    double page_w, page_h;
    double margin_top, margin_bottom, margin_inner, margin_outer;
    /* type */
    const char *body_font;
    const char *heading_font;
    const char *code_font;
    double body_size;
    double line_spacing;    /* multiplier */
    gboolean justify;
    gboolean rtl_furniture; /* page furniture + gutter mirrored for RTL book */
    double h_scale[3];      /* h1, h2, h3+ relative to body */
    gboolean heading_center;
    gboolean number_headings;
    /* ink */
    const char *ink;
    const char *accent;
    gboolean rubricate_bold;   /* bold → accent ink */
    gboolean quote_accent;     /* blockquote = matn layer, accent + larger */
    /* furniture */
    gboolean frame;            /* double-rule border */
    gboolean eastern_folios;   /* page numbers ٠١٢, ‹ ٥ › style */
    gboolean title_header;     /* doc title in top margin */
} PrintTheme;

static const PrintTheme THEMES[] = {
    {
        .id = "matn", .name = "متن — Classical", .sample = "قَالَ المُصنِّفُ رحمه الله",
        .page_w = 595.276, .page_h = 841.890,
        .margin_top = 78, .margin_bottom = 84, .margin_inner = 78, .margin_outer = 64,
        .body_font = "Amiri", .heading_font = "Amiri", .code_font = "monospace",
        .body_size = 14.5, .line_spacing = 1.9, .justify = TRUE, .rtl_furniture = TRUE,
        .h_scale = { 1.7, 1.4, 1.2 }, .heading_center = TRUE, .number_headings = FALSE,
        .ink = "#1b1816", .accent = "#8a1c1c",
        .rubricate_bold = TRUE, .quote_accent = TRUE,
        .frame = TRUE, .eastern_folios = TRUE, .title_header = TRUE,
    },
    {
        .id = "paper-ink", .name = "Paper & Ink", .sample = "The app identity, on paper",
        .page_w = 595.276, .page_h = 841.890,
        .margin_top = 64, .margin_bottom = 70, .margin_inner = 60, .margin_outer = 56,
        .body_font = "Inter", .heading_font = "Cairo", .code_font = "JetBrains Mono",
        .body_size = 11.5, .line_spacing = 1.55, .justify = FALSE, .rtl_furniture = FALSE,
        .h_scale = { 1.8, 1.45, 1.2 }, .heading_center = FALSE, .number_headings = FALSE,
        .ink = "#1b1816", .accent = "#16324f",
        .rubricate_bold = FALSE, .quote_accent = FALSE,
        .frame = FALSE, .eastern_folios = FALSE, .title_header = FALSE,
    },
    {
        .id = "academic", .name = "Academic", .sample = "1.2 Numbered structure",
        .page_w = 595.276, .page_h = 841.890,
        .margin_top = 72, .margin_bottom = 78, .margin_inner = 86, .margin_outer = 72,
        .body_font = "Lora", .heading_font = "Lora", .code_font = "monospace",
        .body_size = 11.0, .line_spacing = 1.5, .justify = TRUE, .rtl_furniture = FALSE,
        .h_scale = { 1.6, 1.3, 1.12 }, .heading_center = FALSE, .number_headings = TRUE,
        .ink = "#111111", .accent = "#111111",
        .rubricate_bold = FALSE, .quote_accent = FALSE,
        .frame = FALSE, .eastern_folios = FALSE, .title_header = TRUE,
    },
    {
        .id = "typewriter", .name = "Typewriter", .sample = "ragged right, fixed pitch",
        .page_w = 595.276, .page_h = 841.890,
        .margin_top = 70, .margin_bottom = 76, .margin_inner = 70, .margin_outer = 70,
        .body_font = "JetBrains Mono", .heading_font = "JetBrains Mono", .code_font = "JetBrains Mono",
        .body_size = 10.5, .line_spacing = 1.5, .justify = FALSE, .rtl_furniture = FALSE,
        .h_scale = { 1.3, 1.15, 1.0 }, .heading_center = FALSE, .number_headings = FALSE,
        .ink = "#222222", .accent = "#222222",
        .rubricate_bold = FALSE, .quote_accent = FALSE,
        .frame = FALSE, .eastern_folios = FALSE, .title_header = FALSE,
    },
};
#define N_THEMES ((int)G_N_ELEMENTS(THEMES))

/* ── Block model ── */
typedef enum { BLK_PARA, BLK_HEADING, BLK_QUOTE, BLK_LIST, BLK_CODE, BLK_HR } BlockType;

typedef struct {
    BlockType type;
    int level;     /* heading level / list indent */
    GString *text; /* raw markdown content (prefix stripped) */
} Block;

static void block_free(gpointer data) {
    Block *b = (Block *)data;
    if (b->text) g_string_free(b->text, TRUE);
    g_free(b);
}

static Block *block_new(BlockType t, int level) {
    Block *b = g_new0(Block, 1);
    b->type = t;
    b->level = level;
    b->text = g_string_new("");
    return b;
}

/* Walk the document into blocks. Same line-prefix vocabulary as the
 * conceal pass — do not grow a second markdown dialect here. */
static GPtrArray *parse_blocks(const char *doc) {
    GPtrArray *blocks = g_ptr_array_new_with_free_func(block_free);
    gchar **lines = g_strsplit(doc, "\n", -1);
    Block *cur = NULL;
    gboolean in_code = FALSE;

    for (int i = 0; lines[i]; i++) {
        const char *line = lines[i];
        const char *p = line;
        while (*p == ' ' || *p == '\t') p++;

        if (in_code) {
            if (strncmp(p, "```", 3) == 0) { in_code = FALSE; cur = NULL; continue; }
            if (!cur || cur->type != BLK_CODE) { cur = block_new(BLK_CODE, 0); g_ptr_array_add(blocks, cur); }
            if (cur->text->len) g_string_append_c(cur->text, '\n');
            g_string_append(cur->text, line);
            continue;
        }
        if (strncmp(p, "```", 3) == 0) { in_code = TRUE; cur = NULL; continue; }

        if (*p == '\0') { cur = NULL; continue; }

        int h = 0;
        while (p[h] == '#') h++;
        if (h >= 1 && h <= 6 && p[h] == ' ') {
            cur = block_new(BLK_HEADING, h);
            g_string_append(cur->text, p + h + 1);
            g_ptr_array_add(blocks, cur);
            cur = NULL;
            continue;
        }
        if (strcmp(p, "---") == 0 || strcmp(p, "***") == 0 || strcmp(p, "___") == 0) {
            g_ptr_array_add(blocks, block_new(BLK_HR, 0));
            cur = NULL;
            continue;
        }
        if (*p == '>') {
            const char *q = p + 1;
            if (*q == ' ') q++;
            if (!cur || cur->type != BLK_QUOTE) { cur = block_new(BLK_QUOTE, 0); g_ptr_array_add(blocks, cur); }
            if (cur->text->len) g_string_append_c(cur->text, ' ');
            g_string_append(cur->text, q);
            continue;
        }
        {
            int indent = (int)((p - line) / 2);
            gboolean is_bullet = ((p[0] == '-' || p[0] == '*' || p[0] == '+') && p[1] == ' ');
            int nd = 0;
            while (p[nd] >= '0' && p[nd] <= '9') nd++;
            gboolean is_num = (nd > 0 && p[nd] == '.' && p[nd + 1] == ' ');
            if (is_bullet || is_num) {
                Block *b = block_new(BLK_LIST, indent);
                if (is_bullet) {
                    /* checkbox? */
                    const char *c = p + 2;
                    if (strncmp(c, "[ ] ", 4) == 0)      { g_string_append(b->text, "☐ "); c += 4; }
                    else if (strncmp(c, "[x] ", 4) == 0 ||
                             strncmp(c, "[X] ", 4) == 0) { g_string_append(b->text, "☑ "); c += 4; }
                    else                                  g_string_append(b->text, "• ");
                    g_string_append(b->text, c);
                } else {
                    g_string_append_len(b->text, p, nd + 1);
                    g_string_append_c(b->text, ' ');
                    g_string_append(b->text, p + nd + 2);
                }
                g_ptr_array_add(blocks, b);
                cur = NULL;
                continue;
            }
        }
        /* paragraph continuation */
        if (!cur || cur->type != BLK_PARA) { cur = block_new(BLK_PARA, 0); g_ptr_array_add(blocks, cur); }
        if (cur->text->len) g_string_append_c(cur->text, ' ');
        g_string_append(cur->text, p);
    }
    g_strfreev(lines);
    return blocks;
}

/* ── Inline markdown → plain text + PangoAttrList ──
 * Handles **bold**, *italic*, `code`, ~~strike~~, ==highlight==,
 * [text](url) → text. Attr indices are BYTE offsets into the output. */
static void add_color(PangoAttrList *attrs, guint start, guint end, const GdkRGBA *c) {
    PangoAttribute *a = pango_attr_foreground_new(
        (guint16)(c->red * 65535), (guint16)(c->green * 65535), (guint16)(c->blue * 65535));
    a->start_index = start; a->end_index = end;
    pango_attr_list_insert(attrs, a);
}

static void parse_inline(const char *src, GString *out, PangoAttrList *attrs,
                         const PrintTheme *t, const GdkRGBA *accent) {
    const char *p = src;
    while (*p) {
        if (strncmp(p, "**", 2) == 0) {
            const char *end = strstr(p + 2, "**");
            if (end && end > p + 2) {
                guint s = out->len;
                g_string_append_len(out, p + 2, end - (p + 2));
                PangoAttribute *b = pango_attr_weight_new(PANGO_WEIGHT_BOLD);
                b->start_index = s; b->end_index = out->len;
                pango_attr_list_insert(attrs, b);
                if (t->rubricate_bold) add_color(attrs, s, out->len, accent);
                p = end + 2;
                continue;
            }
        }
        if (*p == '*' && p[1] && p[1] != '*') {
            const char *end = strchr(p + 1, '*');
            if (end && end > p + 1) {
                guint s = out->len;
                g_string_append_len(out, p + 1, end - (p + 1));
                PangoAttribute *a = pango_attr_style_new(PANGO_STYLE_ITALIC);
                a->start_index = s; a->end_index = out->len;
                pango_attr_list_insert(attrs, a);
                p = end + 1;
                continue;
            }
        }
        if (*p == '`') {
            const char *end = strchr(p + 1, '`');
            if (end && end > p + 1) {
                guint s = out->len;
                g_string_append_len(out, p + 1, end - (p + 1));
                PangoAttribute *f = pango_attr_family_new(t->code_font);
                f->start_index = s; f->end_index = out->len;
                pango_attr_list_insert(attrs, f);
                p = end + 1;
                continue;
            }
        }
        if (strncmp(p, "~~", 2) == 0) {
            const char *end = strstr(p + 2, "~~");
            if (end && end > p + 2) {
                guint s = out->len;
                g_string_append_len(out, p + 2, end - (p + 2));
                PangoAttribute *a = pango_attr_strikethrough_new(TRUE);
                a->start_index = s; a->end_index = out->len;
                pango_attr_list_insert(attrs, a);
                p = end + 2;
                continue;
            }
        }
        if (strncmp(p, "==", 2) == 0) {
            const char *end = strstr(p + 2, "==");
            if (end && end > p + 2) {
                guint s = out->len;
                g_string_append_len(out, p + 2, end - (p + 2));
                add_color(attrs, s, out->len, accent);
                p = end + 2;
                continue;
            }
        }
        if (*p == '[') {
            const char *mid = strstr(p, "](");
            const char *close = mid ? strchr(mid, ')') : NULL;
            if (mid && close) {
                g_string_append_len(out, p + 1, mid - (p + 1));
                p = close + 1;
                continue;
            }
        }
        g_string_append_len(out, p, g_utf8_next_char(p) - p);
        p = g_utf8_next_char(p);
    }
}

/* ── Renderer ── */
typedef struct {
    cairo_t *cr;
    cairo_surface_t *surface;
    const PrintTheme *t;
    GdkRGBA ink, accent;
    int page_num;          /* 1-based */
    double y;              /* current baseline-area cursor (top of next line box) */
    double content_x, content_w, content_top, content_bottom;
    const char *doc_title;
    PangoContext *pctx;    /* from a widget, correct font map */
    int h_counters[3];
    int outline_parents[7];
} Render;

static void draw_page_furniture(Render *r);

static void start_page(Render *r) {
    r->page_num++;
    const PrintTheme *t = r->t;
    /* Mirror gutter: page 1 of an RTL book is a right-hand page → inner
     * (binding) margin on the LEFT of the sheet; LTR book is the inverse. */
    gboolean right_hand = (r->page_num % 2) == 1;
    double m_left, m_right;
    gboolean inner_left = t->rtl_furniture ? right_hand : !right_hand;
    m_left = inner_left ? t->margin_inner : t->margin_outer;
    m_right = inner_left ? t->margin_outer : t->margin_inner;

    r->content_x = m_left;
    r->content_w = t->page_w - m_left - m_right;
    r->content_top = t->margin_top;
    r->content_bottom = t->page_h - t->margin_bottom;
    r->y = r->content_top;
    draw_page_furniture(r);
}

static void end_page(Render *r) {
    cairo_show_page(r->cr);
}

static void draw_page_furniture(Render *r) {
    const PrintTheme *t = r->t;
    cairo_t *cr = r->cr;

    if (t->frame) {
        /* double rule: thin outer, thicker inner, ~6pt apart */
        double pad_out = 18.0, pad_in = 24.0;
        gdk_cairo_set_source_rgba(cr, &r->accent);
        cairo_set_line_width(cr, 0.8);
        cairo_rectangle(cr, pad_out, pad_out, t->page_w - 2 * pad_out, t->page_h - 2 * pad_out);
        cairo_stroke(cr);
        cairo_set_line_width(cr, 1.8);
        cairo_rectangle(cr, pad_in, pad_in, t->page_w - 2 * pad_in, t->page_h - 2 * pad_in);
        cairo_stroke(cr);
    }

    PangoLayout *pl = pango_layout_new(r->pctx);
    PangoFontDescription *fd = pango_font_description_from_string(t->body_font);
    pango_font_description_set_absolute_size(fd, t->body_size * 0.78 * PANGO_SCALE);
    pango_layout_set_font_description(pl, fd);

    /* footer folio */
    char num[32], shown[96];
    snprintf(num, sizeof(num), "%d", r->page_num);
    if (t->eastern_folios) {
        char ar[64];
        arabize_digits(num, ar, sizeof(ar));
        snprintf(shown, sizeof(shown), "‹ %s ›", ar);
    } else {
        snprintf(shown, sizeof(shown), "%s", num);
    }
    pango_layout_set_text(pl, shown, -1);
    int pw, ph;
    pango_layout_get_pixel_size(pl, &pw, &ph);
    gdk_cairo_set_source_rgba(cr, &r->ink);
    cairo_move_to(cr, (t->page_w - pw) / 2.0, t->page_h - t->margin_bottom + (t->margin_bottom - ph) / 2.0);
    pango_cairo_show_layout(cr, pl);

    /* header: document title */
    if (t->title_header && r->doc_title && r->doc_title[0] && r->page_num > 1) {
        pango_layout_set_text(pl, r->doc_title, -1);
        pango_layout_get_pixel_size(pl, &pw, &ph);
        cairo_move_to(cr, (t->page_w - pw) / 2.0, (t->margin_top - ph) / 2.0 + 6.0);
        pango_cairo_show_layout(cr, pl);
    }

    pango_font_description_free(fd);
    g_object_unref(pl);
}

static PangoLayout *make_block_layout(Render *r, const Block *b, double *out_size) {
    const PrintTheme *t = r->t;
    PangoLayout *pl = pango_layout_new(r->pctx);

    double size = t->body_size;
    const char *family = t->body_font;
    gboolean center = FALSE, justify = t->justify, bold = FALSE;

    if (b->type == BLK_HEADING) {
        int idx = b->level <= 1 ? 0 : (b->level == 2 ? 1 : 2);
        size = t->body_size * t->h_scale[idx];
        family = t->heading_font;
        center = t->heading_center;
        justify = FALSE;
        bold = TRUE;
    } else if (b->type == BLK_CODE) {
        family = t->code_font;
        size = t->body_size * 0.85;
        justify = FALSE;
    } else if (b->type == BLK_QUOTE && t->quote_accent) {
        size = t->body_size * 1.08;
    }

    PangoFontDescription *fd = pango_font_description_from_string(family);
    pango_font_description_set_absolute_size(fd, size * PANGO_SCALE);
    if (bold) pango_font_description_set_weight(fd, PANGO_WEIGHT_BOLD);
    pango_layout_set_font_description(pl, fd);
    pango_font_description_free(fd);

    double indent = (b->type == BLK_LIST) ? b->level * 16.0 + 8.0 : 0.0;
    pango_layout_set_width(pl, (int)((r->content_w - indent) * PANGO_SCALE));
    pango_layout_set_wrap(pl, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_justify(pl, justify && b->type != BLK_LIST);
    pango_layout_set_alignment(pl, center ? PANGO_ALIGN_CENTER : PANGO_ALIGN_LEFT);
    pango_layout_set_auto_dir(pl, TRUE); /* per-paragraph first-strong BiDi */
    pango_layout_set_line_spacing(pl, (float)t->line_spacing);

    /* content + inline attrs */
    GString *text = g_string_new("");
    PangoAttrList *attrs = pango_attr_list_new();

    if (b->type == BLK_HEADING && t->number_headings && b->level <= 3) {
        r->h_counters[b->level - 1]++;
        for (int i = b->level; i < 3; i++) r->h_counters[i] = 0;
        char prefix[32] = "";
        if (b->level == 1) snprintf(prefix, sizeof(prefix), "%d  ", r->h_counters[0]);
        else if (b->level == 2) snprintf(prefix, sizeof(prefix), "%d.%d  ", r->h_counters[0], r->h_counters[1]);
        else snprintf(prefix, sizeof(prefix), "%d.%d.%d  ", r->h_counters[0], r->h_counters[1], r->h_counters[2]);
        g_string_append(text, prefix);
    }

    if (b->type == BLK_CODE) {
        g_string_append(text, b->text->str);
    } else {
        parse_inline(b->text->str, text, attrs, t, &r->accent);
    }

    if (b->type == BLK_HEADING && t->rubricate_bold) {
        add_color(attrs, 0, (guint)text->len, &r->accent);
    }
    if (b->type == BLK_QUOTE && t->quote_accent) {
        add_color(attrs, 0, (guint)text->len, &r->accent);
    }

    pango_layout_set_text(pl, text->str, -1);
    pango_layout_set_attributes(pl, attrs);
    pango_attr_list_unref(attrs);
    g_string_free(text, TRUE);

    if (out_size) *out_size = size;
    return pl;
}

static double block_space_after(const PrintTheme *t, const Block *b) {
    switch (b->type) {
        case BLK_HEADING: return t->body_size * 0.9;
        case BLK_LIST:    return t->body_size * 0.25;
        default:          return t->body_size * 0.7;
    }
}

/* Render one block, splitting across pages line-by-line.
 * Orphan/widow rule: never leave a single line of a multi-line block
 * alone on either side of a break. */
static void render_block(Render *r, const Block *b, GArray *outline_pages) {
    const PrintTheme *t = r->t;

    if (b->type == BLK_HR) {
        double w = r->content_w * 0.34;
        double lh = t->body_size * t->line_spacing;
        if (r->y + lh > r->content_bottom) { end_page(r); start_page(r); }
        gdk_cairo_set_source_rgba(r->cr, &r->accent);
        cairo_set_line_width(r->cr, 1.0);
        cairo_move_to(r->cr, r->content_x + (r->content_w - w) / 2.0, r->y + lh / 2.0);
        cairo_rel_line_to(r->cr, w, 0);
        cairo_stroke(r->cr);
        r->y += lh + block_space_after(t, b);
        return;
    }

    PangoLayout *pl = make_block_layout(r, b, NULL);
    double indent = (b->type == BLK_LIST) ? b->level * 16.0 + 8.0 : 0.0;
    int n_lines = pango_layout_get_line_count(pl);

    /* heading keeps with at least two following body lines */
    PangoRectangle ext;
    pango_layout_get_extents(pl, NULL, &ext);
    double total_h = (double)ext.height / PANGO_SCALE;
    double avg_line = total_h / (n_lines > 0 ? n_lines : 1);

    double need_ahead = (b->type == BLK_HEADING) ? total_h + 2.2 * avg_line
                       : (n_lines > 1 ? 2.0 * avg_line : avg_line);
    if (r->y + need_ahead > r->content_bottom && r->y > r->content_top + 1.0) {
        end_page(r);
        start_page(r);
    }

    if (b->type == BLK_HEADING) {
        /* record outline destination at top of heading */
        int data[2] = { r->page_num, b->level };
        g_array_append_val(outline_pages, data[0]);
        char attr[128];
        snprintf(attr, sizeof(attr), "page=%d pos=[%f %f]", r->page_num, r->content_x, r->y);
        int parent = (b->level >= 2) ? r->outline_parents[b->level - 1] : CAIRO_PDF_OUTLINE_ROOT;
        int id = cairo_pdf_surface_add_outline(r->surface, parent, b->text->str, attr, 0);
        if (b->level < 6) r->outline_parents[b->level] = id;
    }

    /* code block background */
    if (b->type == BLK_CODE) {
        cairo_save(r->cr);
        cairo_set_source_rgba(r->cr, 0, 0, 0, 0.05);
        double bh = total_h + 12.0;
        double avail = r->content_bottom - r->y;
        cairo_rectangle(r->cr, r->content_x - 4, r->y - 2, r->content_w + 8, MIN(bh, avail));
        cairo_fill(r->cr);
        cairo_restore(r->cr);
        r->y += 4.0;
    }

    /* quote bar for non-accent themes */
    gboolean quote_bar = (b->type == BLK_QUOTE && !t->quote_accent);

    PangoLayoutIter *it = pango_layout_get_iter(pl);
    double quote_bar_start = r->y;
    do {
        PangoLayoutLine *line = pango_layout_iter_get_line_readonly(it);
        PangoRectangle lext;
        pango_layout_iter_get_line_extents(it, NULL, &lext);
        double line_h = (double)lext.height / PANGO_SCALE;
        int baseline_pango = pango_layout_iter_get_baseline(it);
        PangoRectangle l0;
        pango_layout_index_to_pos(pl, 0, &l0); /* unused, keeps iter API honest */
        (void)l0;
        double baseline_in_line = (double)(baseline_pango - lext.y) / PANGO_SCALE;

        if (r->y + line_h > r->content_bottom) {
            if (quote_bar && r->y > quote_bar_start) {
                gdk_cairo_set_source_rgba(r->cr, &r->accent);
                cairo_set_line_width(r->cr, 2.0);
                cairo_move_to(r->cr, r->content_x - 8, quote_bar_start);
                cairo_line_to(r->cr, r->content_x - 8, r->y);
                cairo_stroke(r->cr);
            }
            end_page(r);
            start_page(r);
            quote_bar_start = r->y;
        }

        gdk_cairo_set_source_rgba(r->cr, &r->ink);
        cairo_move_to(r->cr,
                      r->content_x + indent + (double)lext.x / PANGO_SCALE,
                      r->y + baseline_in_line);
        pango_cairo_show_layout_line(r->cr, line);
        r->y += line_h;
    } while (pango_layout_iter_next_line(it));
    pango_layout_iter_free(it);

    if (quote_bar && r->y > quote_bar_start) {
        gdk_cairo_set_source_rgba(r->cr, &r->accent);
        cairo_set_line_width(r->cr, 2.0);
        cairo_move_to(r->cr, r->content_x - 8, quote_bar_start);
        cairo_line_to(r->cr, r->content_x - 8, r->y);
        cairo_stroke(r->cr);
    }
    if (b->type == BLK_CODE) r->y += 8.0;

    r->y += block_space_after(t, b);
    g_object_unref(pl);
}

static gboolean export_with_theme(AppGui *gui, const PrintTheme *t, const char *pdf_path) {
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(gui->source_view));
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(buf, &s, &e);
    gchar *doc = gtk_text_buffer_get_text(buf, &s, &e, TRUE);

    cairo_surface_t *surface = cairo_pdf_surface_create(pdf_path, t->page_w, t->page_h);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        g_free(doc);
        cairo_surface_destroy(surface);
        return FALSE;
    }

    const char *title = "Qirtas Document";
    if (gui->active_tab_index != -1 && gui->active_tab_index < gui->num_tabs)
        title = gui->open_tabs[gui->active_tab_index];
    cairo_pdf_surface_set_metadata(surface, CAIRO_PDF_METADATA_TITLE, title);
    cairo_pdf_surface_set_metadata(surface, CAIRO_PDF_METADATA_CREATOR, "Qirtas");

    Render r = {0};
    r.cr = cairo_create(surface);
    r.surface = surface;
    r.t = t;
    gdk_rgba_parse(&r.ink, t->ink);
    gdk_rgba_parse(&r.accent, t->accent);
    r.doc_title = title;
    r.pctx = gtk_widget_create_pango_context(gui->source_view);
    r.page_num = 0;
    for (int i = 0; i < 7; i++) r.outline_parents[i] = CAIRO_PDF_OUTLINE_ROOT;

    GArray *outline_pages = g_array_new(FALSE, FALSE, sizeof(int));
    GPtrArray *blocks = parse_blocks(doc);

    start_page(&r);
    for (guint i = 0; i < blocks->len; i++) {
        render_block(&r, g_ptr_array_index(blocks, i), outline_pages);
    }
    end_page(&r);

    g_ptr_array_free(blocks, TRUE);
    g_array_free(outline_pages, TRUE);
    g_object_unref(r.pctx);
    cairo_destroy(r.cr);
    cairo_surface_finish(surface);
    gboolean ok = cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS;
    cairo_surface_destroy(surface);
    g_free(doc);
    return ok;
}

/* ── Chooser dialog ── */
typedef struct {
    AppGui *gui;
    int theme_idx; /* -1 = editor look */
} ExportChoice;

static void on_export_save_response(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source_object);
    ExportChoice *ec = (ExportChoice *)user_data;
    GError *error = NULL;
    GFile *file = gtk_file_dialog_save_finish(dialog, res, &error);
    if (file) {
        char *path = g_file_get_path(file);
        if (path) {
            gboolean ok = export_with_theme(ec->gui, &THEMES[ec->theme_idx], path);
            gui_set_sync_status(ok ? "PDF exported" : "PDF export failed");
            g_free(path);
        }
        g_object_unref(file);
    } else if (error) {
        g_clear_error(&error);
    }
    g_free(ec);
}

static void launch_save_dialog(AppGui *gui, int theme_idx) {
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, qirtas_tr("Export to PDF"));
    gtk_file_dialog_set_initial_name(dialog, "document.pdf");

    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "PDF Documents");
    gtk_file_filter_add_pattern(filter, "*.pdf");
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, filter);
    g_object_unref(filter);
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    g_object_unref(filters);

    ExportChoice *ec = g_new0(ExportChoice, 1);
    ec->gui = gui;
    ec->theme_idx = theme_idx;
    gtk_file_dialog_save(dialog, GTK_WINDOW(gui->window), NULL, on_export_save_response, ec);
}

static void on_theme_card_clicked(GtkButton *btn, gpointer user_data) {
    AppGui *gui = (AppGui *)user_data;
    int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "theme-idx"));
    GtkWidget *win = gtk_widget_get_ancestor(GTK_WIDGET(btn), GTK_TYPE_WINDOW);
    if (win) gtk_window_destroy(GTK_WINDOW(win));

    if (idx < 0) {
        qirtas_export_editor_look(gui);
        return;
    }
    qirtas_pref_set_string("export_theme", THEMES[idx].id);
    launch_save_dialog(gui, idx);
}

void qirtas_export_to_pdf(AppGui *gui) {
    if (!gui || !gui->window) return;

    GtkWidget *win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(win), qirtas_tr("Export to PDF"));
    gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(gui->window));
    gtk_window_set_modal(GTK_WINDOW(win), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(win), 380, -1);
    gtk_widget_add_css_class(win, "settings-sheet-window");

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(vbox, 14);
    gtk_widget_set_margin_end(vbox, 14);
    gtk_widget_set_margin_top(vbox, 14);
    gtk_widget_set_margin_bottom(vbox, 14);
    gtk_window_set_child(GTK_WINDOW(win), vbox);

    GtkWidget *lbl = gtk_label_new(qirtas_tr("Choose an export theme"));
    gtk_widget_add_css_class(lbl, "pop-section-label");
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(vbox), lbl);

    char *last = qirtas_pref_get_string("export_theme");

    for (int i = 0; i < N_THEMES; i++) {
        const PrintTheme *t = &THEMES[i];
        GtkWidget *card = gtk_button_new();
        gtk_widget_add_css_class(card, "pop-btn");
        GtkWidget *cv = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

        GtkWidget *name = gtk_label_new(NULL);
        char *markup = g_markup_printf_escaped(
            "<span font_family='%s' weight='bold' size='large' foreground='%s'>%s</span>",
            t->heading_font, t->accent, t->name);
        gtk_label_set_markup(GTK_LABEL(name), markup);
        g_free(markup);
        gtk_widget_set_halign(name, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(cv), name);

        GtkWidget *sample = gtk_label_new(NULL);
        markup = g_markup_printf_escaped(
            "<span font_family='%s' size='small' foreground='%s'>%s</span>",
            t->body_font, t->ink, t->sample);
        gtk_label_set_markup(GTK_LABEL(sample), markup);
        g_free(markup);
        gtk_widget_set_halign(sample, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(cv), sample);

        gtk_button_set_child(GTK_BUTTON(card), cv);
        g_object_set_data(G_OBJECT(card), "theme-idx", GINT_TO_POINTER(i));
        g_signal_connect(card, "clicked", G_CALLBACK(on_theme_card_clicked), gui);
        if (last && strcmp(last, t->id) == 0)
            gtk_widget_add_css_class(card, "active");
        gtk_box_append(GTK_BOX(vbox), card);
    }
    g_free(last);

    GtkWidget *plain = gtk_button_new_with_label(qirtas_tr("Editor look (syntax highlighted)"));
    gtk_widget_add_css_class(plain, "pop-btn");
    g_object_set_data(G_OBJECT(plain), "theme-idx", GINT_TO_POINTER(-1));
    g_signal_connect(plain, "clicked", G_CALLBACK(on_theme_card_clicked), gui);
    gtk_box_append(GTK_BOX(vbox), plain);

    gtk_window_present(GTK_WINDOW(win));
}
