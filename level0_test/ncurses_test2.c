#include <ncurses.h>

int main()
{
initscr();
keypad(stdscr,TRUE);
noecho();
curs_set(0);

int x=10;
int y=5;

int rows,cols;
getmaxyx(stdscr,rows,cols);


while(1){
clear();
mvprintw(y,x,".");
mvprintw(0,0,"position%d,%d",x,y);
refresh();
int ch = getch();

if (ch=='q'){
break;
}
else if(ch==KEY_UP){
if(y>0)
y--;
}
else if(ch==KEY_DOWN){ 
if(y<rows-1)
y++;
}
else if(ch==KEY_LEFT){ 
if(x>0)
x--;
}
else if(ch==KEY_RIGHT){ 
if(x<cols-1)
x++;
}
}
endwin();
return 0;
}

