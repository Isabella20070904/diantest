#include <ncurses.h>

int main(void) {
    initscr();
    mvprintw(5,10,"hello world");
    mvprintw(10,20,"i am learning");

    refresh();
    getch();
    endwin();
    return 0;
}
