#include <iostream>
#include <termios.h>
#include <unistd.h>
#include <cctype>

struct termios original_termios;

void disableRawMode()
{
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_termios);
}
void enableRawMode()
{

    tcgetattr(STDIN_FILENO, &original_termios);
    atexit(disableRawMode);

    struct termios raw = original_termios;
    raw.c_lflag &= ~(ECHO | ICANON);

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

int main()
{
    enableRawMode();

    std::cout << "hello world\n";
    char c;
    while (std::cin >> c && c != 'q')
    {
        
    }
    return 0;
}