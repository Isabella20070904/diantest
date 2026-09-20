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

    // 光标在“文本”中的位置
    int cx;
    int cy;
} Editor;


/* ==================== Row ==================== */

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


/* ==================== File ==================== */

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
}


/* ==================== Cursor ==================== */

void editor_move_cursor(Editor *e, int key)
{
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

        if (e->cy < e->num_rows &&
            e->cx < e->rows[e->cy].len) {
            e->cx++;
        }
    }

    /*
     * 上下移动后，当前行可能比原来的 x 短。
     * 所以要把 cx 限制在当前行末尾。
     */
    if (e->num_rows > 0 &&
        e->cy >= 0 &&
        e->cy < e->num_rows &&
        e->cx > e->rows[e->cy].len) {

        e->cx = e->rows[e->cy].len;
    }
}


/* ==================== Screen ==================== */

void editor_draw(Editor *e)
{
    erase();

    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    for (int i = 0; i < e->num_rows && i < max_y; i++) {

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


/* ==================== Main ==================== */

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

        if (key == KEY_UP ||
            key == KEY_DOWN ||
            key == KEY_LEFT ||
            key == KEY_RIGHT) {

            editor_move_cursor(&editor, key);
        }
    }

    endwin();

    editor_free(&editor);

    return 0;
}