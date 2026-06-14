# Handoff: Qirtas UI Redesign — Combined Direction

## Overview

This package documents a full UI redesign for the Qirtas GTK4/Zig markdown editor.
The goal is to close the visual quality gap between Qirtas and polished editors like
Obsidian, while keeping the app's identity: the floating paper card on a desk,
RTL-first layout, and the pinned-LTR status bar.

Two themed mockups are included — **dark** (`Qirtas Combined.dc.html`) and
**light** (`Qirtas Light.dc.html`). Open them in a browser to see the full reference.

---

## About the Design Files

The `.dc.html` files in this bundle are **high-fidelity design references built in HTML**.
They are prototypes that show the intended look, layout, and structure — they are NOT
production code to ship directly.

Your task is to **recreate these HTML designs inside the existing Qirtas GTK4/C codebase**,
using its established widget tree, CSS layers, and module structure described in this README.

---

## Fidelity

**High-fidelity.** Colors, typography, spacing, shadows, border radii, and layout
proportions are all intentional and should be matched as closely as GTK4 allows.
Every measurement below is a deliberate design decision.

---

## What Changed (summary)

| Area | Before | After |
|---|---|---|
| Text column | Full card width | Max 680 px, centered, with symmetric side margins |
| Card top edge | Plain border | 2 px navy/gold gradient thread + 46 px header band |
| Card header | None | Breadcrumb path + search icon + outline icon + ⋮ menu |
| Tab strip | Row 2 of bottom bar | Moved to top of window, flat with active underline |
| Sidebar header | None | Brand mark: feather icon + قِرطاس wordmark + المكتبة label |
| Outline panel | Inside sidebar | Closable panel on the desk to the left of the paper |
| Status bar | 2 rows (info + tabs) | 1 slim row (tabs gone, now at top) |
| Line numbers | Shown by default | Hidden by default (preference unchanged) |
| Light theme | Warm sepia-adjacent | Pure-white paper, navy #213A63 from app icon |

---

## Design Tokens

### Dark theme (existing `theme-qirtas-dark.css` — refine these values)

| Token | Value | Usage |
|---|---|---|
| `--color-desk` | `#101218` | Window / desk background |
| `--color-sidebar` | `#14161C` | Sidebar + tab strip + status bar background |
| `--color-sidebar-border` | `#232633` | All sidebar/chrome borders |
| `--color-paper` | `#1D1F27` | Paper card background |
| `--color-paper-border` | `#2A2E3A` | Paper card border |
| `--color-paper-shadow` | `0 1px 2px rgba(0,0,0,.5), 0 12px 36px rgba(0,0,0,.38)` | Paper card shadow |
| `--color-ink` | `#D6D2C6` | Body text |
| `--color-ink-title` | `#ECE8DA` | H1 title |
| `--color-ink-muted` | `#9AA0AC` | Secondary text / inactive tabs |
| `--color-ink-faint` | `#6D7385` | Section labels, placeholders |
| `--color-accent` | `#C9A86B` | Gold: active tab underline, unsaved dot, thread gradient |
| `--color-accent-quote` | `#D9B577` | Highlighted matn / bold quotes in editor |
| `--color-active-row` | `#232734` | Active note row in tree |
| `--color-active-row-text` | `#E3DFD3` | Active note row text |
| `--color-hover-row` | `#1B1E26` | Hovered tree/tab row |
| `--color-search-bg` | `#1B1E26` | Search field background |
| `--color-search-border` | `#262A35` | Search field border |
| `--color-sync-green` | `#79B37A` | Sync/saved status dot |
| `--color-thread` | `linear-gradient(to left, transparent, #C9A86B 30%, #C9A86B 70%, transparent)` | 2 px top edge of paper card |

### Light theme (NEW — add as `theme-qirtas-navy.css`)

| Token | Value | Usage |
|---|---|---|
| `--color-desk` | `#E3DDCE` | Window / desk background |
| `--color-sidebar` | `#EFEBE0` | Sidebar + tab strip + status bar background |
| `--color-sidebar-border` | `#D8D0BF` | All sidebar/chrome borders |
| `--color-paper` | `#FFFFFF` | Paper card background |
| `--color-paper-border` | `#E7E0D0` | Paper card border |
| `--color-paper-shadow` | `0 1px 2px rgba(60,52,33,.08), 0 12px 36px rgba(60,52,33,.12)` | Paper card shadow |
| `--color-ink` | `#33302A` | Body text |
| `--color-ink-title` | `#1F2B40` | H1 title |
| `--color-ink-muted` | `#76705F` | Secondary text / inactive tabs |
| `--color-ink-faint` | `#A59D88` | Section labels, placeholders |
| `--color-accent` | `#213A63` | Navy: active tab underline, unsaved dot, thread gradient |
| `--color-accent-quote` | `#213A63` | Highlighted matn / bold quotes |
| `--color-active-row` | `#DFE6F1` | Active note row in tree (soft navy tint) |
| `--color-active-row-text` | `#213A63` | Active note row text (navy) |
| `--color-hover-row` | `#E6E1D4` | Hovered tree/tab row |
| `--color-search-bg` | `#E6E1D4` | Search field background |
| `--color-search-border` | `#D3CAB7` | Search field border |
| `--color-sync-green` | `#4E9D5B` | Sync/saved status dot |
| `--color-thread` | `linear-gradient(to left, transparent, #213A63 30%, #213A63 70%, transparent)` | 2 px top edge of paper card |

### Typography

| Role | Family | Size | Weight | Line-height |
|---|---|---|---|---|
| UI chrome | IBM Plex Sans Arabic | 13–13.5 px | 400/500/600 | — |
| Section labels | IBM Plex Sans Arabic | 11.5 px | 600 | — |
| Editor body | Noto Naskh Arabic (or user pref) | 19 px | 400 | 2.05 |
| Editor H1 | Noto Naskh Arabic (or user pref) | 36 px | 700 | 1.5 |
| Monospace (counts, badges, shortcuts) | IBM Plex Mono | 10–12.5 px | 400/500 | — |

### Spacing & Radii

| Token | Value |
|---|---|
| Desk gap (paper margins) | 28 px top/right, 24 px bottom, 28 px left (existing `.editor-scroll` margins — keep) |
| Paper border-radius | 12 px (existing — keep) |
| Sidebar width | 276 px |
| Outline panel width | 200 px |
| Tab strip height | 42 px |
| Card header height | 46 px |
| Status bar height | 32 px |
| Tree row padding | 5 px top/bottom, 8 px right |
| Tree row border-radius | 6 px |

---

## Screens / Views

### Main Window

**Layout (top → bottom):**
```
[Tab Strip — 42 px, LTR]
[Sidebar 276 px | Desk]   ← fills remaining height
[Status Bar — 32 px, LTR]
```

The `main_vertical_box` now has three children (top-down):
1. `tab_strip` — moved from bottom_bar to top
2. `sidebar_editor_box` (GtkPaned) — the existing paned
3. `bottom_bar` — slimmed to single row (status_info_row only)

---

### Tab Strip (top, LTR)

**Where:** New position — top of `main_vertical_box`, above `sidebar_editor_box`.
Currently this is Row 2 of `bottom_bar`; move it out.

**Appearance:**
- Background: `--color-sidebar`
- Bottom border: 1 px `--color-sidebar-border`
- Height: 42 px
- Left scroll arrow `◀` + right scroll arrow `▶` flank the scrollable list

**Each tab:**
- Padding: 0 14 px
- Font: 12.5 px `--color-ink-muted`
- **Active tab:** color `--color-active-row-text`, bottom border 2 px `--color-accent`, margin-bottom -1 px (bleeds into the border)
- **Unsaved dot:** 6×6 px circle, color `--color-accent`, beside the filename

**GTK implementation:**
- The existing `tab_bar_scroll` (GtkScrolledWindow) and its children in `src/gui/gui_tabs.c` stay intact.
- Move the entire `tab_strip` GtkBox (the `◀ [scroll] ▶` row) out of `bottom_bar` and prepend it to `main_vertical_box` above `sidebar_editor_box`.
- In `base.css`: change `.tab-bar` height to 42 px, add `border-bottom: 1px solid var(--color-sidebar-border)`, remove the `border-top` it currently has.
- Active tab underline: add a CSS rule on `.tab-item.active` using `border-bottom: 2px solid var(--color-accent)`.

---

### Sidebar (right pane in RTL)

**Where:** `gui->sidebar` — same widget, new header added at the top.

**Brand header (NEW — add above `exp_search_entry`):**
```
[feather SVG 18×18, color --color-accent] [قِرطاس 15 px bold] [المكتبة 11.5 px faint — pushed to start/right]
```
- Container: GtkBox horizontal, padding 16 px all sides, bottom border 1 px `--color-sidebar-border`
- Feather icon: use `qirtas_icon("app")` or inline SVG — the path is the feather quill from the app icon
- **GTK:** prepend a new `GtkBox` (`.sidebar-header`) inside `gui->sidebar` before the search entry.

**Search entry (existing `exp_search_entry`):**
- Background: `--color-search-bg`, border: 1 px `--color-search-border`, border-radius 8 px
- Placeholder: بحث… (Arabic) with a magnifier icon prefix
- No changes to behavior

**Section labels (`الملاحظات`, `المخطط`, etc.):**
- Font: 11.5 px 600 weight, color `--color-ink-faint`
- Padding: 14 px top, 8 px right, 6 px bottom

**Outline panel:**
- **Remove `gui->outline_box` from the sidebar entirely.**
- It moves to the desk (see Outline Panel below).
- The sidebar now only shows: brand header → search → Notes count badge → file tree.

**Active tree row:**
- Background `--color-active-row`, text `--color-active-row-text`
- All other rows: color `--color-ink-muted`, hover `--color-hover-row`

**Tree indent guides:**
- Right border on the indent wrapper: 1 px `--color-sidebar-border`

---

### Paper Card

**Where:** `scrolled` widget (`.editor-scroll`) — keep existing margins and shadow.

**2 px thread (NEW):**
- Add a `GtkBox` with fixed height 2 px as the first child of `editor_page` (or wrap in a GtkOverlay),
  placed above `editor_overlay`.
- CSS: `background: var(--color-thread); border-radius: 12px 12px 0 0;`
- The simplest GTK approach: add a `GtkBox` with `height: 2px` and the gradient background
  as the very first widget inside `.editor-scroll` using `gtk_widget_set_margin_*` zero on all sides.
  **Actually easier:** apply a CSS `border-top: 2px solid` on `.editor-scroll` and use a
  `background-clip` trick, OR just set the first 2 px of `padding-top` of the card to render
  the gradient via a `::before`-style approach. The simplest reliable GTK method is a 2 px tall
  `GtkDrawingArea` or `GtkBox` prepended inside the scrolled window's parent box.

**Card header band (NEW — 46 px, sits above the scrollable area):**

Add a `GtkBox` (horizontal, height 46 px, `.editor-header`) **between** the thread and the
`editor_overlay` (the scrolled window). This requires wrapping both in a vertical GtkBox
inside `editor_page`. Layout left→right in LTR (which means right→left visually in RTL):

```
[breadcrumb path — ink-faint text] ── spacer ── [🔍 search] [≡ outline toggle] [⋮ menu]
```

- **Breadcrumb:** folder/subfolder/filename in muted color, built from the active file path.
  Update it in the same callback that updates the window title.
  Separator: `/` in a fainter color. Last segment: `--color-ink-muted`.
- **Search icon (🔍):** clicking this fires `Ctrl+F` (the existing `search_revealer`) — wire to
  the same handler.
- **Outline toggle (≡):** toggles the closable outline panel (see below). Store state in prefs
  as `outline_panel_visible` (default: true).
- **⋮ menu:** opens the existing `status_menu` (the same ☰ menu from the status bar). You can
  share the same `GMenuModel`/`GtkPopoverMenu`.
- Bottom border: 1 px `--color-paper-border`.
- Padding: 0 18 px.
- Icon size: 14–15 px, color `--color-ink-faint`, hover `--color-accent`.

**Text column constraint (THE KEY CHANGE):**

The `source_view` must render with a maximum effective column of ~680 px, centered inside
the card. GTK does not have a native `max-width` for `GtkSourceView`, so compute side margins:

```c
// In a size-allocate handler on `scrolled` (the paper card):
static void on_paper_size_allocate(GtkWidget *widget, int width, int height, gpointer data) {
    AppGui *gui = data;
    int col_width = 680;
    int margin = MAX(0, (width - col_width) / 2);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(gui->source_view), margin);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(gui->source_view), margin);
}
```

Connect with:
```c
g_signal_connect(scrolled, "size-allocate", G_CALLBACK(on_paper_size_allocate), gui);
```

Also increase top margin so the title has air above it:
```c
gtk_text_view_set_top_margin(GTK_TEXT_VIEW(gui->source_view), 56);
gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(gui->source_view), 80);
```

These replace any existing `left-margin` / `right-margin` / `top-margin` set in CSS or code.
**Do not set these in CSS** — the size-allocate handler wins and must be the single source
of truth for horizontal margins.

---

### Outline Panel (closable marginalia, NEW position)

**Where:** A new panel on the desk, to the left of the paper card in LTR
(which is visually to the left — before/after depends on RTL direction of the desk box).

**GTK structure:**
- The desk area (currently just `editor_page` expanding to fill) becomes a `GtkBox` horizontal:
  ```
  [editor_page — flex:1] | [outline_panel — 200 px, shown/hidden via GtkRevealer]
  ```
- `outline_panel` is a `GtkRevealer` with `transition-type: GTK_REVEALER_TRANSITION_TYPE_SLIDE_LEFT`
  wrapping the existing `gui->outline_box` content.
- The close button (`×`) is a `GtkButton` at the top of the panel header row, wired to hide the
  revealer and update the `outline_panel_visible` preference.
- The outline toggle icon in the card header (see above) wired to show it again.

**Appearance:**
- Background: transparent (shows desk color through)
- Padding: 118 px top (aligns roughly with the card's text start, below the card header)
- Panel label: `المخطط` 11.5 px 600, color `--color-ink-faint`
- Close button: `×` 13 px, color `--color-ink-faint`, hover: background `--color-hover-row`
- Active heading: color `--color-accent`, 5×5 px dot marker
- Other headings: color `--color-ink-muted`, padded right 21 px (indent)

**Move the existing `gui->outline_box` here** — no changes to its content/behavior.
`gui_outline_refresh` continues to work as-is.

---

### Status Bar (slimmed to 1 row)

**Where:** `bottom_bar` in `src/gui.c` — remove Row 2 (tab strip), keep Row 1 only.

**Height:** 32 px (was taller with 2 rows).

**Left cluster (LTR):**
`[sidebar toggle icon]` → `[sync dot ●]` → `[N كلمة]` → `[N حرف]` → `[path — faint]`

**Right cluster:**
`[🔍 search]` → `[☰ menu]`

- Remove the `[sidebar toggle icon]` from left if you prefer (the card header ≡ now handles outline).
  Keep it for the sidebar itself (show/hide the whole sidebar pane).
- Font: 12 px, color `--color-ink-faint`
- Background: `--color-sidebar`, top border: 1 px `--color-sidebar-border`

---

### Line Numbers

**Default: off.** Change the default in `apply_editor_prefs` in `src/gui.c`:
```c
// Change default from TRUE to FALSE:
int show_line_numbers = qirtas_pref_get_int(gui, "show_line_numbers", 0); // was 1
```

The preference key and UI toggle are unchanged — users can still enable them.

---

## Light Theme Implementation

**File to create:** `src/ui/themes/theme-qirtas-navy.css`

Copy `theme-qirtas-light.css` as the base and replace every color token with the
values in the Design Tokens table above (the Light theme section).

**Steps:**
1. Create `src/ui/themes/theme-qirtas-navy.css` with the navy token values.
2. In `apply_theme()` in `src/gui.c`, add a branch for `"navy"`.
3. Add `"Paper & Ink Navy"` to the theme dropdown in the Settings window.
4. Create a matching GtkSourceView style scheme `qirtas-navy.style-scheme.xml` by copying
   `qirtas.style-scheme.xml` and replacing the highlight color with `#213A63`.

**Key color differences from the existing light theme:**
- The accent goes from gold `#C9A86B` to navy `#213A63`
- The desk background: `#E3DDCE` (warm, not pure grey)
- Active row tint: `#DFE6F1` (soft navy, not gold-adjacent)
- Paper: pure `#FFFFFF`

---

## Interactions & Behavior

| Interaction | Behavior |
|---|---|
| Click outline heading | Existing jump behavior — unchanged |
| Click `×` on outline panel | Hides `GtkRevealer`, saves `outline_panel_visible = 0` to prefs |
| Click `≡` in card header | Shows `GtkRevealer`, saves `outline_panel_visible = 1` to prefs |
| Click `🔍` in card header | Same as `Ctrl+F` — triggers `search_revealer` |
| Click `⋮` in card header | Opens the existing `status_menu` popover |
| Resize window | `size-allocate` callback recomputes `source_view` left/right margins live |
| Switch theme to Navy | `apply_theme("navy")` — same flow as existing theme switching |
| Tab active underline | CSS on `.tab-item.active`: `border-bottom: 2px solid var(--color-accent)` |
| Tab unsaved dot | Existing `dirty_dot` widget — change color to `--color-accent` in CSS |

---

## State Management

- `outline_panel_visible` — new int pref (default 1), read/written with `qirtas_pref_get_int` /
  `qirtas_pref_set_int`. Restored on startup in `apply_editor_prefs`.
- `show_line_numbers` default changed from 1 → 0. No schema change needed.
- Tab strip position: no pref needed — it's always at top now (the Preferences → Status Bar
  Position pref can stay as-is; it controlled the whole `bottom_bar`; that pref now only
  affects the slim status row).

---

## Files to Edit

| File | What to change |
|---|---|
| `src/gui.c` | Move tab_strip to top of main_vertical_box; add sidebar brand header; add card header band; wire outline toggle; add size-allocate handler for column constraint; change line_numbers default to 0 |
| `src/ui/themes/base.css` | Tab strip border direction (top→bottom); `.editor-header` styles; `.sidebar-header` styles; status bar height 32 px; outline panel styles |
| `src/ui/themes/theme-qirtas-dark.css` | Add `--color-thread` token (gold gradient) |
| `src/ui/themes/theme-qirtas-navy.css` | **NEW FILE** — navy light theme tokens |
| `src/ui/qirtas-navy.style-scheme.xml` | **NEW FILE** — navy editor highlight scheme |
| `src/gui/gui_outline.c` | No behavior changes; `outline_box` just gets a new parent |
| `src/gui/gui_tabs.c` | No changes — widget moves, not rewired |

---

## Assets

| Asset | Source |
|---|---|
| App feather icon (sidebar header) | `assets/` or the existing icon set via `qirtas_icon()` — use the existing SVG path |
| All other icons | Existing `qirtas_icon()` table — no new icons needed |

---

## Design Reference Files

| File | Contents |
|---|---|
| `Qirtas Combined.dc.html` | Dark theme combined mockup (open in browser) |
| `Qirtas Light.dc.html` | Navy light theme mockup (open in browser) |

---

## Notes for Claude Code

- The `source_view` is the **direct child** of `scrolled` — do NOT wrap it in any container.
  The column constraint is achieved purely via `gtk_text_view_set_left/right_margin()` at runtime.
- The thread gradient on the card top: the easiest GTK4 method is a 2 px `GtkBox` with a
  CSS `background-image: linear-gradient(...)` placed above the editor area in a parent GtkBox,
  NOT on the scrolled window itself (which would scroll with content).
- The card header band sits OUTSIDE the scrolled window — it does not scroll. Place it as a
  non-scrolling sibling above `editor_overlay` inside `editor_page`.
- All new prefs use `qirtas_pref_get_int` / `qirtas_pref_set_int` (defined in `gui_cursor.c`,
  declared in `gui_internal.h`) — do NOT touch the Zig-side `session_state` schema.
- The Arabic RTL direction: `gtk_widget_set_default_direction(GTK_TEXT_DIR_RTL)` already handles
  mirroring. The sidebar ends up on the right, the outline panel ends up on the left of the paper
  — this is correct and requires no extra RTL handling.
- Wrap any new UI strings in `qirtas_tr()` for Arabic/English support.
