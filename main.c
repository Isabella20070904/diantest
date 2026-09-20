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

    // 光标在文本中的位置
    int cx;
    int cy;

    // 文件是否被修改
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
    /*
     * 如果文件是空的，先创建第一行。
     */
    if (e->num_rows == 0) {
        editor_insert_row(e, "", 0);
    }

    Row *row = &e->rows[e->cy];

    /*
     * 给这一行多申请一个字符的位置。
     */
    row->chars = realloc(
        row->chars,
        row->len + 2
    );

    /*
     * 把 cx 后面的内容整体向右移动一格。
     *
     * 例如：
     *
     * hello
     *   ↑
     *
     * 插入 X：
     *
     * heXllo
     */
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
 * Delete Character
 * ========================================================= */

void editor_del_char(Editor *e)
{
    if (e->num_rows == 0) {
        return;
    }

    Row *row = &e->rows[e->cy];

    /*
     * 光标在行首时不能再删除左边的字符。
     */
    if (e->cx == 0) {
        return;
    }

    /*
     * 删除光标左边的字符。
     *
     * 例如：
     *
     * heXllo
     *    ↑
     *
     * Backspace 后：
     *
     * hello
     *   ↑
     */
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
}


/* =========================================================
 * Delete Key
 * ========================================================= */

void editor_delete_char(Editor *e)
{
    if (e->num_rows == 0) {
        return;
    }

    Row *row = &e->rows[e->cy];

    /*
     * 如果光标已经在行尾，
     * 当前这一版暂时什么都不做。
     *
     * 后面的 Enter / 行合并会解决这个问题。
     */
    if (e->cx >= row->len) {
        return;
    }

    /*
     * 删除光标右边的字符。
     *
     * 例如：
     *
     * he|llo
     *
     * Delete：
     *
     * he|lo
     */
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
        }

    } else if (key == KEY_RIGHT) {

        if (e->cx < e->rows[e->cy].len) {
            e->cx++;
        }
    }

    /*
     * 上下移动以后，
     * 防止 cx 超过当前行长度。
     */
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

        /*
         * 普通字符
         */
        if (key >= 32 && key <= 126) {
            editor_insert_char(&editor, key);
        }

        /*
         * 光标移动
         */
        else if (key == KEY_UP ||
                 key == KEY_DOWN ||
                 key == KEY_LEFT ||
                 key == KEY_RIGHT) {

            editor_move_cursor(&editor, key);
        }

        /*
         * Backspace
         */
        else if (key == KEY_BACKSPACE ||
                 key == 127 ||
                 key == 8) {

            editor_del_char(&editor);
        }

        /*
         * Delete
         */
        else if (key == KEY_DC) {

            editor_delete_char(&editor);
        }
    }

    endwin();

    editor_free(&editor);

    return 0;
}