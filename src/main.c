/*
 * Pen (Plaintext Editing Notepad)
 * Copyright (C) 2026 Uel McNeill
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see the file COPYING.
 */

#include "raylib.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>

#include "tinyfiledialogs.h"

typedef struct {
    char *data;
    int len;
    int cap;
    int cursor;
} Buffer;

static int clampi(int v, int lo, int hi) { if (v < lo) return lo; if (v > hi) return hi; return v; }
static int mini(int a, int b) { return a < b ? a : b; }
static int maxi(int a, int b) { return a > b ? a : b; }

static void buf_init(Buffer *b) {
    b->cap = 1024;
    b->data = (char*)malloc((size_t)b->cap);
    b->len = 0;
    b->cursor = 0;
    if (b->data) b->data[0] = '\0';
}
static void buf_free(Buffer *b) { free(b->data); b->data = NULL; b->len = b->cap = b->cursor = 0; }

static void buf_ensure(Buffer *b, int needed) {
    if (needed <= b->cap) return;
    int newcap = b->cap;
    while (newcap < needed) newcap *= 2;
    char *p = (char*)realloc(b->data, (size_t)newcap);
    if (!p) return;
    b->data = p;
    b->cap = newcap;
}

static void buf_insert_bytes(Buffer *b, const char *s, int n) {
    if (n <= 0) return;
    buf_ensure(b, b->len + n + 1);
    if (!b->data) return;

    memmove(b->data + b->cursor + n, b->data + b->cursor, (size_t)(b->len - b->cursor));
    memcpy(b->data + b->cursor, s, (size_t)n);
    b->len += n;
    b->cursor += n;
    b->data[b->len] = '\0';
}
static void buf_insert_byte(Buffer *b, char c) { buf_insert_bytes(b, &c, 1); }

static void buf_delete_range(Buffer *b, int a, int z) {
    a = clampi(a, 0, b->len);
    z = clampi(z, 0, b->len);
    if (z <= a) return;

    memmove(b->data + a, b->data + z, (size_t)(b->len - z));
    b->len -= (z - a);
    b->data[b->len] = '\0';

    if (b->cursor > z) b->cursor -= (z - a);
    else if (b->cursor > a) b->cursor = a;
}

static void buf_backspace(Buffer *b) {
    if (b->cursor <= 0) return;
    buf_delete_range(b, b->cursor - 1, b->cursor);
}

static void cursor_row_col(const Buffer *b, int *outRow, int *outCol) {
    int row = 0, col = 0;
    for (int i = 0; i < b->cursor && i < b->len; i++) {
        if (b->data[i] == '\n') { row++; col = 0; }
        else { col++; }
    }
    *outRow = row;
    *outCol = col;
}

static int line_start_index(const Buffer *b, int targetRow) {
    int row = 0;
    if (targetRow <= 0) return 0;
    for (int i = 0; i < b->len; i++) {
        if (b->data[i] == '\n') {
            row++;
            if (row == targetRow) return i + 1;
        }
    }
    return b->len;
}

static int line_end_index(const Buffer *b, int start) {
    for (int i = start; i < b->len; i++) if (b->data[i] == '\n') return i;
    return b->len;
}

static int total_rows(const Buffer *b) {
    int rows = 1;
    for (int i = 0; i < b->len; i++) if (b->data[i] == '\n') rows++;
    return rows;
}

static int line_length_at_row(const Buffer *b, int row) {
    int s = line_start_index(b, row);
    int e = line_end_index(b, s);
    return e - s;
}

static int index_at_row_col(const Buffer *b, int row, int col) {
    int s = line_start_index(b, row);
    int len = line_length_at_row(b, row);
    int c = clampi(col, 0, len);
    return s + c;
}

static void move_home(Buffer *b) {
    int row, col;
    cursor_row_col(b, &row, &col);
    b->cursor = line_start_index(b, row);
}
static void move_end(Buffer *b) {
    int row, col;
    cursor_row_col(b, &row, &col);
    int s = line_start_index(b, row);
    b->cursor = line_end_index(b, s);
}

typedef struct {
    bool active;
    int anchor;
    int caret;
} Selection;

static bool sel_has(const Selection *s) { return s->active && s->anchor != s->caret; }
static int  sel_a(const Selection *s) { return mini(s->anchor, s->caret); }
static int  sel_z(const Selection *s) { return maxi(s->anchor, s->caret); }
static void sel_set_single(Selection *s, int idx) { s->active = false; s->anchor = s->caret = idx; }

static int index_from_mouse(const Buffer *b, Rectangle textArea, int scrollRow, float lineH, float charW, Vector2 mouse) {
    int relRow = (int)((mouse.y - textArea.y) / lineH);
    if (relRow < 0) relRow = 0;

    int row = scrollRow + relRow;
    int maxRow = total_rows(b) - 1;
    row = clampi(row, 0, maxRow);

    float relX = mouse.x - textArea.x;
    int col = (int)((relX + (charW * 0.5f)) / charW);
    if (col < 0) col = 0;

    return index_at_row_col(b, row, col);
}

static bool save_to_path(const char *path, const Buffer *buf) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    size_t wrote = fwrite(buf->data, 1, (size_t)buf->len, f);
    fclose(f);
    return wrote == (size_t)buf->len;
}

static bool load_from_path(const char *path, Buffer *buf, Selection *sel, int *scrollRow) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long size = ftell(f);
    if (size < 0) { fclose(f); return false; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return false; }

    buf_ensure(buf, (int)size + 1);
    if (!buf->data) { fclose(f); return false; }

    size_t got = fread(buf->data, 1, (size_t)size, f);
    fclose(f);

    buf->len = (int)got;
    buf->data[buf->len] = '\0';
    buf->cursor = buf->len;
    sel_set_single(sel, buf->cursor);
    if (scrollRow) *scrollRow = 0;
    return true;
}

static int wrap_fit_count(Font font, float fontSize, float maxWidth, const char *s, int n) {
    if (n <= 0) return 0;
    int lastSpace = -1;

    char tmp[4096];
    int limit = (int)sizeof(tmp) - 1;
    int takeMax = (n < limit) ? n : limit;

    for (int i = 0; i < takeMax; i++) {
        tmp[i] = s[i];
        tmp[i+1] = '\0';
        if (s[i] == ' ') lastSpace = i;

        float w = MeasureTextEx(font, tmp, fontSize, 0).x;
        if (w > maxWidth) {
            if (lastSpace >= 0) return lastSpace + 1;
            return (i > 0) ? i : 1;
        }
    }
    return takeMax;
}

static const char *base_name(const char *p) {
    if (!p || !p[0]) return p;
    const char *a = strrchr(p, '/');
    const char *b = strrchr(p, '\\');
    const char *m = a;
    if (b && (!m || b > m)) m = b;
    return m ? (m + 1) : p;
}

static void draw_text(Font f, const char *s, float x, float y, float size, Color c) {
    DrawTextEx(f, s, (Vector2){x,y}, size, 0, c);
}

static bool ui_button(Rectangle r, const char *label, Font font, float fontSize,
                      Color bg, Color hover, Color pressed, Color fg) {
    Vector2 m = GetMousePosition();
    bool hot = CheckCollisionPointRec(m, r);
    bool down = hot && IsMouseButtonDown(MOUSE_LEFT_BUTTON);

    Color c = bg;
    if (hot) c = hover;
    if (down) c = pressed;

    DrawRectangleRounded(r, 0.25f, 10, c);
    DrawRectangleRoundedLines(r, 0.25f, 10, (Color){35,42,54,255});

    Vector2 t = MeasureTextEx(font, label, fontSize, 0);
    DrawTextEx(font, label,
               (Vector2){ r.x + (r.width - t.x)/2.0f, r.y + (r.height - t.y)/2.0f },
               fontSize, 0, fg);

    return hot && IsMouseButtonReleased(MOUSE_LEFT_BUTTON);
}

static bool menu_item_lr(Rectangle r, const char *left, const char *right,
                         Font font, float fontSize, Color fg) {
    Vector2 m = GetMousePosition();
    bool hot = CheckCollisionPointRec(m, r);
    Color bg = hot ? (Color){33,39,49,255} : (Color){28,33,41,255};
    DrawRectangleRec(r, bg);

    draw_text(font, left, r.x + 10, r.y + (r.height - fontSize)/2.0f - 1, fontSize, fg);

    if (right && right[0]) {
        Vector2 tw = MeasureTextEx(font, right, fontSize, 0);
        draw_text(font, right, r.x + r.width - 10 - tw.x, r.y + (r.height - fontSize)/2.0f - 1, fontSize, fg);
    }
    return hot && IsMouseButtonReleased(MOUSE_LEFT_BUTTON);
}

typedef enum { MENU_NONE, MENU_FILE, MENU_EDIT } Menu;

static void restore_cursor_now(void) {
    EnableCursor();
    ShowCursor();
    SetMouseCursor(MOUSE_CURSOR_DEFAULT);
}

// --- Toast helper ---
typedef struct {
    char msg[128];
    double until;
} Toast;

static void toast_set(Toast *t, const char *msg, double seconds) {
    strncpy(t->msg, msg, sizeof(t->msg) - 1);
    t->msg[sizeof(t->msg) - 1] = '\0';
    t->until = GetTime() + seconds;
}

static bool do_open(Buffer *buf, Selection *sel, int *scrollRow, char *pathOut, int pathOutSz, bool *hasPath) {
    const char *path = tinyfd_openFileDialog("Open text file", "", 0, NULL, NULL, 0);
    restore_cursor_now();
    if (!path || !path[0]) return false;

    bool ok = load_from_path(path, buf, sel, scrollRow);
    if (ok) {
        strncpy(pathOut, path, (size_t)pathOutSz - 1);
        pathOut[pathOutSz - 1] = '\0';
        *hasPath = true;
    }
    return ok;
}

static bool do_save_as(const Buffer *buf, char *pathOut, int pathOutSz, bool *hasPath) {
    const char *suggest = (*hasPath && pathOut[0]) ? pathOut : "untitled.txt";
    const char *path = tinyfd_saveFileDialog("Save As", suggest, 0, NULL, NULL);
    restore_cursor_now();
    if (!path || !path[0]) return false;

    bool ok = save_to_path(path, buf);
    if (ok) {
        strncpy(pathOut, path, (size_t)pathOutSz - 1);
        pathOut[pathOutSz - 1] = '\0';
        *hasPath = true;
    }
    return ok;
}

static bool do_save(const Buffer *buf, char *pathOut, int pathOutSz, bool *hasPath) {
    if (*hasPath && pathOut[0]) return save_to_path(pathOut, buf);
    return do_save_as(buf, pathOut, pathOutSz, hasPath);
}

static const char* find_asset(const char *rel) {
    static char path[1024];

    const char *appdir = getenv("APPDIR");
    if (appdir && appdir[0]) {
        snprintf(path, sizeof(path), "%s/usr/share/pen/assets/%s", appdir, rel);
        if (FileExists(path)) return path;
    }

    snprintf(path, sizeof(path), "/usr/share/pen/assets/%s", rel);
    if (FileExists(path)) return path;

    snprintf(path, sizeof(path), "/usr/local/share/pen/assets/%s", rel);
    if (FileExists(path)) return path;

    snprintf(path, sizeof(path), "assets/%s", rel);
    if (FileExists(path)) return path;

    snprintf(path, sizeof(path), "../assets/%s", rel);
    if (FileExists(path)) return path;

    snprintf(path, sizeof(path), "assets/%s", rel);
    return path;
}


int main(void) {
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    InitWindow(1200, 640, "Pen");
    SetTargetFPS(60);

    Buffer buf; buf_init(&buf);
    Selection sel; sel_set_single(&sel, 0);

    int textPx = 22;
    Font editorFont = LoadFontEx(find_asset("fonts/JetBrainsMonoNL-Regular.ttf"), textPx, NULL, 0);
    if (editorFont.texture.id == 0) editorFont = GetFontDefault();

    float uiSize = 16.0f;
    Font uiFont     = LoadFontEx(find_asset("fonts/Inter-Regular.ttf"), (int)uiSize, NULL, 0);
    if (uiFont.texture.id == 0) uiFont = editorFont;

    const float fontSize = (float)textPx;
    const float lineGap  = 8.0f;
    const float lineH    = fontSize + lineGap;

    float charW = MeasureTextEx(editorFont, "M", fontSize, 0).x;
    if (charW < 1.0f) charW = 12.0f;

    int scrollRow = 0;
    int desiredCol = 0;
    bool dragging = false;

    char currentPath[512] = "";
    bool hasPath = false;

    // Dirty + Toast
    bool dirty = false;
    Toast toast = { .msg = "", .until = 0 };

    // Backspace repeat
    double bsNext = 0.0;
    bool bsHeldPrev = false;
    const double BS_INITIAL_DELAY = 0.32;
    const double BS_REPEAT_RATE   = 0.045;

    Menu menu = MENU_NONE;
    bool quitRequested = false;

    bool wasFocused = IsWindowFocused();

    while (!WindowShouldClose() && !quitRequested) {
        bool focused = IsWindowFocused();
        if (focused && !wasFocused) restore_cursor_now();
        wasFocused = focused;

        if (IsKeyPressed(KEY_ESCAPE)) quitRequested = true;

        int w = GetScreenWidth();
        int h = GetScreenHeight();

        Color bg     = (Color){ 14, 17, 22, 255 };
        Color panel  = (Color){ 20, 24, 31, 255 };
        Color text   = (Color){ 230, 233, 240, 255 };
        Color muted  = (Color){ 150, 160, 175, 255 };
        Color accent = (Color){ 96, 165, 250, 255 };
        Color border = (Color){ 35, 42, 54, 255 };
        Color selBg  = (Color){ 96, 165, 250, 80 };

        const int topBarH = 44;

        int cardX = 40;
        int cardY = 70;
        int cardW = w - 80;
        int cardH = h - cardY - 60;
        if (cardH < 120) cardH = 120;

        int pad = 22;
        Rectangle textArea = { (float)cardX + pad, (float)cardY + pad, (float)cardW - pad*2, (float)cardH - pad*2 };

        int visibleRows = (int)(textArea.height / lineH);
        if (visibleRows < 1) visibleRows = 1;

        int rows = total_rows(&buf);
        int maxScroll = rows - visibleRows;
        if (maxScroll < 0) maxScroll = 0;

        bool ctrl  = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
        bool shiftKey = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);

        bool cursorOn = ((int)(GetTime() * 2.0) % 2) == 0;

        Vector2 mouse = GetMousePosition();
        bool mouseInText = CheckCollisionPointRec(mouse, textArea);

        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON) && mouseInText) {
            dragging = true;
            int idx = index_from_mouse(&buf, textArea, scrollRow, lineH, charW, mouse);

            if (!shiftKey) { buf.cursor = idx; sel_set_single(&sel, idx); }
            else {
                if (!sel.active) { sel.active = true; sel.anchor = buf.cursor; sel.caret = buf.cursor; }
                buf.cursor = idx; sel.caret = buf.cursor;
            }
            menu = MENU_NONE;
        }
        if (dragging && IsMouseButtonDown(MOUSE_LEFT_BUTTON) && mouseInText) {
            int idx = index_from_mouse(&buf, textArea, scrollRow, lineH, charW, mouse);
            if (!sel.active) { sel.active = true; sel.anchor = buf.cursor; sel.caret = buf.cursor; }
            buf.cursor = idx; sel.caret = buf.cursor;
        }
        if (IsMouseButtonReleased(MOUSE_LEFT_BUTTON)) dragging = false;

        float wheel = GetMouseWheelMove();
        if (wheel != 0.0f) {
            scrollRow -= (int)wheel;
            scrollRow = clampi(scrollRow, 0, maxScroll);
        }

        // --- File shortcuts (and dirty/toast) ---
        if (ctrl && IsKeyPressed(KEY_O)) {
            if (do_open(&buf, &sel, &scrollRow, currentPath, (int)sizeof(currentPath), &hasPath)) {
                dirty = false;
                toast_set(&toast, "Opened", 1.0);
            }
        }

        if (ctrl && IsKeyPressed(KEY_S) && !shiftKey) {
            if (do_save(&buf, currentPath, (int)sizeof(currentPath), &hasPath)) {
                dirty = false;
                toast_set(&toast, "Saved", 1.2);
            }
        }

        if (ctrl && IsKeyPressed(KEY_S) && shiftKey) {
            if (do_save_as(&buf, currentPath, (int)sizeof(currentPath), &hasPath)) {
                dirty = false;
                toast_set(&toast, "Saved As", 1.2);
            }
        }

        if (ctrl && IsKeyPressed(KEY_Q)) quitRequested = true;

        // Edit shortcuts
        if (ctrl && IsKeyPressed(KEY_A)) { sel.active = true; sel.anchor = 0; sel.caret = buf.len; buf.cursor = buf.len; }
        if (ctrl && IsKeyPressed(KEY_C) && sel_has(&sel)) {
            int a = sel_a(&sel), z = sel_z(&sel), n = z - a;
            char *tmp = (char*)malloc((size_t)n + 1);
            if (tmp) { memcpy(tmp, buf.data + a, (size_t)n); tmp[n] = '\0'; SetClipboardText(tmp); free(tmp); }
        }
        if (ctrl && IsKeyPressed(KEY_X) && sel_has(&sel)) {
            int a = sel_a(&sel), z = sel_z(&sel), n = z - a;
            char *tmp = (char*)malloc((size_t)n + 1);
            if (tmp) { memcpy(tmp, buf.data + a, (size_t)n); tmp[n] = '\0'; SetClipboardText(tmp); free(tmp); }
            buf_delete_range(&buf, a, z);
            sel_set_single(&sel, buf.cursor);
            dirty = true;
        }
        if (ctrl && IsKeyPressed(KEY_V)) {
            const char *clip = GetClipboardText();
            if (clip && clip[0]) {
                if (sel_has(&sel)) { buf_delete_range(&buf, sel_a(&sel), sel_z(&sel)); sel_set_single(&sel, buf.cursor); }
                buf_insert_bytes(&buf, clip, (int)strlen(clip));
                sel_set_single(&sel, buf.cursor);
                dirty = true;
            }
        }

        // Enter
        if (IsKeyPressed(KEY_ENTER)) {
            if (sel_has(&sel)) { buf_delete_range(&buf, sel_a(&sel), sel_z(&sel)); sel_set_single(&sel, buf.cursor); }
            buf_insert_byte(&buf, '\n');
            sel_set_single(&sel, buf.cursor);
            dirty = true;
        }

        // Backspace repeat
        double now = GetTime();
        bool bsDown = IsKeyDown(KEY_BACKSPACE);

        if (IsKeyPressed(KEY_BACKSPACE)) {
            if (sel_has(&sel)) buf_delete_range(&buf, sel_a(&sel), sel_z(&sel));
            else buf_backspace(&buf);
            sel_set_single(&sel, buf.cursor);
            bsNext = now + BS_INITIAL_DELAY;
            bsHeldPrev = true;
            dirty = true;
        } else if (bsDown && bsHeldPrev && now >= bsNext) {
            if (sel_has(&sel)) buf_delete_range(&buf, sel_a(&sel), sel_z(&sel));
            else buf_backspace(&buf);
            sel_set_single(&sel, buf.cursor);
            bsNext = now + BS_REPEAT_RATE;
            dirty = true;
        } else if (!bsDown) {
            bsHeldPrev = false;
        }

        // Typing
        int ch = GetCharPressed();
        while (ch > 0) {
            if (sel_has(&sel)) { buf_delete_range(&buf, sel_a(&sel), sel_z(&sel)); sel_set_single(&sel, buf.cursor); }

            if (ch == 9) {
                const char *spaces = "    ";
                buf_insert_bytes(&buf, spaces, 4);
                dirty = true;
            } else if (ch >= 32 && ch <= 126) {
                buf_insert_byte(&buf, (char)ch);
                dirty = true;
            }
            sel_set_single(&sel, buf.cursor);
            ch = GetCharPressed();
        }

        // Cursor movement + selection
        int curRow = 0, curCol = 0;
        cursor_row_col(&buf, &curRow, &curCol);

        bool shift = shiftKey;
        if (shift && !sel.active) { sel.active = true; sel.anchor = buf.cursor; sel.caret = buf.cursor; }

        if (IsKeyPressed(KEY_LEFT)) {
            if (buf.cursor > 0) buf.cursor--;
            if (shift) sel.caret = buf.cursor; else sel_set_single(&sel, buf.cursor);
        }
        if (IsKeyPressed(KEY_RIGHT)) {
            if (buf.cursor < buf.len) buf.cursor++;
            if (shift) sel.caret = buf.cursor; else sel_set_single(&sel, buf.cursor);
        }

        cursor_row_col(&buf, &curRow, &curCol);

        if (IsKeyPressed(KEY_HOME)) {
            move_home(&buf);
            if (shift) sel.caret = buf.cursor; else sel_set_single(&sel, buf.cursor);
        }
        if (IsKeyPressed(KEY_END)) {
            move_end(&buf);
            if (shift) sel.caret = buf.cursor; else sel_set_single(&sel, buf.cursor);
        }

        cursor_row_col(&buf, &curRow, &curCol);

        if (IsKeyPressed(KEY_UP)) {
            desiredCol = curCol;
            int newRow = (curRow > 0) ? curRow - 1 : 0;
            buf.cursor = index_at_row_col(&buf, newRow, desiredCol);
            if (shift) sel.caret = buf.cursor; else sel_set_single(&sel, buf.cursor);
        }
        if (IsKeyPressed(KEY_DOWN)) {
            desiredCol = curCol;
            int maxRow2 = total_rows(&buf) - 1;
            int newRow = (curRow < maxRow2) ? curRow + 1 : maxRow2;
            buf.cursor = index_at_row_col(&buf, newRow, desiredCol);
            if (shift) sel.caret = buf.cursor; else sel_set_single(&sel, buf.cursor);
        }

        cursor_row_col(&buf, &curRow, &curCol);
        if (!shift) desiredCol = curCol;

        if (curRow < scrollRow) scrollRow = curRow;
        if (curRow >= scrollRow + visibleRows) scrollRow = curRow - visibleRows + 1;
        scrollRow = clampi(scrollRow, 0, maxScroll);

        // ---------- DRAW ----------
        BeginDrawing();
        ClearBackground(bg);

        // Main card
        DrawRectangleRounded((Rectangle){ (float)cardX, (float)cardY, (float)cardW, (float)cardH }, 0.08f, 12, panel);
        DrawRectangleRoundedLines((Rectangle){ (float)cardX, (float)cardY, (float)cardW, (float)cardH }, 0.08f, 12, border);

        // Editor text (draw FIRST so menus are fully opaque on top)
        cursor_row_col(&buf, &curRow, &curCol);

        int cursorLineStart = line_start_index(&buf, curRow);
        int cursorLineEnd   = line_end_index(&buf, cursorLineStart);
        int cursorLineLen   = cursorLineEnd - cursorLineStart;
        int cursorOffInLine = clampi(buf.cursor - cursorLineStart, 0, cursorLineLen);

        float maxTextWidth = textArea.width;

        int lineIdx = line_start_index(&buf, scrollRow);
        int drawnVisual = 0;

        for (int row = scrollRow; row < total_rows(&buf) && drawnVisual < visibleRows; row++) {
            int end = line_end_index(&buf, lineIdx);
            int lineLen = end - lineIdx;

            if (lineLen == 0) {
                float y = textArea.y + drawnVisual * lineH;
                if (cursorOn && row == curRow && cursorOffInLine == 0) {
                    DrawRectangle((int)textArea.x, (int)(y + 4), 2, (int)(fontSize + 4), accent);
                }
                drawnVisual++;
            } else {
                int off = 0;
                while (off < lineLen && drawnVisual < visibleRows) {
                    float y = textArea.y + drawnVisual * lineH;

                    int remaining = lineLen - off;
                    int take = wrap_fit_count(editorFont, fontSize, maxTextWidth, buf.data + lineIdx + off, remaining);
                    if (take <= 0) take = 1;
                    if (take > remaining) take = remaining;

                    char tmp[4096] = {0};
                    int n = (take < (int)sizeof(tmp) - 1) ? take : (int)sizeof(tmp) - 1;
                    memcpy(tmp, buf.data + lineIdx + off, (size_t)n);
                    tmp[n] = '\0';

                    if (sel_has(&sel)) {
                        int a = sel_a(&sel), z = sel_z(&sel);
                        int segA = lineIdx + off;
                        int segZ = lineIdx + off + take;
                        int hiA = maxi(a, segA);
                        int hiZ = mini(z, segZ);
                        if (hiZ > hiA) {
                            int colA = hiA - segA;
                            int colZ = hiZ - segA;
                            float x1 = textArea.x + colA * charW;
                            float x2 = textArea.x + colZ * charW;
                            DrawRectangle((int)x1, (int)(y + 3), (int)(x2 - x1), (int)(fontSize + 6), selBg);
                        }
                    }

                    DrawTextEx(editorFont, tmp, (Vector2){ textArea.x, y }, fontSize, 0, text);

                    if (cursorOn && row == curRow) {
                        bool lastSeg = (off + take == lineLen);
                        bool caretHere =
                            (cursorOffInLine >= off && cursorOffInLine < off + take) ||
                            (lastSeg && cursorOffInLine == lineLen);

                        if (caretHere) {
                            int caretLocal = cursorOffInLine - off;
                            if (lastSeg && cursorOffInLine == lineLen) caretLocal = take;

                            char left[4096];
                            int leftLen = clampi(caretLocal, 0, n);
                            if (leftLen > 0) memcpy(left, tmp, (size_t)leftLen);
                            left[leftLen] = '\0';

                            float cx = textArea.x + MeasureTextEx(editorFont, left, fontSize, 0).x;
                            DrawRectangle((int)cx, (int)(y + 4), 2, (int)(fontSize + 4), accent);
                        }
                    }

                    drawnVisual++;
                    off += take;
                }
            }

            if (end >= buf.len) break;
            lineIdx = end + 1;
        }

        // Top bar (draw after editor)
        DrawRectangle(0, 0, w, topBarH, panel);
        draw_text(uiFont, "Pen", 16, 12, 20.0f, text);

        // Dirty dot (ONLY when dirty)
        if (dirty) DrawCircle(w - 18, 22, 5, accent);

        Rectangle fileBtn = (Rectangle){ 90, 8, 70, 28 };
        Rectangle editBtn = (Rectangle){ 170, 8, 70, 28 };

        bool clickFile = ui_button(fileBtn, "File", uiFont, uiSize,
                                  (Color){28,33,41,255}, (Color){33,39,49,255}, (Color){40,46,58,255}, text);
        bool clickEdit = ui_button(editBtn, "Edit", uiFont, uiSize,
                                  (Color){28,33,41,255}, (Color){33,39,49,255}, (Color){40,46,58,255}, text);

        if (clickFile) menu = (menu == MENU_FILE) ? MENU_NONE : MENU_FILE;
        if (clickEdit) menu = (menu == MENU_EDIT) ? MENU_NONE : MENU_EDIT;

        // Dropdowns (draw LAST so they are not “transparent”)
        bool clickedItem = false;

        if (menu == MENU_FILE) {
            Rectangle drop = (Rectangle){ fileBtn.x, fileBtn.y + fileBtn.height + 6, 240, 4*28 };
            DrawRectangleRounded(drop, 0.10f, 10, (Color){28,33,41,255});
            DrawRectangleRoundedLines(drop, 0.10f, 10, border);

            Rectangle r1 = (Rectangle){ drop.x, drop.y + 0,  drop.width, 28 };
            Rectangle r2 = (Rectangle){ drop.x, drop.y + 28, drop.width, 28 };
            Rectangle r3 = (Rectangle){ drop.x, drop.y + 56, drop.width, 28 };
            Rectangle r4 = (Rectangle){ drop.x, drop.y + 84, drop.width, 28 };

            if (menu_item_lr(r1, "Open…", "Ctrl+O", uiFont, uiSize, text)) {
                if (do_open(&buf, &sel, &scrollRow, currentPath, (int)sizeof(currentPath), &hasPath)) {
                    dirty = false;
                    toast_set(&toast, "Opened", 1.0);
                }
                clickedItem = true; menu = MENU_NONE;
            }
            if (menu_item_lr(r2, "Save", "Ctrl+S", uiFont, uiSize, text)) {
                if (do_save(&buf, currentPath, (int)sizeof(currentPath), &hasPath)) {
                    dirty = false;
                    toast_set(&toast, "Saved", 1.2);
                }
                clickedItem = true; menu = MENU_NONE;
            }
            if (menu_item_lr(r3, "Save As…", "Ctrl+Shift+S", uiFont, uiSize, text)) {
                if (do_save_as(&buf, currentPath, (int)sizeof(currentPath), &hasPath)) {
                    dirty = false;
                    toast_set(&toast, "Saved As", 1.2);
                }
                clickedItem = true; menu = MENU_NONE;
            }
            if (menu_item_lr(r4, "Quit", "Ctrl+Q", uiFont, uiSize, text)) {
                quitRequested = true;
                clickedItem = true; menu = MENU_NONE;
            }
        }

        if (menu == MENU_EDIT) {
            Rectangle drop = (Rectangle){ editBtn.x, editBtn.y + editBtn.height + 6, 240, 4*28 };
            DrawRectangleRounded(drop, 0.10f, 10, (Color){28,33,41,255});
            DrawRectangleRoundedLines(drop, 0.10f, 10, border);

            Rectangle r1 = (Rectangle){ drop.x, drop.y + 0,  drop.width, 28 };
            Rectangle r2 = (Rectangle){ drop.x, drop.y + 28, drop.width, 28 };
            Rectangle r3 = (Rectangle){ drop.x, drop.y + 56, drop.width, 28 };
            Rectangle r4 = (Rectangle){ drop.x, drop.y + 84, drop.width, 28 };

            if (menu_item_lr(r1, "Cut", "Ctrl+X", uiFont, uiSize, text)) { clickedItem = true; menu = MENU_NONE; }
            if (menu_item_lr(r2, "Copy", "Ctrl+C", uiFont, uiSize, text)) { clickedItem = true; menu = MENU_NONE; }
            if (menu_item_lr(r3, "Paste", "Ctrl+V", uiFont, uiSize, text)) { clickedItem = true; menu = MENU_NONE; }
            if (menu_item_lr(r4, "Select All", "Ctrl+A", uiFont, uiSize, text)) {
                sel.active = true; sel.anchor = 0; sel.caret = buf.len; buf.cursor = buf.len;
                clickedItem = true; menu = MENU_NONE;
            }
        }

        if (menu != MENU_NONE && IsMouseButtonPressed(MOUSE_LEFT_BUTTON) && !clickedItem) {
            Rectangle dropArea = (Rectangle){0,0,0,0};
            if (menu == MENU_FILE) dropArea = (Rectangle){ fileBtn.x, fileBtn.y + fileBtn.height + 6, 240, 4*28 };
            if (menu == MENU_EDIT) dropArea = (Rectangle){ editBtn.x, editBtn.y + editBtn.height + 6, 240, 4*28 };
            bool inBtns = CheckCollisionPointRec(mouse, fileBtn) || CheckCollisionPointRec(mouse, editBtn);
            bool inDrop = CheckCollisionPointRec(mouse, dropArea);
            if (!inBtns && !inDrop) menu = MENU_NONE;
        }

        // Status bar
        DrawRectangle(0, h - 34, w, 34, panel);
        const char *name = hasPath ? base_name(currentPath) : "(untitled)";
        char status[512];
        snprintf(status, sizeof(status),
                 "%s  |  Ctrl+O Open  Ctrl+S Save  Ctrl+Shift+S Save As  |  Ctrl+C/X/V/A  |  Row %d Col %d   (Esc quits)",
                 name, curRow + 1, curCol + 1);
        draw_text(uiFont, status, 16, (float)h - 24, 14.0f, muted);

        // Toast popup (top-right, under the title bar)
        if (toast.until > GetTime() && toast.msg[0]) {
            Vector2 tw = MeasureTextEx(uiFont, toast.msg, 16.0f, 0);
            float padX = 14, padY = 10;
            float boxW = tw.x + padX*2;
            float boxH = 16.0f + padY*2;

            Rectangle box = { (float)w - boxW - 18, (float)topBarH + 12, boxW, boxH };
            DrawRectangleRounded(box, 0.25f, 10, (Color){28,33,41,255});
            DrawRectangleRoundedLines(box, 0.25f, 10, border);
            draw_text(uiFont, toast.msg, box.x + padX, box.y + padY - 1, 16.0f, text);
        }

        EndDrawing();
    }

    UnloadFont(editorFont);
    if (uiFont.texture.id != editorFont.texture.id) UnloadFont(uiFont);

    buf_free(&buf);
    CloseWindow();
    return 0;
}

