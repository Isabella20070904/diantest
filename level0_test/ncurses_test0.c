#include<ncurses.h>

int main (){
initscr();
printw("press any key");
refresh();
int ch =getch();
endwin();
printf("You pressed %c\n",ch);
return 0;
}
