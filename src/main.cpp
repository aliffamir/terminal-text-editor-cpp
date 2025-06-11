#include <iostream>
#include <termios.h>
#include <unistd.h>

void enableRawMode()
{
    struct termios raw;

    tcgetattr(STDIN_FILENO, &raw);

    raw.c_lflag &= ~(ECHO);

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

int main()
{
    enableRawMode();
    
    std::cout << "hello world\n";
    char c;
    while (std::cin >> c && c != 'q')
        ;
    return 0;
}