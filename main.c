#define _GNU_SOURCE

#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CTRL_KEY(k) ((k) & 0x1f)

/* =========================================================
 * 常量
 * ========================================================= */

/* 编辑模式：普通编辑 / 搜索 / 替换 的各个子状态 */
#define MODE_NORMAL         0
#define MODE_SEARCH_INPUT   1   /* 正在输入搜索词 */
#define MODE_SEARCH_NAV     2   /* 已搜索，浏览候选 */
#define MODE_REPLACE_SEARCH 3   /* 替换：输入搜索词 */
#define MODE_REPLACE_TEXT   4   /* 替换：输入替换文本 */
#define MODE_REPLACE_NAV    5   /* 替换：逐个确认候选 */

#define SEARCH_QUERY_MAX 256
#define REPLACE_TEXT_MAX 256

#define UNDO_MAX 100

/* undo/redo 动作类型 */
typedef enum {
    ACTION_INSERT_CHAR, /* 插入了一个字符：text 记录该字符，撤销时删除 */
    ACTION_DELETE_CHAR, /* 删除了一个字符：text 记录被删字符，撤销时插回 */
    ACTION_NEWLINE,     /* 插入了一个换行（回车分行）：撤销时合并回去 */
    ACTION_MERGE_ROW,   /* 退格在行首合并了两行：撤销时在 col 处重新拆行 */
    ACTION_CUT,         /* 剪切了一段文本：text 记录被剪文本 */
    ACTION_PASTE        /* 粘贴了一段文本：text 记录被粘贴文本 */
} ActionType;

typedef struct {
    ActionType type;
    int row, col;         /* 动作发生的起始位置 */
    int end_row, end_col; /* 对于跨行范围类动作（剪切/粘贴），记录结束位置 */
    char *text;           /* 关联文本内容（可能为 NULL） */
    int text_len;
} Action;

typedef struct {
    Action items[UNDO_MAX];
    int top; /* 栈顶元素个数，也就是当前保存的步数 */
} ActionStack;

/* =========================================================
 * 数据结构
 * ========================================================= */

typedef struct {
    char *chars;
    int len;
} Row;

typedef struct {
    Row *rows;
    int num_rows;

    int cx;
    int cy;

    int row_offset;
    int col_offset;

    int modified;

    char *filename;

    char status_msg[80];

    int quit_times;

    /* ---------------- 搜索 / 替换 ---------------- */
    int mode; /* MODE_NORMAL / MODE_SEARCH_* / MODE_REPLACE_* */

    char search_query[SEARCH_QUERY_MAX];
    int search_len;

    char replace_query[SEARCH_QUERY_MAX];
    int replace_query_len;

    char replace_text[REPLACE_TEXT_MAX];
    int replace_text_len;

    /* 当前高亮的候选匹配：match_row == -1 表示当前没有匹配 */
    int match_row;
    int match_start;
    int match_len;

    /* 进入搜索/替换前保存的光标与滚动位置，Esc 时用于还原 */
    int saved_cx, saved_cy;
    int saved_row_offset, saved_col_offset;

    /* 提示行（搜索/替换输入行）里光标应停留的列，绘制时计算 */
    int prompt_cursor_col;

    /* ---------------- 选区 / 剪贴板 ---------------- */
    int selecting;
    int sel_start_row, sel_start_col;
    int sel_end_row, sel_end_col;

    char *clipboard;
    int clipboard_len;

    /* ---------------- 撤销 / 重做 ---------------- */
    ActionStack undo_stack;
    ActionStack redo_stack;
    int undo_recording; /* 为 0 时，插入/删除字符等操作不会被记录（用于回放） */
} Editor;

/* =========================================================
 * Undo / Redo 栈的基本操作
 * ========================================================= */

static void action_free(Action *a)
{
    free(a->text);
    a->text = NULL;
}

static void stack_clear(ActionStack *s)
{
    for (int i = 0; i < s->top; i++) {
        action_free(&s->items[i]);
    }
    s->top = 0;
}

static void stack_push(ActionStack *s, Action a)
{
    if (s->top == UNDO_MAX) {
        /* 超过步数上限，丢弃最早的一步 */
        action_free(&s->items[0]);
        memmove(&s->items[0], &s->items[1], sizeof(Action) * (UNDO_MAX - 1));
        s->top--;
    }
    s->items[s->top++] = a;
}

static Action stack_pop(ActionStack *s)
{
    return s->items[--s->top];
}

/* 前置声明：定义顺序在文件后面，但 undo/redo 等函数需要提前引用 */
void editor_set_status(Editor *e, const char *msg);

/* =========================================================
 * Row
 * ========================================================= */

void editor_insert_row(Editor *e, const char *s, int len)
{
    e->rows = realloc(
        e->rows,
        sizeof(Row) * (e->num_rows + 1)
    );

    Row *row = &e->rows[e->num_rows];

    row->chars = malloc(len + 1);

    memcpy(row->chars, s, len);
    row->chars[len] = '\0';

    row->len = len;

    e->num_rows++;
}


void editor_delete_row(Editor *e, int at)
{
    if (at < 0 || at >= e->num_rows) {
        return;
    }

    free(e->rows[at].chars);

    memmove(
        &e->rows[at],
        &e->rows[at + 1],
        sizeof(Row) * (e->num_rows - at - 1)
    );

    e->num_rows--;

    if (e->num_rows == 0) {
        free(e->rows);
        e->rows = NULL;
    } else {
        e->rows = realloc(
            e->rows,
            sizeof(Row) * e->num_rows
        );
    }
}


void editor_free(Editor *e)
{
    for (int i = 0; i < e->num_rows; i++) {
        free(e->rows[i].chars);
    }

    free(e->rows);
    free(e->filename);
    free(e->clipboard);

    stack_clear(&e->undo_stack);
    stack_clear(&e->redo_stack);
}


/* =========================================================
 * 初始化
 * ========================================================= */

void editor_init(Editor *e)
{
    memset(e, 0, sizeof(Editor));

    e->quit_times = 2;
    e->mode = MODE_NORMAL;
    e->match_row = -1;
    e->undo_recording = 1;
}


/* =========================================================
 * File
 * ========================================================= */

void editor_open(Editor *e, const char *filename)
{
    FILE *fp = fopen(filename, "r");

    if (fp == NULL) {
        return;
    }

    char *line = NULL;
    size_t capacity = 0;
    ssize_t len;

    while ((len = getline(&line, &capacity, fp)) != -1) {

        while (len > 0 &&
               (line[len - 1] == '\n' ||
                line[len - 1] == '\r')) {
            len--;
        }

        editor_insert_row(e, line, len);
    }

    free(line);
    fclose(fp);

    e->modified = 0;
}


/* =========================================================
 * Insert Character
 * ========================================================= */

void editor_insert_char(Editor *e, int c)
{
    if (e->num_rows == 0) {
        editor_insert_row(e, "", 0);
    }

    int row_idx = e->cy;
    int col_idx = e->cx;

    Row *row = &e->rows[e->cy];

    row->chars = realloc(
        row->chars,
        row->len + 2
    );

    memmove(
        &row->chars[e->cx + 1],
        &row->chars[e->cx],
        row->len - e->cx + 1
    );

    row->chars[e->cx] = c;

    row->len++;
    e->cx++;

    e->modified = 1;

    if (e->undo_recording) {
        char ch = (char)c;
        Action a = {
            .type = ACTION_INSERT_CHAR,
            .row = row_idx,
            .col = col_idx,
            .end_row = row_idx,
            .end_col = col_idx + 1,
            .text = strndup(&ch, 1),
            .text_len = 1
        };
        stack_clear(&e->redo_stack);
        stack_push(&e->undo_stack, a);
    }
}


/* =========================================================
 * Newline
 * ========================================================= */

void editor_insert_newline(Editor *e)
{
    if (e->num_rows == 0) {
        editor_insert_row(e, "", 0);
    }

    int start_row = e->cy;
    int start_col = e->cx;

    Row *row = &e->rows[e->cy];

    int left_len = e->cx;
    int right_len = row->len - e->cx;

    char *right = malloc(right_len + 1);

    memcpy(
        right,
        &row->chars[e->cx],
        right_len
    );

    right[right_len] = '\0';

    row->chars[left_len] = '\0';
    row->len = left_len;

    e->rows = realloc(
        e->rows,
        sizeof(Row) * (e->num_rows + 1)
    );

    memmove(
        &e->rows[e->cy + 2],
        &e->rows[e->cy + 1],
        sizeof(Row) * (e->num_rows - e->cy - 1)
    );

    e->rows[e->cy + 1].chars = right;
    e->rows[e->cy + 1].len = right_len;

    e->num_rows++;

    e->cy++;
    e->cx = 0;

    e->modified = 1;

    if (e->undo_recording) {
        Action a = {
            .type = ACTION_NEWLINE,
            .row = start_row,
            .col = start_col,
            .end_row = start_row + 1,
            .end_col = 0,
            .text = NULL,
            .text_len = 0
        };
        stack_clear(&e->redo_stack);
        stack_push(&e->undo_stack, a);
    }
}


/* =========================================================
 * Merge Rows
 * ========================================================= */

void editor_merge_rows(Editor *e, int at)
{
    if (at < 0 || at >= e->num_rows - 1) {
        return;
    }

    Row *current = &e->rows[at];
    Row *next = &e->rows[at + 1];

    int old_len = current->len;

    current->chars = realloc(
        current->chars,
        current->len + next->len + 1
    );

    memcpy(
        &current->chars[old_len],
        next->chars,
        next->len
    );

    current->len += next->len;
    current->chars[current->len] = '\0';

    editor_delete_row(e, at + 1);
}


/* =========================================================
 * 在指定位置拆行（editor_insert_newline 的底层复用版本，
 * 供 undo/redo 回放时恢复换行使用，不记录撤销历史）
 * ========================================================= */

void editor_split_row(Editor *e, int row, int col)
{
    int saved_recording = e->undo_recording;
    e->undo_recording = 0;

    e->cy = row;
    e->cx = col;

    editor_insert_newline(e);

    e->undo_recording = saved_recording;
}


/* =========================================================
 * Backspace
 * ========================================================= */

void editor_del_char(Editor *e)
{
    if (e->num_rows == 0) {
        return;
    }

    Row *row = &e->rows[e->cy];

    if (e->cx > 0) {

        char removed = row->chars[e->cx - 1];

        memmove(
            &row->chars[e->cx - 1],
            &row->chars[e->cx],
            row->len - e->cx + 1
        );

        row->len--;
        e->cx--;

        row->chars = realloc(
            row->chars,
            row->len + 1
        );

        e->modified = 1;

        if (e->undo_recording) {
            Action a = {
                .type = ACTION_DELETE_CHAR,
                .row = e->cy,
                .col = e->cx,
                .end_row = e->cy,
                .end_col = e->cx + 1,
                .text = strndup(&removed, 1),
                .text_len = 1
            };
            stack_clear(&e->redo_stack);
            stack_push(&e->undo_stack, a);
        }

        return;
    }

    if (e->cy == 0) {
        return;
    }

    int previous_len = e->rows[e->cy - 1].len;

    if (e->undo_recording) {
        Action a = {
            .type = ACTION_MERGE_ROW,
            .row = e->cy - 1,
            .col = previous_len,
            .end_row = e->cy - 1,
            .end_col = previous_len,
            .text = NULL,
            .text_len = 0
        };
        stack_clear(&e->redo_stack);
        stack_push(&e->undo_stack, a);
    }

    editor_merge_rows(e, e->cy - 1);

    e->cy--;
    e->cx = previous_len;

    e->modified = 1;
}


/* =========================================================
 * Delete
 * ========================================================= */

void editor_delete_char(Editor *e)
{
    if (e->num_rows == 0) {
        return;
    }

    Row *row = &e->rows[e->cy];

    if (e->cx < row->len) {

        char removed = row->chars[e->cx];

        memmove(
            &row->chars[e->cx],
            &row->chars[e->cx + 1],
            row->len - e->cx
        );

        row->len--;

        row->chars = realloc(
            row->chars,
            row->len + 1
        );

        e->modified = 1;

        if (e->undo_recording) {
            Action a = {
                .type = ACTION_DELETE_CHAR,
                .row = e->cy,
                .col = e->cx,
                .end_row = e->cy,
                .end_col = e->cx + 1,
                .text = strndup(&removed, 1),
                .text_len = 1
            };
            stack_clear(&e->redo_stack);
            stack_push(&e->undo_stack, a);
        }

        return;
    }

    if (e->cy < e->num_rows - 1) {

        if (e->undo_recording) {
            Action a = {
                .type = ACTION_MERGE_ROW,
                .row = e->cy,
                .col = row->len,
                .end_row = e->cy,
                .end_col = row->len,
                .text = NULL,
                .text_len = 0
            };
            stack_clear(&e->redo_stack);
            stack_push(&e->undo_stack, a);
        }

        editor_merge_rows(e, e->cy);

        e->modified = 1;
    }
}


/* =========================================================
 * 在指定位置插入一段文本（内部使用，不记录撤销历史，
 * 供 undo/redo 回放、粘贴等场景复用）
 * ========================================================= */

void editor_insert_text_at(Editor *e, int row, int col, const char *text, int len)
{
    int saved_recording = e->undo_recording;
    e->undo_recording = 0;

    e->cy = row;
    e->cx = col;

    for (int i = 0; i < len; i++) {
        char c = text[i];

        if (c == '\n') {
            editor_insert_newline(e);
        } else {
            editor_insert_char(e, (unsigned char)c);
        }
    }

    e->undo_recording = saved_recording;
}


/* =========================================================
 * 删除 [start, end) 范围内的文本（跨行），用于选区剪切、
 * 撤销插入/粘贴、重做剪切等场景
 * ========================================================= */

void editor_delete_range(Editor *e, int sr, int sc, int er, int ec)
{
    if (e->num_rows == 0) {
        return;
    }

    if (sr == er) {
        Row *row = &e->rows[sr];

        if (ec > row->len) ec = row->len;
        if (sc < 0) sc = 0;
        if (ec <= sc) {
            e->cy = sr;
            e->cx = sc;
            return;
        }

        int cut_len = ec - sc;

        memmove(
            &row->chars[sc],
            &row->chars[ec],
            row->len - ec + 1
        );

        row->len -= cut_len;

        row->chars = realloc(row->chars, row->len + 1);

    } else {
        Row *first = &e->rows[sr];
        Row *last = &e->rows[er];

        int tail_len = last->len - ec;
        if (tail_len < 0) tail_len = 0;

        first->chars = realloc(first->chars, sc + tail_len + 1);

        memcpy(
            &first->chars[sc],
            &last->chars[ec],
            tail_len
        );

        first->len = sc + tail_len;
        first->chars[first->len] = '\0';

        for (int i = sr + 1; i <= er; i++) {
            free(e->rows[i].chars);
        }

        memmove(
            &e->rows[sr + 1],
            &e->rows[er + 1],
            sizeof(Row) * (e->num_rows - er - 1)
        );

        e->num_rows -= (er - sr);

        e->rows = realloc(e->rows, sizeof(Row) * e->num_rows);
    }

    e->cy = sr;
    e->cx = sc;
    e->modified = 1;
}


/* =========================================================
 * 提取 [start, end) 范围内的文本（跨行以 '\n' 连接），
 * 用于复制 / 剪切到剪贴板
 * ========================================================= */

char *editor_extract_range(Editor *e, int sr, int sc, int er, int ec, int *out_len)
{
    int cap = 64;
    int len = 0;
    char *buf = malloc(cap);

    for (int r = sr; r <= er; r++) {
        Row *row = &e->rows[r];

        int start = (r == sr) ? sc : 0;
        int end = (r == er) ? ec : row->len;

        if (start < 0) start = 0;
        if (end > row->len) end = row->len;
        if (end < start) end = start;

        int piece_len = end - start;

        while (len + piece_len + 2 > cap) {
            cap *= 2;
            buf = realloc(buf, cap);
        }

        memcpy(&buf[len], &row->chars[start], piece_len);
        len += piece_len;

        if (r != er) {
            buf[len++] = '\n';
        }
    }

    buf[len] = '\0';
    *out_len = len;

    return buf;
}


/* 根据起点和一段文本（可能含 '\n'）计算文本插入后光标应落在的终点位置 */
void editor_compute_end_pos(int start_row, int start_col, const char *text, int len,
                             int *end_row, int *end_col)
{
    int row = start_row;
    int col = start_col;

    for (int i = 0; i < len; i++) {
        if (text[i] == '\n') {
            row++;
            col = 0;
        } else {
            col++;
        }
    }

    *end_row = row;
    *end_col = col;
}


/* =========================================================
 * Undo / Redo
 * ========================================================= */

void editor_undo(Editor *e)
{
    if (e->undo_stack.top == 0) {
        editor_set_status(e, "Nothing to undo");
        return;
    }

    Action a = stack_pop(&e->undo_stack);

    e->undo_recording = 0;

    switch (a.type) {

    case ACTION_INSERT_CHAR:
        editor_delete_range(e, a.row, a.col, a.end_row, a.end_col);
        break;

    case ACTION_DELETE_CHAR:
        editor_insert_text_at(e, a.row, a.col, a.text, a.text_len);
        e->cy = a.row;
        e->cx = a.col + a.text_len;
        break;

    case ACTION_NEWLINE:
        editor_merge_rows(e, a.row);
        e->cy = a.row;
        e->cx = a.col;
        break;

    case ACTION_MERGE_ROW:
        editor_split_row(e, a.row, a.col);
        e->cy = a.row + 1;
        e->cx = 0;
        break;

    case ACTION_CUT:
        editor_insert_text_at(e, a.row, a.col, a.text, a.text_len);
        editor_compute_end_pos(a.row, a.col, a.text, a.text_len, &a.end_row, &a.end_col);
        e->cy = a.end_row;
        e->cx = a.end_col;
        break;

    case ACTION_PASTE:
        editor_delete_range(e, a.row, a.col, a.end_row, a.end_col);
        break;
    }

    e->undo_recording = 1;
    e->modified = 1;

    stack_push(&e->redo_stack, a);

    editor_set_status(e, "Undo");
}


void editor_redo(Editor *e)
{
    if (e->redo_stack.top == 0) {
        editor_set_status(e, "Nothing to redo");
        return;
    }

    Action a = stack_pop(&e->redo_stack);

    e->undo_recording = 0;

    switch (a.type) {

    case ACTION_INSERT_CHAR:
        editor_insert_text_at(e, a.row, a.col, a.text, a.text_len);
        e->cy = a.end_row;
        e->cx = a.end_col;
        break;

    case ACTION_DELETE_CHAR:
        editor_delete_range(e, a.row, a.col, a.end_row, a.end_col);
        e->cy = a.row;
        e->cx = a.col;
        break;

    case ACTION_NEWLINE:
        editor_split_row(e, a.row, a.col);
        e->cy = a.row + 1;
        e->cx = 0;
        break;

    case ACTION_MERGE_ROW:
        editor_merge_rows(e, a.row);
        e->cy = a.row;
        e->cx = a.col;
        break;

    case ACTION_CUT:
        editor_delete_range(e, a.row, a.col, a.end_row, a.end_col);
        break;

    case ACTION_PASTE:
        editor_insert_text_at(e, a.row, a.col, a.text, a.text_len);
        e->cy = a.end_row;
        e->cx = a.end_col;
        break;
    }

    e->undo_recording = 1;
    e->modified = 1;

    stack_push(&e->undo_stack, a);

    editor_set_status(e, "Redo");
}


/* =========================================================
 * Cursor
 * ========================================================= */

void editor_move_cursor(Editor *e, int key)
{
    if (e->num_rows == 0) {
        return;
    }

    if (key == KEY_UP) {

        if (e->cy > 0) {
            e->cy--;
        }

    } else if (key == KEY_DOWN) {

        if (e->cy < e->num_rows - 1) {
            e->cy++;
        }

    } else if (key == KEY_LEFT) {

        if (e->cx > 0) {
            e->cx--;

        } else if (e->cy > 0) {

            e->cy--;
            e->cx = e->rows[e->cy].len;
        }

    } else if (key == KEY_RIGHT) {

        if (e->cx < e->rows[e->cy].len) {
            e->cx++;

        } else if (e->cy < e->num_rows - 1) {

            e->cy++;
            e->cx = 0;
        }
    }

    if (e->cx > e->rows[e->cy].len) {
        e->cx = e->rows[e->cy].len;
    }
}


/* =========================================================
 * 选区 / 剪贴板
 *
 * 终端兼容性说明：
 *   - KEY_SLEFT / KEY_SRIGHT 是 ncurses 中专门对应 Shift+方向键（左/右）
 *     的键码，绝大多数终端（xterm、gnome-terminal、iTerm2 等）的 terminfo
 *     都定义了对应的 kLFT/kRIT 能力，兼容性较好。
 *   - KEY_SF / KEY_SR（scroll forward / scroll reverse）历史上用于"向前/
 *     向后滚动"，但很多 terminfo 数据库（包括常见的 xterm 系列）把
 *     Shift+Down / Shift+Up 映射到了这两个键码上，因此这里借用它们表示
 *     Shift+下 / Shift+上。如果目标终端没有这样映射，这两个方向的选区
 *     可能不会触发；此时可以退而求其次改用 Ctrl+方向键，或用 define_key()
 *     手动注册终端的转义序列。
 * ========================================================= */

void editor_get_selection_range(Editor *e, int *sr, int *sc, int *er, int *ec)
{
    int r1 = e->sel_start_row, c1 = e->sel_start_col;
    int r2 = e->sel_end_row, c2 = e->sel_end_col;

    if (r1 > r2 || (r1 == r2 && c1 > c2)) {
        *sr = r2; *sc = c2;
        *er = r1; *ec = c1;
    } else {
        *sr = r1; *sc = c1;
        *er = r2; *ec = c2;
    }
}


void editor_selection_move(Editor *e, int key)
{
    if (e->num_rows == 0) {
        return;
    }

    if (!e->selecting) {
        e->selecting = 1;
        e->sel_start_row = e->cy;
        e->sel_start_col = e->cx;
    }

    int nav_key;

    if (key == KEY_SLEFT) {
        nav_key = KEY_LEFT;
    } else if (key == KEY_SRIGHT) {
        nav_key = KEY_RIGHT;
    } else if (key == KEY_SR) {
        nav_key = KEY_UP;
    } else {
        nav_key = KEY_DOWN;
    }

    editor_move_cursor(e, nav_key);

    e->sel_end_row = e->cy;
    e->sel_end_col = e->cx;
}


void editor_copy(Editor *e)
{
    if (!e->selecting) {
        editor_set_status(e, "Nothing selected");
        return;
    }

    int sr, sc, er, ec;
    editor_get_selection_range(e, &sr, &sc, &er, &ec);

    free(e->clipboard);
    e->clipboard = editor_extract_range(e, sr, sc, er, ec, &e->clipboard_len);

    editor_set_status(e, "Copied selection to clipboard");
}


void editor_cut(Editor *e)
{
    if (!e->selecting) {
        editor_set_status(e, "Nothing selected");
        return;
    }

    int sr, sc, er, ec;
    editor_get_selection_range(e, &sr, &sc, &er, &ec);

    int len;
    char *text = editor_extract_range(e, sr, sc, er, ec, &len);

    free(e->clipboard);
    e->clipboard = malloc(len + 1);
    memcpy(e->clipboard, text, len + 1);
    e->clipboard_len = len;

    editor_delete_range(e, sr, sc, er, ec);

    e->selecting = 0;
    e->modified = 1;

    if (e->undo_recording) {
        Action a = {
            .type = ACTION_CUT,
            .row = sr,
            .col = sc,
            .end_row = sr,   /* 撤销（重新插入）后会更新为真实终点 */
            .end_col = sc,
            .text = text,
            .text_len = len
        };
        stack_clear(&e->redo_stack);
        stack_push(&e->undo_stack, a);
    } else {
        free(text);
    }

    editor_set_status(e, "Cut selection to clipboard");
}


void editor_paste(Editor *e)
{
    if (!e->clipboard || e->clipboard_len == 0) {
        editor_set_status(e, "Clipboard empty");
        return;
    }

    e->selecting = 0;

    int start_row = e->cy;
    int start_col = e->cx;

    int saved_recording = e->undo_recording;
    e->undo_recording = 0;

    for (int i = 0; i < e->clipboard_len; i++) {
        char c = e->clipboard[i];

        if (c == '\n') {
            editor_insert_newline(e);
        } else {
            editor_insert_char(e, (unsigned char)c);
        }
    }

    e->undo_recording = saved_recording;

    int end_row = e->cy;
    int end_col = e->cx;

    e->modified = 1;

    if (e->undo_recording) {
        char *text = malloc(e->clipboard_len + 1);
        memcpy(text, e->clipboard, e->clipboard_len + 1);

        Action a = {
            .type = ACTION_PASTE,
            .row = start_row,
            .col = start_col,
            .end_row = end_row,
            .end_col = end_col,
            .text = text,
            .text_len = e->clipboard_len
        };
        stack_clear(&e->redo_stack);
        stack_push(&e->undo_stack, a);
    }

    editor_set_status(e, "Pasted from clipboard");
}


/* =========================================================
 * 搜索
 * ========================================================= */

/* 在一行内从 from_col 开始向后查找子串，返回起始列，找不到返回 -1 */
static int row_find_forward(Row *r, const char *q, int qlen, int from_col)
{
    if (qlen == 0) return -1;
    if (from_col < 0) from_col = 0;

    for (int i = from_col; i <= r->len - qlen; i++) {
        if (memcmp(&r->chars[i], q, qlen) == 0) {
            return i;
        }
    }
    return -1;
}

/* 在一行内查找不晚于 before_col 的最后一处匹配，返回起始列，找不到返回 -1 */
static int row_find_backward(Row *r, const char *q, int qlen, int before_col)
{
    if (qlen == 0) return -1;

    int max_start = r->len - qlen;
    if (before_col > max_start) before_col = max_start;
    if (before_col < 0) return -1;

    for (int i = before_col; i >= 0; i--) {
        if (memcmp(&r->chars[i], q, qlen) == 0) {
            return i;
        }
    }
    return -1;
}


/* 向前(dir=1)或向后(dir=-1)查找下一个候选，找到则更新
 * match_row/match_start/match_len 及光标位置并返回 1；
 * 找不到（含到达文件首/尾不回绕）则返回 0 */
int editor_search_next(Editor *e, int dir)
{
    if (e->search_len == 0 || e->num_rows == 0) {
        return 0;
    }

    int row = (e->match_row == -1) ? e->cy : e->match_row;
    int col;

    if (e->match_row == -1) {
        col = e->cx;
    } else if (dir == 1) {
        col = e->match_start + e->match_len;
    } else {
        col = e->match_start - 1;
    }

    if (dir == 1) {
        for (int r = row; r < e->num_rows; r++) {
            int from = (r == row) ? col : 0;
            int pos = row_find_forward(&e->rows[r], e->search_query, e->search_len, from);

            if (pos != -1) {
                e->match_row = r;
                e->match_start = pos;
                e->match_len = e->search_len;
                e->cy = r;
                e->cx = pos;
                return 1;
            }
        }
    } else {
        for (int r = row; r >= 0; r--) {
            int before = (r == row) ? col : e->rows[r].len;
            int pos = row_find_backward(&e->rows[r], e->search_query, e->search_len, before);

            if (pos != -1) {
                e->match_row = r;
                e->match_start = pos;
                e->match_len = e->search_len;
                e->cy = r;
                e->cx = pos;
                return 1;
            }
        }
    }

    return 0;
}


void editor_search_cancel(Editor *e)
{
    e->mode = MODE_NORMAL;
    e->match_row = -1;

    e->cx = e->saved_cx;
    e->cy = e->saved_cy;
    e->row_offset = e->saved_row_offset;
    e->col_offset = e->saved_col_offset;

    editor_set_status(e, "");
}


void editor_find_start(Editor *e)
{
    e->mode = MODE_SEARCH_INPUT;

    e->search_len = 0;
    e->search_query[0] = '\0';
    e->match_row = -1;

    e->saved_cx = e->cx;
    e->saved_cy = e->cy;
    e->saved_row_offset = e->row_offset;
    e->saved_col_offset = e->col_offset;

    editor_set_status(e, "Search: type query, Enter to search, Esc to cancel");
}


int editor_process_key_search_input(Editor *e, int key)
{
    if (key == 27) {
        editor_search_cancel(e);
        return 0;
    }

    if (key == '\n' || key == KEY_ENTER) {
        e->mode = MODE_SEARCH_NAV;

        if (editor_search_next(e, 1)) {
            editor_set_status(e, "Search: Enter=next  Ctrl-P=prev  Esc=exit");
        } else {
            editor_set_status(e, "Search: no match found");
        }
        return 0;
    }

    if (key == KEY_BACKSPACE || key == 127 || key == 8) {
        if (e->search_len > 0) {
            e->search_len--;
            e->search_query[e->search_len] = '\0';
        }
        return 0;
    }

    if (key >= 32 && key <= 126 && e->search_len < SEARCH_QUERY_MAX - 1) {
        e->search_query[e->search_len++] = (char)key;
        e->search_query[e->search_len] = '\0';
    }

    return 0;
}


int editor_process_key_search_nav(Editor *e, int key)
{
    if (key == 27) {
        editor_search_cancel(e);
        return 0;
    }

    if (key == '\n' || key == KEY_ENTER) {
        if (!editor_search_next(e, 1)) {
            editor_set_status(e, "Search: reached end of file");
        }
        return 0;
    }

    if (key == CTRL_KEY('p')) {
        if (!editor_search_next(e, -1)) {
            editor_set_status(e, "Search: reached start of file");
        }
        return 0;
    }

    return 0;
}


/* =========================================================
 * 替换
 * ========================================================= */

void editor_replace_cancel(Editor *e)
{
    e->mode = MODE_NORMAL;
    e->match_row = -1;
    editor_set_status(e, "");
}


void editor_replace_start(Editor *e)
{
    e->mode = MODE_REPLACE_SEARCH;

    e->replace_query_len = 0;
    e->replace_query[0] = '\0';
    e->replace_text_len = 0;
    e->replace_text[0] = '\0';
    e->match_row = -1;

    e->saved_cx = e->cx;
    e->saved_cy = e->cy;
    e->saved_row_offset = e->row_offset;
    e->saved_col_offset = e->col_offset;

    editor_set_status(e, "Replace: type search term, Enter to continue, Esc to cancel");
}


/* 在当前候选处执行替换，并将光标/匹配长度更新到替换后的文本末尾。
 * 注意：此功能不接入 undo/redo 历史（详见文件头说明），如需撤销
 * 替换，可手动用 Ctrl-Z 依次撤销其内部产生的插入/删除动作。 */
void editor_replace_at_match(Editor *e)
{
    if (e->match_row == -1) {
        return;
    }

    Row *row = &e->rows[e->match_row];

    int new_len = row->len - e->match_len + e->replace_text_len;

    row->chars = realloc(row->chars, new_len + 1);

    memmove(
        &row->chars[e->match_start + e->replace_text_len],
        &row->chars[e->match_start + e->match_len],
        row->len - e->match_start - e->match_len + 1
    );

    memcpy(&row->chars[e->match_start], e->replace_text, e->replace_text_len);

    row->len = new_len;

    e->cy = e->match_row;
    e->cx = e->match_start + e->replace_text_len;

    e->match_len = e->replace_text_len;

    e->modified = 1;
}


int editor_process_key_replace_search(Editor *e, int key)
{
    if (key == 27) {
        editor_replace_cancel(e);
        return 0;
    }

    if (key == '\n' || key == KEY_ENTER) {
        if (e->replace_query_len == 0) {
            editor_replace_cancel(e);
            return 0;
        }

        e->mode = MODE_REPLACE_TEXT;
        editor_set_status(e, "Replace: type replacement text, Enter to continue");
        return 0;
    }

    if (key == KEY_BACKSPACE || key == 127 || key == 8) {
        if (e->replace_query_len > 0) {
            e->replace_query_len--;
            e->replace_query[e->replace_query_len] = '\0';
        }
        return 0;
    }

    if (key >= 32 && key <= 126 && e->replace_query_len < SEARCH_QUERY_MAX - 1) {
        e->replace_query[e->replace_query_len++] = (char)key;
        e->replace_query[e->replace_query_len] = '\0';
    }

    return 0;
}


int editor_process_key_replace_text(Editor *e, int key)
{
    if (key == 27) {
        editor_replace_cancel(e);
        return 0;
    }

    if (key == '\n' || key == KEY_ENTER) {
        memcpy(e->search_query, e->replace_query, e->replace_query_len + 1);
        e->search_len = e->replace_query_len;
        e->match_row = -1;

        e->mode = MODE_REPLACE_NAV;

        if (editor_search_next(e, 1)) {
            editor_set_status(e, "Replace: Y=replace  N=skip  A=all  Enter/Ctrl-P=nav  Esc=exit");
        } else {
            editor_set_status(e, "Replace: no match found");
        }
        return 0;
    }

    if (key == KEY_BACKSPACE || key == 127 || key == 8) {
        if (e->replace_text_len > 0) {
            e->replace_text_len--;
            e->replace_text[e->replace_text_len] = '\0';
        }
        return 0;
    }

    if (key >= 32 && key <= 126 && e->replace_text_len < REPLACE_TEXT_MAX - 1) {
        e->replace_text[e->replace_text_len++] = (char)key;
        e->replace_text[e->replace_text_len] = '\0';
    }

    return 0;
}


int editor_process_key_replace_nav(Editor *e, int key)
{
    if (key == 27 || key == CTRL_KEY('c')) {
        editor_replace_cancel(e);
        return 0;
    }

    if (key == '\n' || key == KEY_ENTER) {
        if (!editor_search_next(e, 1)) {
            editor_set_status(e, "Replace: reached end of file");
        }
        return 0;
    }

    if (key == CTRL_KEY('p')) {
        if (!editor_search_next(e, -1)) {
            editor_set_status(e, "Replace: reached start of file");
        }
        return 0;
    }

    if (key == 'y' || key == 'Y') {
        if (e->match_row != -1) {
            editor_replace_at_match(e);
            editor_search_next(e, 1);
        }
        return 0;
    }

    if (key == 'n' || key == 'N') {
        editor_search_next(e, 1);
        return 0;
    }

    if (key == 'a' || key == 'A') {
        int count = 0;

        while (e->match_row != -1) {
            editor_replace_at_match(e);
            count++;

            if (!editor_search_next(e, 1)) {
                break;
            }
        }

        char msg[64];
        snprintf(msg, sizeof(msg), "Replaced %d occurrence(s)", count);
        editor_set_status(e, msg);

        e->mode = MODE_NORMAL;
        e->match_row = -1;
        return 0;
    }

    return 0;
}


/* =========================================================
 * Scroll
 * ========================================================= */

/* 正文可显示的行数：搜索/替换模式下上方多占一行提示，正文区域相应减少 */
int editor_text_rows(Editor *e)
{
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    (void)max_x;

    int rows = max_y - 1;

    if (e->mode != MODE_NORMAL) {
        rows -= 1;
    }

    if (rows < 1) {
        rows = 1;
    }

    return rows;
}


void editor_scroll(Editor *e)
{
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    (void)max_y;

    int text_rows = editor_text_rows(e);

    if (e->cy < e->row_offset) {
        e->row_offset = e->cy;
    }

    if (e->cy >= e->row_offset + text_rows) {
        e->row_offset = e->cy - text_rows + 1;
    }

    if (e->cx < e->col_offset) {
        e->col_offset = e->cx;
    }

    if (e->cx >= e->col_offset + max_x) {
        e->col_offset = e->cx - max_x + 1;
    }
}

/* =========================================================
 * Screen
 * ========================================================= */

void editor_draw_status_bar(Editor *e)
{
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    int status_y = max_y - 1;

    attron(A_REVERSE);

    mvhline(status_y, 0, ' ', max_x);

    const char *mode_tag = "";

    if (e->mode == MODE_SEARCH_INPUT || e->mode == MODE_SEARCH_NAV) {
        mode_tag = " [SEARCH]";
    } else if (e->mode == MODE_REPLACE_SEARCH ||
               e->mode == MODE_REPLACE_TEXT ||
               e->mode == MODE_REPLACE_NAV) {
        mode_tag = " [REPLACE]";
    }

    char left_status[100];

    snprintf(
        left_status,
        sizeof(left_status),
        "dedit - %s%s%s",
        e->filename ? e->filename : "[No Name]",
        e->modified ? " [modified]" : "",
        mode_tag
    );

    mvprintw(
        status_y,
        0,
        "%s",
        left_status
    );

    mvprintw(
        status_y,
        max_x / 2,
        "%s",
        e->status_msg
    );

    char position[50];

    snprintf(
        position,
        sizeof(position),
        " %d:%d ",
        e->cy + 1,
        e->cx + 1
    );

    int position_x = max_x - strlen(position);

    if (position_x > 0) {
        mvprintw(
            status_y,
            position_x,
            "%s",
            position
        );
    }

    attroff(A_REVERSE);
}


/* 绘制搜索/替换模式下，状态栏上方的输入提示行 */
void editor_draw_prompt(Editor *e)
{
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    int prompt_y = max_y - 2;

    mvhline(prompt_y, 0, ' ', max_x);

    char prefix[80];
    char value[400];

    if (e->mode == MODE_SEARCH_INPUT || e->mode == MODE_SEARCH_NAV) {
        snprintf(prefix, sizeof(prefix), "Search: ");
        snprintf(value, sizeof(value), "%.*s", e->search_len, e->search_query);

    } else if (e->mode == MODE_REPLACE_SEARCH) {
        snprintf(prefix, sizeof(prefix), "Replace - search: ");
        snprintf(value, sizeof(value), "%.*s", e->replace_query_len, e->replace_query);

    } else if (e->mode == MODE_REPLACE_TEXT) {
        snprintf(prefix, sizeof(prefix), "Replace with: ");
        snprintf(value, sizeof(value), "%.*s", e->replace_text_len, e->replace_text);

    } else if (e->mode == MODE_REPLACE_NAV) {
        snprintf(prefix, sizeof(prefix), "Replace ");
        snprintf(
            value, sizeof(value),
            "'%.*s' -> '%.*s'  [Y/N/A/Enter/Ctrl-P/Esc]",
            e->replace_query_len, e->replace_query,
            e->replace_text_len, e->replace_text
        );

    } else {
        prefix[0] = '\0';
        value[0] = '\0';
    }

    mvprintw(prompt_y, 0, "%s%s", prefix, value);

    e->prompt_cursor_col = (int)(strlen(prefix) + strlen(value));
}


/* 在某一屏幕行上叠加绘制选区高亮 / 搜索匹配高亮（反色），
 * 使用 mvchgat 只修改已打印字符的属性，不重复输出文本内容 */
void editor_draw_row_overlay(Editor *e, int file_y, int screen_y, int max_x)
{
    if (e->selecting) {
        int sr, sc, er, ec;
        editor_get_selection_range(e, &sr, &sc, &er, &ec);

        if (file_y >= sr && file_y <= er) {
            int start_col = (file_y == sr) ? sc : 0;
            int end_col = (file_y == er) ? ec : e->rows[file_y].len;

            int screen_start = start_col - e->col_offset;
            int screen_end = end_col - e->col_offset;

            if (screen_start < 0) screen_start = 0;
            if (screen_end > max_x) screen_end = max_x;

            if (screen_end > screen_start) {
                mvchgat(screen_y, screen_start, screen_end - screen_start, A_REVERSE, 0, NULL);
            }
        }
    }

    if (e->match_row == file_y) {
        int screen_start = e->match_start - e->col_offset;
        int screen_end = e->match_start + e->match_len - e->col_offset;

        if (screen_start < 0) screen_start = 0;
        if (screen_end > max_x) screen_end = max_x;

        if (screen_end > screen_start) {
            mvchgat(screen_y, screen_start, screen_end - screen_start, A_REVERSE, 0, NULL);
        }
    }
}


void editor_draw(Editor *e)
{
    erase();

    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    editor_scroll(e);

    int text_rows = editor_text_rows(e);

    for (int screen_y = 0; screen_y < text_rows; screen_y++) {
        int file_y = e->row_offset + screen_y;
        if (file_y >= e->num_rows) break;

        Row *row = &e->rows[file_y];

        if (e->col_offset < row->len) {
            mvaddnstr(screen_y, 0,
                      &row->chars[e->col_offset],
                      max_x);
        }

        editor_draw_row_overlay(e, file_y, screen_y, max_x);
    }

    if (e->mode != MODE_NORMAL) {
        editor_draw_prompt(e);
    }

    editor_draw_status_bar(e);

    if (e->mode == MODE_SEARCH_INPUT ||
        e->mode == MODE_REPLACE_SEARCH ||
        e->mode == MODE_REPLACE_TEXT) {
        move(max_y - 2, e->prompt_cursor_col);
    } else {
        int screen_y = e->cy - e->row_offset;
        int screen_x = e->cx - e->col_offset;
        move(screen_y, screen_x);
    }

    refresh();
}

void editor_set_status(Editor *e, const char *msg)
{
    snprintf(
        e->status_msg,
        sizeof(e->status_msg),
        "%s",
        msg
    );
}

int editor_save(Editor *e)
{
    if (!e->filename) return 0;

    FILE *fp = fopen(e->filename, "w");

    if (fp == NULL) {
        return 0;
    }

    for (int i = 0; i < e->num_rows; i++) {
        fwrite(
            e->rows[i].chars,
            1,
            e->rows[i].len,
            fp
        );

        fputc('\n', fp);
    }

    fclose(fp);

    e->modified = 0;

    return 1;
}

int editor_process_key(Editor *e, int key)
{
    /* 搜索 / 替换模式下，键盘输入统一交给对应的子处理函数 */
    if (e->mode == MODE_SEARCH_INPUT) {
        return editor_process_key_search_input(e, key);
    }
    if (e->mode == MODE_SEARCH_NAV) {
        return editor_process_key_search_nav(e, key);
    }
    if (e->mode == MODE_REPLACE_SEARCH) {
        return editor_process_key_replace_search(e, key);
    }
    if (e->mode == MODE_REPLACE_TEXT) {
        return editor_process_key_replace_text(e, key);
    }
    if (e->mode == MODE_REPLACE_NAV) {
        return editor_process_key_replace_nav(e, key);
    }

    /* ---------------- 普通编辑模式 ---------------- */

    if (key == CTRL_KEY('s')) {
        if (editor_save(e)) {
            editor_set_status(e, "File saved successfully.");
        } else {
            editor_set_status(e, "Error saving file!");
        }
        e->quit_times = 2;
        return 0;
    }

    if (key == CTRL_KEY('q')) {

        if (e->modified) {

            if (e->quit_times > 1) {
                editor_set_status(
                    e,
                    "WARNING: Unsaved changes! Press Ctrl-Q again to quit."
                );

                e->quit_times--;
                return 0;
            }
        }

        return 1;
    }

    e->quit_times = 2;

    if (key == CTRL_KEY('f')) {
        editor_find_start(e);
        return 0;
    }

    if (key == CTRL_KEY('r')) {
        editor_replace_start(e);
        return 0;
    }

    if (key == CTRL_KEY('z')) {
        editor_undo(e);
        return 0;
    }

    if (key == CTRL_KEY('y')) {
        editor_redo(e);
        return 0;
    }

    if (key == CTRL_KEY('c')) {
        editor_copy(e);
        return 0;
    }

    if (key == CTRL_KEY('x')) {
        editor_cut(e);
        return 0;
    }

    if (key == CTRL_KEY('v')) {
        editor_paste(e);
        return 0;
    }

    if (key == KEY_SLEFT || key == KEY_SRIGHT || key == KEY_SF || key == KEY_SR) {
        editor_selection_move(e, key);
        return 0;
    }

    if (key >= 32 && key <= 126) {
        e->selecting = 0;
        editor_insert_char(e, key);
    }
    else if (
        key == KEY_UP ||
        key == KEY_DOWN ||
        key == KEY_LEFT ||
        key == KEY_RIGHT
    ) {
        e->selecting = 0;
        editor_move_cursor(e, key);
    }
    else if (
        key == KEY_BACKSPACE ||
        key == 127 ||
        key == 8
    ) {
        e->selecting = 0;
        editor_del_char(e);
    }
    else if (key == KEY_DC) {
        e->selecting = 0;
        editor_delete_char(e);
    }
    else if (
        key == '\n' ||
        key == KEY_ENTER
    ) {
        e->selecting = 0;
        editor_insert_newline(e);
    }

    return 0;
}

/* 修改：保持与函数调用一致名称 */
int parse_key(const char *token)
{
    if (strcmp(token, "<Enter>") == 0) {
        return '\n';
    }

    if (strcmp(token, "<Backspace>") == 0) {
        return KEY_BACKSPACE;
    }

    if (strcmp(token, "<Del>") == 0) {
        return KEY_DC;
    }

    if (strcmp(token, "<Ctrl-S>") == 0) {
        return CTRL_KEY('s');
    }

    if (strcmp(token, "<Ctrl-Q>") == 0) {
        return CTRL_KEY('q');
    }

    if (strcmp(token, "<Esc>") == 0) {
        return 27;
    }

    if (strcmp(token, "<Up>") == 0) {
        return KEY_UP;
    }

    if (strcmp(token, "<Down>") == 0) {
        return KEY_DOWN;
    }

    if (strcmp(token, "<Left>") == 0) {
        return KEY_LEFT;
    }

    if (strcmp(token, "<Right>") == 0) {
        return KEY_RIGHT;
    }

    if (strcmp(token, "<Ctrl-F>") == 0) {
        return CTRL_KEY('f');
    }

    if (strcmp(token, "<Ctrl-R>") == 0) {
        return CTRL_KEY('r');
    }

    if (strcmp(token, "<Ctrl-Z>") == 0) {
        return CTRL_KEY('z');
    }

    if (strcmp(token, "<Ctrl-Y>") == 0) {
        return CTRL_KEY('y');
    }

    if (strcmp(token, "<Ctrl-C>") == 0) {
        return CTRL_KEY('c');
    }

    if (strcmp(token, "<Ctrl-X>") == 0) {
        return CTRL_KEY('x');
    }

    if (strcmp(token, "<Ctrl-V>") == 0) {
        return CTRL_KEY('v');
    }

    if (strcmp(token, "<Ctrl-P>") == 0) {
        return CTRL_KEY('p');
    }

    if (strcmp(token, "<Shift-Left>") == 0) {
        return KEY_SLEFT;
    }

    if (strcmp(token, "<Shift-Right>") == 0) {
        return KEY_SRIGHT;
    }

    if (strcmp(token, "<Shift-Up>") == 0) {
        return KEY_SR;
    }

    if (strcmp(token, "<Shift-Down>") == 0) {
        return KEY_SF;
    }

    if (strlen(token) == 1) {
        return (unsigned char)token[0];
    }

    return -1;
}

int run_test_input(Editor *e, const char *input)
{
    int len = strlen(input);

    for (int i = 0; i < len; ) {

        if (input[i] == '<') {

            const char *end = strchr(&input[i], '>');

            if (end == NULL) {
                return 0;
            }

            int token_len = end - &input[i] + 1;

            char token[50];

            if (token_len >= (int)sizeof(token)) {
                return 0;
            }

            memcpy(token, &input[i], token_len);
            token[token_len] = '\0';

            int key = parse_key(token);

            if (key == -1) {
                return 0;
            }

            editor_process_key(e, key);

            i += token_len;
        }
        else {
            editor_process_key(e, input[i]);

            i++;
        }
    }

    return 1;
}

int run_test_file(const char *filename)
{
    FILE *fp = fopen(filename, "r");

    if (fp == NULL) {
        printf("[FAIL] cannot open %s\n", filename);
        return 0;
    }

    char input[4096];

    if (fgets(input, sizeof(input), fp) == NULL) {
        fclose(fp);

        printf("[FAIL] empty test: %s\n", filename);
        return 0;
    }

    fclose(fp);

    input[strcspn(input, "\r\n")] = '\0';

    Editor e;
    editor_init(&e);

    e.filename = strdup("test_output.txt");

    /* 修复：移除了多余的参数 */
    editor_insert_row(&e, "", 0);

    int ok = run_test_input(&e, input);

    if (!ok) {
        printf("[FAIL] invalid test input: %s\n", filename);
        editor_free(&e);
        return 0;
    }

    editor_free(&e);

    printf("[PASS] %s\n", filename);

    return 1;
}

void run_basic_test(void)
{
    Editor e;
    editor_init(&e);

    e.filename = strdup("test_output.txt");

    editor_process_key(&e, 'h');
    editor_process_key(&e, 'e');
    editor_process_key(&e, 'l');
    editor_process_key(&e, 'l');
    editor_process_key(&e, 'o');

    editor_process_key(&e, '\n');

    editor_process_key(&e, 'w');
    editor_process_key(&e, 'o');
    editor_process_key(&e, 'r');
    editor_process_key(&e, 'l');
    editor_process_key(&e, 'd');

    if (
        e.num_rows == 2 &&
        strcmp(e.rows[0].chars, "hello") == 0 &&
        strcmp(e.rows[1].chars, "world") == 0
    ) {
        printf("[PASS] basic typing\n");
    }
    else {
        printf("[FAIL] basic typing\n");
    }

    editor_free(&e);
}

void run_undo_redo_test(void)
{
    Editor e;
    editor_init(&e);
    e.filename = strdup("test_output.txt");

    run_test_input(&e, "hello");
    run_test_input(&e, "<Ctrl-Z><Ctrl-Z>");

    int ok1 = (e.num_rows == 1 && strcmp(e.rows[0].chars, "hel") == 0);

    run_test_input(&e, "<Ctrl-Y><Ctrl-Y>");

    int ok2 = (e.num_rows == 1 && strcmp(e.rows[0].chars, "hello") == 0);

    if (ok1 && ok2) {
        printf("[PASS] undo/redo char insert\n");
    } else {
        printf("[FAIL] undo/redo char insert\n");
    }

    editor_free(&e);
}

void run_search_test(void)
{
    Editor e;
    editor_init(&e);
    e.filename = strdup("test_output.txt");

    run_test_input(&e, "foo bar foo baz");

    /* 搜索是从光标位置开始向后找（不回绕），把光标移回行首才能找到全部候选 */
    e.cy = 0;
    e.cx = 0;

    run_test_input(&e, "<Ctrl-F>foo<Enter>");

    int ok1 = (e.match_row == 0 && e.match_start == 0);

    run_test_input(&e, "<Enter>");

    int ok2 = (e.match_row == 0 && e.match_start == 8);

    run_test_input(&e, "<Enter>");

    /* 不回绕：应仍停留在最后一个候选 */
    int ok3 = (e.match_row == 0 && e.match_start == 8);

    run_test_input(&e, "<Esc>");

    int ok4 = (e.mode == MODE_NORMAL && e.match_row == -1);

    if (ok1 && ok2 && ok3 && ok4) {
        printf("[PASS] search find/next/no-wrap/cancel\n");
    } else {
        printf("[FAIL] search find/next/no-wrap/cancel\n");
    }

    editor_free(&e);
}

void run_replace_test(void)
{
    Editor e;
    editor_init(&e);
    e.filename = strdup("test_output.txt");

    run_test_input(&e, "foo bar foo baz foo");
    e.cy = 0;
    e.cx = 0;

    /* 搜索 foo -> 替换为 X；第一个候选 Y 替换，第二个候选 N 跳过，
     * 剩下的用 A 全部替换 -> 结果应为 "X bar foo baz X" */
    run_test_input(&e, "<Ctrl-R>foo<Enter>X<Enter>Y<Enter>N<Enter>A");

    int ok = (e.num_rows == 1 && strcmp(e.rows[0].chars, "X bar foo baz X") == 0);

    if (ok) {
        printf("[PASS] replace Y/N/A flow\n");
    } else {
        printf("[FAIL] replace Y/N/A flow (got: \"%s\")\n",
               e.num_rows > 0 ? e.rows[0].chars : "(empty)");
    }

    editor_free(&e);
}

void run_clipboard_undo_test(void)
{
    Editor e;
    editor_init(&e);
    e.filename = strdup("test_output.txt");

    run_test_input(&e, "hello world");

    /* 选中 "hello"（第0到第5列），剪切，再粘贴到行尾 */
    e.cy = 0; e.cx = 0;
    run_test_input(&e, "<Shift-Right><Shift-Right><Shift-Right><Shift-Right><Shift-Right>");
    run_test_input(&e, "<Ctrl-X>");

    int ok1 = (e.num_rows == 1 && strcmp(e.rows[0].chars, " world") == 0);
    int ok2 = (e.clipboard_len == 5 && strncmp(e.clipboard, "hello", 5) == 0);

    /* 撤销剪切，文本应恢复 */
    run_test_input(&e, "<Ctrl-Z>");
    int ok3 = (e.num_rows == 1 && strcmp(e.rows[0].chars, "hello world") == 0);

    /* 重做剪切，文本应再次被剪掉 */
    run_test_input(&e, "<Ctrl-Y>");
    int ok4 = (e.num_rows == 1 && strcmp(e.rows[0].chars, " world") == 0);

    /* 移动到行尾，粘贴回去 */
    e.cy = 0; e.cx = e.rows[0].len;
    run_test_input(&e, "<Ctrl-V>");
    int ok5 = (e.num_rows == 1 && strcmp(e.rows[0].chars, " worldhello") == 0);

    if (ok1 && ok2 && ok3 && ok4 && ok5) {
        printf("[PASS] selection cut/paste + undo/redo\n");
    } else {
        printf("[FAIL] selection cut/paste + undo/redo (got: \"%s\")\n",
               e.num_rows > 0 ? e.rows[0].chars : "(empty)");
    }

    editor_free(&e);
}

/* =========================================================
 * Main
 * ========================================================= */

int main(int argc, char *argv[])
{
    // 测试模式
    if (argc >= 2 && strcmp(argv[1], "--test") == 0) {
        run_basic_test();
        run_undo_redo_test();
        run_search_test();
        run_replace_test();
        run_clipboard_undo_test();
        return 0;
    }

    // 测试指定测试用例文件
    if (argc >= 3 && strcmp(argv[1], "--test-file") == 0) {
        run_test_file(argv[2]);
        return 0;
    }

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <filename>\n", argv[0]);
        return 1;
    }

    Editor editor;
    editor_init(&editor);

    editor.filename = strdup(argv[1]);

    editor_open(&editor, argv[1]);

    // 初始化终端屏幕
    initscr();
    raw();
    noecho();
    keypad(stdscr, TRUE);
    start_color();

    // 设置初始状态栏提示
    editor_set_status(&editor, "HELP: Ctrl-S=save Ctrl-Q=quit Ctrl-F=find Ctrl-R=replace Ctrl-Z/Y=undo/redo");

    // 主交互循环
    while (1) {
        editor_draw(&editor);

        int key = getch();

        if (editor_process_key(&editor, key)) {
            break;
        }
    }

    // 恢复终端状态并释放资源
    endwin();
    editor_free(&editor);

    return 0;
}
