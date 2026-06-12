# Qirtas — UI Layout Map

What is where on screen, what widget owns it, and where it's built in code.
All build code lives in `activate()` in `src/gui.c` unless noted.

## The big picture

```
┌─ AdwApplicationWindow (undecorated, 1180×760 default) ──────────────────┐
│ main_vertical_box  (GtkBox, vertical)                                   │
│ ┌─ sidebar_editor_box (GtkPaned, horizontal) ──────────────────────────┐│
│ │ ┌─ sidebar ────────┐ ┌─ stack (GtkStack, css .workspace) ──────────┐ ││
│ │ │ (GtkBox, vert)   │ │                                             │ ││
│ │ │                  │ │  page "editor" → editor_page (GtkBox, vert) │ ││
│ │ │ workspace search │ │  ┌─ editor_overlay (GtkOverlay) ──────────┐ │ ││
│ │ │ (GtkSearchEntry) │ │  │ child: scrolled (GtkScrolledWindow,    │ │ ││
│ │ │        ↓         │ │  │   css .editor-scroll = the PAPER CARD, │ │ ││
│ │ │ OUTLINE section  │ │  │   margins 28/28/24/20 = the desk gap)  │ │ ││
│ │ │ heading TOC,     │ │  │   └─ source_view (GtkSourceView,      │ │ ││
│ │ │ hidden if no     │ │  │      DIRECT scrollable child — never  │ │ ││
│ │ │ headings,        │ │  │      wrap in a box, see As-Built §3)  │ │ ││
│ │ │ gui->outline_box │ │  │ overlay 1: cursor_trail_area          │ │ ││
│ │ │        ↓         │ │  │   (GtkDrawingArea, transparent,       │ │ ││
│ │ │ "Notes" header + │ │  │    click-through, ink smear)          │ │ ││
│ │ │ file tree        │ │  │ overlay 2: source_map (GtkSourceMap,  │ │ ││
│ │ │ (exp_scroll →    │ │  │    right edge, hidden unless pref on) │ │ ││
│ │ │  tree_box =      │ │  └────────────────────────────────────────┘ │ ││
│ │ │  explorer_       │ │  search_revealer (GtkRevealer, slides up    │ ││
│ │ │  listbox)        │ │   from bottom of editor page):              │ ││
│ │ │        ↓         │ │   row 1: search entry + count + prev/next   │ ││
│ │ │ nav_bot (empty — │ │   row 2: replace entry + Replace + All      │ ││
│ │ │ settings moved   │ │                                             │ ││
│ │ │ to status menu)  │ │  page "stats", page "sync", … (other pages) │ ││
│ │ └──────────────────┘ └─────────────────────────────────────────────┘ ││
│ └───────────────────────────────────────────────────────────────────────┘│
│ ┌─ bottom_bar (GtkBox vertical, css .bottom-bar, ALWAYS LTR) ───────────┐│
│ │ status_info_row (GtkBox horizontal):                                  ││
│ │  [sidebar toggle][sync ●][N words][N chars][path]──spacer──[🔍][☰]    ││
│ │ tab_strip: [◀][ tab tab tab … (tab_bar_scroll) ][▶]                   ││
│ └───────────────────────────────────────────────────────────────────────┘│
└───────────────────────────────────────────────────────────────────────────┘
```

## Region by region

### Sidebar (left pane of the GtkPaned, ~260 px, `gui->sidebar`)
Top to bottom:
1. **Workspace search** (`exp_search_entry`) — FTS search across all notes,
   Arabic-normalized. Results replace the tree below while typing.
2. **OUTLINE** (`gui->outline_box`, built in `gui.c`, refreshed by
   `gui_outline_refresh` in `src/gui/gui_outline.c`) — heading TOC of the
   open note, indented by level, max height 180 px, hidden when the note has
   no headings. Click → jump. Refreshed by the same 220 ms debounce as the
   word count and on every full buffer reload.
3. **Notes** header + count badge, then the **file tree**
   (`explorer_listbox`, built by `populate_explorer`/`tree_build_dir` in
   `gui.c`). Rows: left-click opens, right-click → Open / Open with File
   Manager (capture-phase gesture).
4. `nav_bot` — currently empty (settings moved to the status menu).

Sidebar can sit on the right instead (Preferences → Sidebar Side), and
toggles with Ctrl+\\ or the status-bar button.

### Editor (the paper)
- `scrolled` carries css class `.editor-scroll` — the floating paper card:
  border, 10 px radius, layered shadow. Its 28/28/24/20 margins against the
  window are the "desk" gap. **This is the identity of the app — keep it.**
  Compact Layout shrinks margins to 12/12/10/8; Focus Mode removes them.
- `source_view` is the *direct* child of the scrolled window (lazy layout —
  never re-wrap it in a box).
- `cursor_trail_area` overlays the whole card, transparent and
  click-through; the tick callback draws the ink-smear caret trail.
- `source_map` (overview minimap) floats on the right edge of the card,
  visible only when the preference is on.
- Right-click in the editor → formatting popover (FORMAT column: bold,
  italic… / PARAGRAPH column: H1–H6, lists, Horizontal Rule), built in
  `src/gui/gui_popover.c`.

### Search bar (in-file)
`search_revealer` slides up at the bottom of the editor page (Ctrl+F /
Ctrl+H or status-bar 🔍). Two rows: find (entry, match count, prev/next,
close) and replace (entry, Replace, All). Built in `gui.c`, logic in
`src/gui/gui_search.c`.

### Bottom status bar (`bottom_bar`, two rows, pinned LTR even in Arabic)
- **Row 1, left cluster:** sidebar toggle → sync status dot (●, colored by
  save/sync state, click = Sync Now) → word count → char count → file path.
  Counts use Eastern Arabic numerals when the UI language is Arabic.
- **Row 1, right cluster:** in-file search button (🔍) → menu button (☰).
  The ☰ menu (`status_menu_item` rows): New / Open / Save / Export PDF /
  Copy File / Save As… ─ Find-Replace / Fullscreen ─ Preferences / Keyboard
  Shortcuts ─ Quit.
- **Row 2:** the tab strip — scrollable tab pills with dirty dots and close
  buttons (`src/gui/gui_tabs.c`), flanked by ◀ ▶ scroll buttons.
- The whole bar can be moved to the top (Preferences → Status Bar Position).

### Transient windows (not in the main tree)
| Window | Opens from | Built in |
|---|---|---|
| Preferences ("settings sheet", 500×620) | ☰ menu / Ctrl+, | `gui.c` (pop_box…settings_win) |
| Quick Switcher (480×380, fuzzy file open) | Ctrl+P | `src/gui/gui_switcher.c` |
| Keyboard Shortcuts reference | ☰ menu / Ctrl+? | `src/gui/gui_shortcuts.c` |
| GitHub device-code dialog | Sync card Connect | `src/gui/gui_sync.c` |
| Vault recovery modal | backend request | `gui.c` |

### RTL behavior (Arabic UI language)
`gtk_widget_set_default_direction(RTL)` mirrors everything — sidebar ends up
on the right, paned flips, alignment flips — EXCEPT the bottom status bar,
which is explicitly pinned LTR (`gtk_widget_set_direction` on `bottom_bar`).
Inside the editor, paragraph direction is per-paragraph by first strong
character with markdown prefixes skipped (`detect_rtl`, `gui_conceal.c`).

### CSS layers (who styles what)
1. `src/ui/themes/theme-<name>.css` — color tokens only (`:root` vars).
2. `src/ui/themes/base.css` — all layout/spacing/widget styling, including
   `.editor-scroll` (paper card), `.sidebar`, `.bottom-bar`, `.tree-row`,
   `.tab-item`, `.compact-ui` overrides.
3. Font provider (`update_editor_font`, APPLICATION+1 priority) — editor
   font family/size and the caret color (wins over theme provider —
   custom caret color must be emitted here).
