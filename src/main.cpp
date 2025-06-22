#include <algorithm>
#include <cctype>
#include <cstdio>
#include <errno.h>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <vector>

#define KILO_VERSION "0.0.1"
#define CTRL_KEY(k) ((k) & 0x1f)
#define KILO_TAB_STOP 8

enum EditorKey
{
  ARROW_UP = 1000,
  ARROW_RIGHT,
  ARROW_DOWN,
  ARROW_LEFT,
  DEL_KEY,
  HOME_KEY,
  END_KEY,
  PAGE_UP,
  PAGE_DOWN,
};

struct erow
{
  std::string chars;
  std::string render;
};

struct editorConfig
{
  int cursorX, cursorY;
  int renderX;
  int rowoffset;
  int coloffset;
  int screenrows;
  int screencols;
  int numrows;
  std::vector<erow> row;
  std::string filename;
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

  termios raw = E.original_termios;
  raw.c_iflag &= ~(BRKINT | INPCK | ISTRIP | ICRNL | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag |= (CS8);
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
    die("tcsetattr");
}

int editorReadKey()
{
  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1)
  {
    if (nread == -1 && errno != EAGAIN)
      die("read");
  }

  if (c == '\x1b')
  {
    char seq[3];
    if (read(STDIN_FILENO, &seq[0], 1) != 1)
      return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1)
      return '\x1b';
    if (seq[0] == '[')
    {
      if (seq[1] >= '0' && seq[1] <= '9')
      {
        if (read(STDIN_FILENO, &seq[2], 1) != 1)
          return '\x1b';
        if (seq[2] == '~')
        {
          switch (seq[1])
          {
          case '1':
            return HOME_KEY;
          case '3':
            return DEL_KEY;
          case '4':
            return END_KEY;
          case '5':
            return PAGE_UP;
          case '6':
            return PAGE_DOWN;
          case '7':
            return HOME_KEY;
          case '8':
            return END_KEY;
          }
        }
      }
      else
      {
        switch (seq[1])
        {
        case 'A':
          return ARROW_UP;
        case 'B':
          return ARROW_DOWN;
        case 'C':
          return ARROW_RIGHT;
        case 'D':
          return ARROW_LEFT;
        case 'H':
          return HOME_KEY;
        case 'F':
          return END_KEY;
        }
      }
    }
    else if (seq[0] == 'O')
    {
      switch (seq[1])
      {
      case 'H':
        return HOME_KEY;
      case 'F':
        return END_KEY;
      }
    }
    return '\x1b';
  }
  else
  {
    return c;
  }
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

/* helpers */

// replaces a char to a specified string in place, str is an INOUT param
std::string replaceAll(char from, std::string_view to, std::string& str)
{
  std::string newString;
  for (char& ch : str)
  {
    if (ch == from)
    {
      newString += to;
    }
    else
    {
      newString += ch;
    }
  }
  return newString;
}

/* row operations */

int editorRowCxToRx(erow& row, int cursorX)
{
  int renderX = 0;
  for (int j{0}; j < cursorX && j < static_cast<int>(row.chars.size()); ++j)
  {
    if (row.chars[j] == '\t')
    {
      renderX += (KILO_TAB_STOP - 1) - (renderX & KILO_TAB_STOP);
    }
    renderX++;
  }
  return renderX;
}

void editorUpdateRow(erow& row)
{
  row.render.clear();
  // row.render = replaceAll('\t', std::string{KILO_TAB_STOP, ' '}, row.chars);
  std::string result;
  int idx{0};
  for (const char ch : row.chars)
  {
    if (ch == '\t')
    {
      result += ' ';
      ++idx;

      // fill with spaces until we reach the next tab stop
      while (idx % KILO_TAB_STOP != 0)
      {
        result += ' ';
        ++idx;
      }
    }
    else
    {
      result += ch;
      ++idx;
    }
  }

  row.render = std::move(result);
}

void editorAppendRow(std::string_view line)
{
  E.row.emplace_back(erow{static_cast<std::string>(line), ""});
  editorUpdateRow(E.row[E.numrows]);
  E.numrows++;
}

/* file i/o */
void editorOpen(char* filename)
{
  E.filename = filename;
  std::ifstream infile;
  infile.open(filename);
  if (!infile.is_open())
  {
    die("fs.open");
  }

  for (std::string line; std::getline(infile, line);)
  {
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
    {
      line.pop_back();
    }

    editorAppendRow(line);
  }
  infile.close();
}

/* output */
void editorScroll()
{
  E.renderX = E.cursorX;

  if (E.cursorY < E.numrows)
  {
    E.renderX = editorRowCxToRx(E.row[E.cursorY], E.cursorX);
  }
  if (E.cursorY < E.rowoffset)
  {
    E.rowoffset = E.cursorY;
  }
  if (E.cursorY >= E.rowoffset + E.screenrows)
  {
    E.rowoffset = E.cursorY - E.screenrows + 1;
  }

  if (E.renderX < E.coloffset)
  {
    E.coloffset = E.renderX;
  }
  if (E.renderX >= E.coloffset + E.screencols)
  {
    E.coloffset = E.renderX - E.coloffset + 1;
  }
}

void editorDrawRows(std::string& buffer)
{
  for (int y{0}; y < E.screenrows; ++y)
  {
    int filerow = y + E.rowoffset;
    if (filerow >= E.numrows)
    {
      if (E.numrows == 0 && y == E.screenrows / 3)
      {
        std::string welcome = "Kilo editor -- version " + static_cast<std::string>(KILO_VERSION);
        int welcomeLength = std::min(static_cast<int>(welcome.size()), E.screencols);
        int padding = (E.screencols - welcomeLength) / 2;
        if (padding)
        {
          buffer.append(1, '~');
          --padding;
        }
        while (padding--)
        {
          buffer.append(1, ' ');
        }

        buffer.append(welcome.c_str(), welcomeLength);
      }
      else
      {
        buffer.append(1, '~');
      }
    }
    else
    {
      int len = E.row[filerow].render.size() - E.coloffset;
      if (len < 0)
      {
        len = 0;
      }
      if (len > E.screencols)
      {
        len = E.screencols;
      }
      buffer.append(E.row[filerow].render.substr(E.coloffset, len), 0, len);
    }

    buffer.append("\x1b[K", 3);
    buffer.append("\r\n", 2);
  }
}

void editorDrawStatusBar(std::string& buffer)
{
  buffer.append("\x1b[7m", 4);

  std::string status = std::format("{:20s} - {:d}", E.filename.empty() ? "[No Name]" : E.filename, E.numrows);
  std::size_t len{status.length() > E.screencols ? E.screencols : status.length()};

  std::string rStatus = std::format("{:d}/{:d}", E.cursorY + 1, E.numrows);

  buffer.append(status.c_str(), len);

  while (len < E.screencols)
  {
    if (E.screencols - len == rStatus.length())
    {
      buffer.append(rStatus.c_str(), rStatus.length());
      break;
    }
    else
    {
      buffer.append(" ");
      ++len;
    }
  }

  buffer.append("\x1b[m", 3);
}

void editorRefreshScreen()
{
  editorScroll();

  std::string buffer;

  // hide cursor
  buffer.append("\x1b[?25l", 6);
  buffer.append("\x1b[H", 3);

  editorDrawRows(buffer);
  editorDrawStatusBar(buffer);

  std::string cursor = std::format("\x1b[{};{}H", (E.cursorY - E.rowoffset) + 1, (E.renderX - E.coloffset) + 1);
  buffer.append(cursor.c_str(), cursor.size());

  // display cursor
  buffer.append("\x1b[?25h", 6);

  write(STDOUT_FILENO, buffer.c_str(), buffer.size());
}

/* input */

void editorMoveCursor(int key)
{
  switch (key)
  {
  case ARROW_LEFT:
    if (E.cursorX != 0)
    {
      E.cursorX--;
    }
    else if (E.cursorY > 0)
    { // if cursorX == 0 and not first line move to previous line
      E.cursorY--;
      E.cursorX = E.row[E.cursorY].chars.size();
    }
    break;
  case ARROW_RIGHT:
    if (E.cursorY < E.numrows && E.cursorX < E.row[E.cursorY].chars.size())
    {
      E.cursorX++;
    }
    else if (E.cursorY < E.numrows && E.cursorX == E.row[E.cursorY].chars.size())
    {
      E.cursorY++;
      E.cursorX = 0;
    }
    break;
  case ARROW_UP:
    if (E.cursorY > 0)
    {
      E.cursorY--;
    }
    break;
  case ARROW_DOWN:
    if (E.cursorY < E.numrows)
    {
      E.cursorY++;
    }
    break;
  }

  if (E.cursorY < E.numrows)
  {
    int rowLen = E.row[E.cursorY].chars.size();
    if (E.cursorX > rowLen)
    {
      E.cursorX = rowLen;
    }
  }
}

void editorProcessKeypress()
{
  int c = editorReadKey();

  switch (c)
  {
  case CTRL_KEY('q'):
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    exit(0);
    break;

  case HOME_KEY:
    E.cursorX = 0;
    break;

  case END_KEY:
    if (E.cursorY < E.numrows)
    {
      E.cursorY = E.row[E.cursorY].chars.size();
    }
    break;

  case PAGE_UP:
  case PAGE_DOWN: {
    if (c == PAGE_UP)
    {
      E.cursorY = E.rowoffset;
    }
    else if (c == PAGE_DOWN)
    {
      E.cursorY = E.rowoffset + E.screenrows - 1;
      if (E.cursorY > E.numrows)
        E.cursorY = E.numrows;
    }

    int times = E.screenrows;
    while (times--)
    {
      editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
    }
    break;
  }
  case ARROW_UP:
  case ARROW_LEFT:
  case EditorKey::ARROW_DOWN:
  case EditorKey::ARROW_RIGHT:
    editorMoveCursor(c);
    break;
  }
}

void initEditor()
{
  E.cursorX = 0;
  E.cursorY = 0;
  E.renderX = 0;
  E.rowoffset = 0;
  E.coloffset = 0;
  E.numrows = 0;
  E.row = {};
  E.filename = "";

  if (getWindowSize(E.screenrows, E.screencols) == -1)
    die("getWindowSize");

  // reserve a line at the bottom for our status bar
  E.screenrows -= 1;
}

int main(int argc, char* argv[])
{
  enableRawMode();
  initEditor();
  if (argc >= 2)
  {
    editorOpen(argv[1]);
  }

  while (1)
  {
    editorRefreshScreen();
    editorProcessKeypress();
  }
  return 0;
}
