#include <ncurses.h>
#include <ctype.h>

/* 定义 Ctrl 键组合宏：将字符与 0x1f 按位与，得到对应的控制字符 ASCII 码 */
#define CTRL_KEY(k) ((k) & 0x1f)

int main(void) {
    /* 1. 初始化 ncurses */
    initscr();
    cbreak();             /* 禁用行缓冲 */
    noecho();             /* 禁用自动回显 */
    keypad(stdscr, TRUE); /* 开启功能键支持 */

    int y = 0, x = 0;     /* 记录光标位置 */
    int max_y = 0, max_x = 0;
    int ch;

    while (1) {
        /* 关键改动片段：获取当前窗口的边界最大值 */
        getmaxyx(stdscr, max_y, max_x);

        /* 边界检查：确保窗口缩小时光标不会越界 */
        if (y >= max_y) y = max_y - 1;
        if (x >= max_x) x = max_x - 1;

        move(y, x);       /* 移动光标到指定位置 */
        refresh();        /* 刷新屏幕 */

        ch = getch();     /* 读取键盘输入 */

        /* 关键改动片段：按键处理逻辑，使用 Ctrl-Q 退出，移除 Esc 退出 */
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
                    /* 打印字符后光标右移，但不能超过屏幕右边界 */
                    if (x < max_x - 1) {
                        x++;
                    }
                }
                break;
        }
    }

    /* 2. 正确恢复终端状态 */
    endwin();
    return 0;
}