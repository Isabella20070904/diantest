#include <ncurses.h>

int main()
{
    initscr();

    WINDOW *win;

    win = newwin(10, 30, 5, 10);

    box(win, 0, 0);

    mvwprintw(win, 2, 5, "Hello Window!");

    wrefresh(win);

    wgetch(win);

    delwin(win);

    endwin();

    return 0;
}
