#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define CTRL_KEY(k) ((k) & 0x1f)

/* 渲染文件内容到 ncurses 屏幕 */
void render_file(FILE *fp) {
    char line[1024];
    int row = 0;
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    /* 逐行读取文件内容 */
    while (fgets(line, sizeof(line), fp) != NULL && row < max_y) {
        /* 清除行末换行符 */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[len - 1] = '\0';
            len--;
        }
        
        /* 关键改动片段：使用 mvaddnstr 渲染屏幕允许的最大宽度，防止截断报错 */
        mvaddnstr(row, 0, line, max_x);
        row++;
    }
}

int main(int argc, char *argv[]) {
    /* 1. 命令行参数检查 */
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <filename>\n", argv[0]);
        return 1;
    }

    /* 2. 初始化 ncurses */
    initscr();
    raw();
    noecho();
    keypad(stdscr, TRUE);

    /* 关键改动片段：尝试打开文件，失败时在 ncurses 界面优雅提示 */
    FILE *fp = fopen(argv[1], "r");
    if (!fp) {
        printw("Error: Cannot open file '%s'. Press any key to exit...", argv[1]);
        refresh();
        getch();
        endwin();
        return 1;
    }

    /* 关键改动片段：读取并渲染文件 */
    render_file(fp);
    fclose(fp);

    int y = 0, x = 0;
    int max_y = 0, max_x = 0;
    int ch;

    while (1) {
        getmaxyx(stdscr, max_y, max_x);

        if (y >= max_y) y = max_y - 1;
        if (x >= max_x) x = max_x - 1;

        move(y, x);
        refresh();

        ch = getch();

        if (ch == CTRL_KEY('q')) {
            break;
        }

        switch (ch) {
            case KEY_UP:
                if (y > 0) y--;
                break;
            case KEY_DOWN:
                if (y < max_y - 1) y++;
                break;
            case KEY_LEFT:
                if (x > 0) x--;
                break;
            case KEY_RIGHT:
                if (x < max_x - 1) x++;
                break;
            default:
                if (isprint(ch)) {
                    addch(ch);
                    if (x < max_x - 1) x++;
                }
                break;
        }
    }

    endwin();
    return 0;
}