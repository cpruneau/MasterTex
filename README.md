# MasterTex

A LaTeX editor for large multi-file projects (Qt 6 / C++).

## Build on macOS

You need Qt 6 (6.4 or newer), CMake, and the Xcode command-line tools (`xcode-select --install`).

```sh
cd ~/Developer/MasterTex

# If Qt came from Homebrew (brew install qt):
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$(brew --prefix qt)"

# If Qt came from the Qt online installer, point at its macos folder instead, e.g.:
# cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=~/Qt/6.8.0/macos

cmake --build build -j8
open build/MasterTex.app
```

To open your book straight away:

```sh
open build/MasterTex.app --args /path/to/your/main.tex
```

To check how MasterTex reads a project without opening the GUI:

```sh
build/MasterTex.app/Contents/MacOS/MasterTex --dump /path/to/your/main.tex
```

## What milestone 1 does

- **Projects**: File > Open Project… and choose the master `.tex`. MasterTex follows
  `\input`, `\include`, `\subfile`, `\import`/`\subimport`, reading live editor buffers,
  so the model includes unsaved edits.
- **Structure browser** (left), with three tabs:
  - *Outline*: parts, chapters and sections across all files, with figures and tables
    (caption, label, image files) nested under their section. Missing images are red.
  - *Files*: the include tree plus the bibliography files.
  - *References*: every label with its definition and all uses, undefined references,
    citations with their uses and `.bib` entries, entries never cited, and images with their uses.
  - A filter box searches all three tabs. Right-click any item to open it in a new window.
- **File browser** (left, top): the project folder. Double-click to open; images open in the default app.
- **Multiple windows**: File > New Window, File > Move Tab to New Window, or right-click
  "Open in New Window". All windows on the same file share one document: edits, undo and
  the unsaved marker stay in sync everywhere.
- **Undocking**: double-click a tab (or right-click > Undock, or View > Undock Editor, Shift+Cmd+U)
  to give that file its own window. Close that window, or use View > Dock Editor Back (Shift+Cmd+J),
  to return it to the tabs. Drag an undocked editor onto an edge of the main window to place it side
  by side with the tabs. Menus, shortcuts and the find bar act on whichever editor you are typing in.
- **PDF pane with SyncTeX**: View (Shift+Cmd+V) shows the PDF inside MasterTex.
  Cmd-click or double-click in the PDF jumps to that line of the source; Cmd-click in the editor
  (or Tools > Show Cursor Position in PDF, Cmd+Opt+P) scrolls the PDF there and highlights it.
  The pane reloads after each compile and keeps your page. Requires the Qt PDF module
  (Qt Maintenance Tool: Additional Libraries > Qt PDF); without it, View opens the system viewer.
- **Restructure** (Structure pane › Restructure…, or Tools › Restructure Document…, Cmd+Opt+T):
  drag chapters and section files in a draft of the book's tree — reorder them, move them to another
  part or chapter, promote or demote them. Commented-out chapters are shown (greyed) and can move too.
  The right-hand side lists every step Apply Changes will take: include lines moved between files,
  chapter folders moved with all their contents (figures, papers, notes), section files given their
  own chapter folder when promoted, headings shifted (\section → \chapter …), every path rewritten in
  every file (commented lines too), figures used only by the moved text moved with it. Labels,
  \ref and \cite need no change. Items keep their compiled/commented-out state.
  Every restructure is journaled in `.mastertex/restructure/` inside the book: if anything fails it
  rolls back, and Tools › Undo Last Restructure restores the book exactly. In a Git repository it also
  commits before and after. Headings written inside a file (marked 🔒) can't be moved yet.
- **Issues** (bottom): missing files and images, duplicate labels, undefined references,
  citations missing from the bibliography, unused labels, and errors from the last build.
  Double-click to jump to the place.
- **Build**: Tools > Run (Cmd+R) runs `latexmk`; Run All (Cmd+Shift+R) forces a full rebuild;
  View PDF opens the result. MasterTex adds `/Library/TeX/texbin` to the search path, because
  apps started from the Dock don't see your shell's PATH (Options > Settings to change it).
- **Menus**: File, Edit, LaTeX, Tools, Math (symbol submenus, matrix), Wizards (figure, table,
  list, matrix, new chapter file, bibliography entry), Bibliography, Macros (placeholder),
  View, Options, Help.
- **Toolbars**: file and build operations, plus LaTeX constructs.
- **Edit**: selection and occurrence commands, line operations (delete, move, duplicate, sort,
  Control-A/E/K/U), text case (lower, upper, title case strict/smart), find & replace bar
  (case, whole word, regex with \1 references), go to line / previous & next change / back-forward
  toggle, bookmarks 1–9 (Cmd+1…9 to go, Cmd+Opt+1…9 to toggle), Unicode characters. macOS adds
  AutoFill, Start Dictation and Emoji & Symbols to the Edit menu itself.
- **Tools**: Build & View, Compile, Stop, View, Bibliography (bibtex or biber, chosen automatically),
  Glossary, Index, single engine runs, Clean, external Terminal, log view and gutter markers,
  Lorem ipsum, Thesaurus (macOS Dictionary), Find Repetitions. Spell checking and user commands are
  placeholders for now.
- **LaTeX / Math**: \documentclass and \usepackage pickers listing what is installed in your TeX
  distribution, preamble commands, sectioning, environments, boxes, fonts, tabular, spacing, accents,
  include files, cross references, \ref to next/previous label; math environments, functions,
  definitions, font styles, stacking symbols, accents and spaces.
- **Preferences** (MasterTex menu, Cmd+,): empty for now; line endings and encoding come next.
- **Symbol panes**: Math > Show Symbol Panes (Greek, operators, relations, arrows, physics).
- **Safety**: saves are atomic (a crash mid-save can't corrupt a file); unsaved buffers are
  autosaved every minute and offered for recovery the next time the file is opened.
  MasterTex does not reopen the previous session on startup.

## Source layout

| File | Role |
|---|---|
| `src/DocumentManager.*` | One shared document per file, atomic save, autosave/recovery |
| `src/Project.*` | Parser and linked project model (outline, includes, labels, refs, cites, images, issues) |
| `src/StructurePanel.*` | Outline / Files / References browser |
| `src/MainWindow.*` | Windows, menus, toolbars, docks, build |
| `src/CodeEditor.*`, `src/LatexHighlighter.*` | Editor with line numbers and LaTeX highlighting |
| `src/Wizards.*`, `src/SymbolPanel.*`, `src/SettingsDialog.*` | Wizards, symbol panes, settings |

## Next milestones (proposed)

1. Restructuring with dependencies: move a section or chapter (with its figures) to another
   file or folder; MasterTex rewrites `\input` lines and image paths and previews the change.
2. Built-in PDF viewer with SyncTeX (click in PDF → source, and back).
3. Find/replace across the project, label and citation renaming everywhere.
4. Macros, more wizards, bibliography manager.
