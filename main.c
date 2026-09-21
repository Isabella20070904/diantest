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

    int row_offset;
    int col_offset;

    int modified;

    char *filename;

    char status_msg[80];
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
 * Backspace
 * ========================================================= */

void editor_del_char(Editor *e)
{
    if (e->num_rows == 0) {
        return;
    }

    Row *row = &e->rows[e->cy];

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

    if (e->cy == 0) {
        return;
    }

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
 * Scroll
 * ========================================================= */

void editor_scroll(Editor *e)
{
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    int text_rows = max_y - 1;

    if (text_rows < 1) {
        text_rows = 1;
    }

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

    char left_status[100];

    snprintf(
        left_status,
        sizeof(left_status),
        "dedit - %s%s",
        e->filename,
        e->modified ? " [modified]" : ""
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

void editor_draw(Editor *e)
{
    erase();

    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    editor_scroll(e);

    for (int screen_y = 0; screen_y < max_y - 1; screen_y++) {
        int file_y = e->row_offset + screen_y;
        if (file_y >= e->num_rows) break;

        Row *row = &e->rows[file_y];

        if (e->col_offset < row->len) {
            mvaddnstr(screen_y, 0,
                      &row->chars[e->col_offset],
                      max_x);
        }
    }

    editor_draw_status_bar(e);   // 在这里调用

    int screen_y = e->cy - e->row_offset;
    int screen_x = e->cx - e->col_offset;
    move(screen_y, screen_x);

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

    editor.filename = strdup(argv[1]);

    editor_open(&editor, argv[1]);

    initscr();

    raw();
    noecho();
    keypad(stdscr, TRUE);

    int quit_times = 2;

    while (1) {
    editor_draw(&editor);

    int key = getch();

    if (key == CTRL_KEY('s')) {
        editor_save(&editor);
    }
    else if (key == CTRL_KEY('q')) {

        if (editor.modified) {

            if (quit_times > 1) {
                editor_set_status(
                    &editor,
                    "WARNING: Unsaved changes! Press Ctrl-Q again to quit."
                );

                quit_times--;
                continue;
            }
        }

        break;
    }
    else if (key >= 32 && key <= 126) {
        editor_insert_char(&editor, key);
        quit_times = 2;
    }
    else if (
        key == KEY_UP ||
        key == KEY_DOWN ||
        key == KEY_LEFT ||
        key == KEY_RIGHT
    ) {
        editor_move_cursor(&editor, key);
        quit_times = 2;
    }
    else if (
        key == KEY_BACKSPACE ||
        key == 127 ||
        key == 8
    ) {
        editor_del_char(&editor);
        quit_times = 2;
    }
    else if (key == KEY_DC) {
        editor_delete_char(&editor);
        quit_times = 2;
    }
    else if (
        key == '\n' ||
        key == KEY_ENTER
    ) {
        editor_insert_newline(&editor);
        quit_times = 2;
    }
}
    
    endwin();

    editor_free(&editor);

    return 0;
}