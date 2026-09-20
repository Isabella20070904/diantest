#define _GNU_SOURCE

#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CTRL_KEY(k) ((k) & 0x1f)

typedef struct {
    char *chars;
    int len;
} Row;

typedef struct {
    Row *rows;
    int num_rows;

    int cx;
    int cy;

    int modified;
} Editor;


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

    /*
     * 把后面的 Row 往前移动。
     */
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
}


/* =========================================================
 * Newline
 * ========================================================= */

void editor_insert_newline(Editor *e)
{
    if (e->num_rows == 0) {
        editor_insert_row(e, "", 0);
    }

    Row *row = &e->rows[e->cy];

    /*
     * 当前行：
     *
     * hello world
     *      ^
     *
     * cx = 5
     *
     * 拆成：
     *
     * hello
     *  world
     */

    int left_len = e->cx;
    int right_len = row->len - e->cx;

    /*
     * 先保存右半部分。
     */
    char *right = malloc(right_len + 1);

    memcpy(
        right,
        &row->chars[e->cx],
        right_len
    );

    right[right_len] = '\0';

    /*
     * 当前行只保留左半部分。
     */
    row->chars[left_len] = '\0';
    row->len = left_len;

    /*
     * 新增下一行。
     */
    e->rows = realloc(
        e->rows,
        sizeof(Row) * (e->num_rows + 1)
    );

    /*
     * 后面的 Row 整体向后移动。
     */
    memmove(
        &e->rows[e->cy + 2],
        &e->rows[e->cy + 1],
        sizeof(Row) * (e->num_rows - e->cy - 1)
    );

    /*
     * 新行就是当前行的下一行。
     */
    e->rows[e->cy + 1].chars = right;
    e->rows[e->cy + 1].len = right_len;

    e->num_rows++;

    /*
     * 光标移动到新行开头。
     */
    e->cy++;
    e->cx = 0;

    e->modified = 1;
}


/* =========================================================
 * Merge Rows
 * ========================================================= */

void editor_merge_rows(Editor *e, int at)
{
    /*
     * 合并：
     *
     * rows[at]
     * rows[at + 1]
     *
     * 成：
     *
     * rows[at]
     */

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
 * Backspace
 * ========================================================= */

void editor_del_char(Editor *e)
{
    if (e->num_rows == 0) {
        return;
    }

    Row *row = &e->rows[e->cy];

    /*
     * 如果不在行首，删除左边字符。
     */
    if (e->cx > 0) {

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

        return;
    }

    /*
     * 如果在第一行行首，没有东西可以删除。
     */
    if (e->cy == 0) {
        return;
    }

    /*
     * 行首 Backspace：
     *
     * hello
     * |
     * world
     *
     * 变成：
     *
     * hello|world
     */

    int previous_len = e->rows[e->cy - 1].len;

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

    /*
     * 行内 Delete。
     */
    if (e->cx < row->len) {

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

        return;
    }

    /*
     * 光标在行尾：
     *
     * hello|
     * world
     *
     * 合并成：
     *
     * hello|world
     */

    if (e->cy < e->num_rows - 1) {

        editor_merge_rows(e, e->cy);

        e->modified = 1;
    }
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

            /*
             * 左移到上一行行尾。
             */
            e->cy--;
            e->cx = e->rows[e->cy].len;
        }

    } else if (key == KEY_RIGHT) {

        if (e->cx < e->rows[e->cy].len) {
            e->cx++;

        } else if (e->cy < e->num_rows - 1) {

            /*
             * 右移到下一行开头。
             */
            e->cy++;
            e->cx = 0;
        }
    }

    if (e->cx > e->rows[e->cy].len) {
        e->cx = e->rows[e->cy].len;
    }
}


/* =========================================================
 * Screen
 * ========================================================= */

void editor_draw(Editor *e)
{
    erase();

    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    for (int i = 0;
         i < e->num_rows && i < max_y;
         i++) {

        mvaddnstr(
            i,
            0,
            e->rows[i].chars,
            max_x
        );
    }

    move(e->cy, e->cx);

    refresh();
}


/* =========================================================
 * Main
 * ========================================================= */

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <filename>\n", argv[0]);
        return 1;
    }

    Editor editor = {0};

    editor_open(&editor, argv[1]);

    initscr();
    raw();
    noecho();
    keypad(stdscr, TRUE);

    while (1) {

        editor_draw(&editor);

        int key = getch();

        if (key == CTRL_KEY('q')) {
            break;
        }

        if (key >= 32 && key <= 126) {

            editor_insert_char(&editor, key);

        } else if (key == KEY_UP ||
                   key == KEY_DOWN ||
                   key == KEY_LEFT ||
                   key == KEY_RIGHT) {

            editor_move_cursor(&editor, key);

        } else if (key == KEY_BACKSPACE ||
                   key == 127 ||
                   key == 8) {

            editor_del_char(&editor);

        } else if (key == KEY_DC) {

            editor_delete_char(&editor);

        } else if (key == '\n' ||
                   key == KEY_ENTER) {

            editor_insert_newline(&editor);
        }
    }

    endwin();

    editor_free(&editor);

    return 0;
}