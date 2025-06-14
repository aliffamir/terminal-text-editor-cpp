#include <cctype>
#include <cstdio>
#include <errno.h>
#include <iostream>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define CTRL_KEY(k) ((k) & 0x1f)

struct editorConfig
{
    int screenrows;
    int screencols;
    termios original_termios;
};

editorConfig E;

void die(const char* s)
{
    perror(s);
    exit(1);
}

/* terminal */
void disableRawMode()
{
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.original_termios) == -1)
        die("tcsetattr");
}
void enableRawMode()
{

    if (tcgetattr(STDIN_FILENO, &E.original_termios) == -1)
        die("tcgetattr");

    atexit(disableRawMode);

    struct termios raw = E.original_termios;
    raw.c_iflag &= ~(BRKINT | INPCK | ISTRIP | ICRNL | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
        die("tcsetattr");
}

char editorReadKey()
{
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1)
    {
        if (nread == -1 && errno != EAGAIN)
            die("read");
    }
    return c;
}

int getCursorPosition(int& rows, int& cols)
{
    std::string buf;
    buf.reserve(32);

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
        return -1;

    unsigned int i;
    while (i < sizeof(buf) - 1)
    {
        if (read(STDIN_FILENO, &buf[i], 1) != 1)
            break;
        if (buf[i] == 'R')
            break;
        ++i;
    }
    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[')
        return -1;
    if (std::sscanf(&buf[2], "%d;%d", &rows, &cols) != 2)
        return -1;

    return 0;
}

int getWindowSize(int& rows, int& cols)
{
    winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)
    {
        if (write(STDOUT_FILENO, "\x1b[99C\x1b[999B", 12) != 12)
            return -1;
        return getCursorPosition(rows, cols);
    }
    else
    {
        cols = ws.ws_col;
        rows = ws.ws_row;
        return 0;
    }
}
/* output */
void editorDrawRows(std::string& buffer)
{
    for (int y{0}; y < E.screenrows; ++y)
    {
        // write(STDOUT_FILENO, "~", 1);
        buffer.append(1, '~');

        if (y < E.screenrows - 1)
        {
            // write(STDOUT_FILENO, "\r\n", 2);
            buffer.append("\r\n", 2);
        }
    }
}

void editorRefreshScreen()
{
    std::string buffer;

    // hide cursor
    buffer.append("\x1b[?25l", 6);
    buffer.append("\x1b[2J", 4);
    buffer.append("\x1b[H", 3);

    editorDrawRows(buffer);

    buffer.append("\x1b[H", 3);
    // display cursor
    buffer.append("\x1b[?25h", 6);

    write(STDOUT_FILENO, buffer.c_str(), buffer.size());
}

/* input */
void editorProcessKeypress()
{
    char c = editorReadKey();

    switch (c)
    {
    case CTRL_KEY('q'):
        write(STDOUT_FILENO, "\x1b[2J", 4);
        write(STDOUT_FILENO, "\x1b[H", 3);
        exit(0);
        break;
    }
}

void initEditor()
{
    if (getWindowSize(E.screenrows, E.screencols) == -1)
        die("getWindowSize");
}

int main()
{
    enableRawMode();
    initEditor();

    while (1)
    {
        editorRefreshScreen();
        editorProcessKeypress();
    }
    return 0;
}