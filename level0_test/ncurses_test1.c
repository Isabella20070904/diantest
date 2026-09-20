#include <ncurses.h>
int main(){
initscr();
keypad(stdscr,TRUE);
printw("press an arrow key");
refresh();
int ch=getch();
if (ch==KEY_UP){
printw("You pressed up");
}
if (ch==KEY_DOWN){ 
printw("You pressed DOWN");
}
refresh();
getch();
endwin();
return 0;
}
