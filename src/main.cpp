#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <errno.h>
#include <format>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define KILO_VERSION "0.0.1"
#define CTRL_KEY(k) ((k) & 0x1f)
#define KILO_TAB_STOP 8
#define KILO_QUIT_TIMES 3

/* forward declarations */
void editorSetStatusMessage(std::string_view fmt, ...);
void editorRefreshScreen();
std::string editorPrompt(std::string&& prompt, void (*callback)(std::string_view, int));

enum EditorKey
{
    BACKSPACE = 127,
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

enum EditorHighlight
{
    HL_NORMAL = 0,
    HL_COMMENT,
    HL_STRING,
    HL_NUMBER,
    HL_MATCH,
};

#define HL_HIGHLIGHT_NUMBERS (1 << 0)
#define HL_HIGHLIGHT_STIRNGS (1 << 1)

/* data */

struct EditorSyntax
{
    std::string filetype;
    std::span<const std::string_view> filematch;
    std::string singleLineCommentStart;
    int flags;
};

struct erow
{
    std::string chars;
    std::string render;
    std::string highlight;
};

struct EditorConfig
{
    int cursorX, cursorY;
    int renderX;
    int rowoffset;
    int coloffset;
    int screenrows;
    int screencols;
    int numrows;
    std::vector<erow> row;
    int dirty;
    std::string filename;
    std::string statusmsg;
    std::time_t statusmsg_time;
    const EditorSyntax* syntax;
    termios original_termios;
};
EditorConfig E;

/* filetypes */

constexpr std::array<std::string_view, 3> C_HL_extensions{".c", ".h", ".cpp"};

constexpr std::array<EditorSyntax, 1> HLDB{{
    {"c", C_HL_extensions, "//", HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STIRNGS},
}};

constexpr int HLDB_ENTRIES{HLDB.size()};

/* terminal */
void die(const char* s)
{
    perror(s);
    exit(1);
}

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

/* syntax highlighting */

bool isSeparator(int c)
{
    return isspace(c) || c == '\0' || std::string_view{",.()+-/*=~%<>[];"}.find(c) != std::string_view::npos;
}
void editorUpdateSyntax(erow& row)
{
    row.highlight.assign(row.render.size(), HL_NORMAL);

    if (!E.syntax)
        return;

    std::string_view scs = E.syntax->singleLineCommentStart;
    bool isScs = scs.length();

    // initialise to true because we consider the beginning of the line as a separator
    // otherwise, numbers at the beginning of a line won't be highlighted
    bool isPrevSeparator = true;
    int inString = 0;

    int i{0};
    while (i < row.render.size())
    {
        char c = row.render[i];
        unsigned char prevHighlight = (i > 0) ? row.highlight[i - 1] : HL_NORMAL;

        if (isScs && !inString) {
            if(row.render.substr(i).starts_with(scs)) {
                std::fill(row.highlight.begin() + i, row.highlight.end(), HL_COMMENT);
                break;
            }
        }

        if (E.syntax->flags & HL_HIGHLIGHT_STIRNGS)
        {
            if (inString)
            {
                row.highlight[i] = HL_STRING;

                if (c == '\\' && i + 1 < row.render.length())
                {
                    row.highlight[i + 1] = HL_STRING;
                    i += 2;
                    continue;
                }

                if (c == inString)
                {
                    inString = 0;
                }
                i++;
                isPrevSeparator = true;
                continue;
            }
            else
            {
                if (c == '"' || c == '\'')
                {
                    inString = c;
                    row.highlight[i] = HL_STRING;
                    i++;
                    continue;
                }
            }
        }

        if (E.syntax->flags & HL_HIGHLIGHT_NUMBERS)
        {
            if (isdigit(c) && (isPrevSeparator || prevHighlight == HL_NUMBER) ||
                (c == '.' && prevHighlight == HL_NUMBER))
            {
                row.highlight[i] = HL_NUMBER;
                i++;
                isPrevSeparator = false;
                continue;
            }
        }

        isPrevSeparator = isSeparator(c);
        i++;
    }
}

int editorSyntaxToColor(int hl)
{
    switch (hl)
    {
    case HL_COMMENT:
        return 36;
    case HL_STRING:
        return 35;
    case HL_NUMBER:
        return 31;
    case HL_MATCH:
        return 34;
    default:
        return 37;
    }
}

void editorSelectSyntaxHighlight()
{
    E.syntax = nullptr;
    if (E.filename.empty())
        return;

    std::string_view fileExtension{};

    std::size_t idx{E.filename.rfind('.')};
    if (idx != std::string::npos)
    {
        fileExtension = {E.filename.begin() + idx, E.filename.end()};
    }

    // Loop over each Language syntax
    for (const EditorSyntax& syntax : HLDB)
    {
        // Loop over each file extension/type
        for (const auto& fileType : syntax.filematch)
        {
            bool isExtension = syntax.filematch[0] == ".";
            if ((isExtension && !(fileExtension.empty()) && fileExtension == fileType) ||
                (!isExtension && (E.filename.find(fileType) != std::string_view::npos)))
            {
                E.syntax = &syntax;

                for (auto& filerow : E.row)
                {
                    editorUpdateSyntax(filerow);
                }
                return;
            }
        }
    }
}

/* row operations */

int editorRowCxToRx(erow& row, int cursorX)
{
    int renderX{0};
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

int editorRowRxToCx(erow& row, int renderX)
{
    int currentRx{0};
    int cursorX;
    for (cursorX = 0; cursorX < row.chars.length(); ++cursorX)
    {
        if (row.chars[cursorX] == '\t')
        {
            currentRx = (KILO_TAB_STOP - 1) - (currentRx % KILO_TAB_STOP);
        }
        currentRx++;

        if (currentRx > cursorX)
            return cursorX;
    }

    return cursorX;
}

void editorUpdateRow(erow& row)
{
    row.render.clear();
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
    editorUpdateSyntax(row);
}

void editorInsertRow(int at, std::string_view line)
{
    if (at < 0 || at > E.numrows)
        return;

    E.row.emplace(E.row.begin() + at, erow{static_cast<std::string>(line), "", ""});
    editorUpdateRow(E.row[at]);
    E.numrows++;
    E.dirty++;
}

void editorRowInsertChar(erow& row, int at, int c)
{
    if (at < 0 || at > row.chars.size())
    {
        at = row.chars.size();
    }
    row.chars.insert(at, 1, c);
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowDeleteChar(erow& row, int at)
{
    if (at < 0 || at >= row.chars.length())
    {
        return;
    }

    row.chars.erase(at, 1);
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowAppendString(erow& row, std::string_view str)
{
    row.chars.append(str);
    editorUpdateRow(row);
    E.dirty++;
}

void editorDelRow(erow& row, int at)
{
    if (at < 0 || at >= E.numrows)
        return;

    E.row.erase(E.row.begin() + at);
    E.numrows--;
    E.dirty++;
}

/* editor operations */

void editorInsertChar(int c)
{
    if (E.cursorY == E.numrows)
    {
        editorInsertRow(E.numrows, "");
    }

    editorRowInsertChar(E.row[E.cursorY], E.cursorX, c);
    E.cursorX++;
}

void editorDelChar()
{
    if (E.cursorY == E.numrows)
        return;
    if (E.cursorY == 0 && E.cursorX == 0)
        return;

    if (E.cursorX > 0)
    {
        editorRowDeleteChar(E.row[E.cursorY], E.cursorX - 1);
        E.cursorX--;
    }
    else
    {
        int prevLen = E.row[E.cursorY - 1].chars.length();
        editorRowAppendString(E.row[E.cursorY - 1], E.row[E.cursorY].chars);
        editorDelRow(E.row[E.cursorY], E.cursorY);
        E.cursorY--;
        E.cursorX = prevLen;
    }
}

void editorInsertNewline()
{
    if (E.cursorX == 0)
    {
        editorInsertRow(E.cursorY, "");
    }
    else
    {
        erow& row = E.row[E.cursorY];
        editorInsertRow(E.cursorY + 1, row.chars.substr(E.cursorX));
        row.chars.erase(E.cursorX);
        editorUpdateRow(row);
    }
    E.cursorY++;
    E.cursorX = 0;
}

/* file i/o */
std::string editorRowsToString()
{
    std::string fileContent;
    for (const erow row : E.row)
    {
        fileContent += row.chars;
        fileContent += '\n';
    }
    return fileContent;
}

void editorOpen(char* filename)
{
    E.filename = filename;

    editorSelectSyntaxHighlight();

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

        editorInsertRow(E.numrows, line);
    }
    infile.close();
    E.dirty = 0;
}

void editorSave()
{
    if (E.filename.empty())
    {
        E.filename = editorPrompt("Save as: %s", nullptr);
        if (E.filename.empty())
        {
            editorSetStatusMessage("Save aborted");
            return;
        }
        editorSelectSyntaxHighlight();
    }

    std::string fileContent = editorRowsToString();

    std::ofstream ostream(E.filename, std::ios::out);

    if (ostream)
    {
        if (ostream.write(fileContent.c_str(), fileContent.length()))
        {
            editorSetStatusMessage("%d bytes written to disk", fileContent.length());
            E.dirty = 0;
            return;
        }
        ostream.close();
    }

    editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

/* find */
void editorFindCallback(std::string_view query, int key)
{
    static int lastMatch = -1;
    static int direction = 1;

    static int savedHighlightLine;
    static std::string savedHighlight = {""};

    if (!savedHighlight.empty())
    {
        E.row[savedHighlightLine].highlight = savedHighlight;
        savedHighlight.clear();
    }

    if (key == '\r' || key == '\x1b')
    {
        lastMatch = -1;
        direction = 1;
        return;
    }
    else if (key == ARROW_RIGHT || key == ARROW_DOWN)
    {
        direction = 1;
    }
    else if (key == ARROW_LEFT || key == ARROW_UP)
    {
        direction = -1;
    }
    else
    {
        lastMatch = -1;
        direction = 1;
    }

    if (lastMatch == -1)
    {
        direction = 1;
    }
    int current{lastMatch};
    for (int i{0}; i < E.numrows; ++i)
    {
        current += direction;
        if (current == -1)
        {
            current = E.numrows - 1;
        }
        else if (current == E.numrows)
        {
            current = 0;
        }

        erow& row = E.row[current];
        std::size_t match = row.render.find(query);
        if (!(match == std::string::npos))
        {
            lastMatch = current;
            E.cursorY = current;
            E.cursorX = editorRowRxToCx(row, match);
            E.rowoffset = E.numrows;

            savedHighlightLine = current;
            savedHighlight = row.highlight;
            std::fill_n(row.highlight.begin() + match, query.length(), HL_MATCH);
            break;
        }
    }
}

void editorFind()
{
    int prevCX = E.cursorX;
    int prevCY = E.cursorY;
    int prevColOff = E.coloffset;
    int prevRowOff = E.rowoffset;

    std::string query = editorPrompt("Search: %s (ESC to cancel)", editorFindCallback);

    // restore cursor position and offset
    if (query.empty())
    {
        E.cursorX = prevCX;
        E.cursorY = prevCY;
        E.coloffset = prevColOff;
        E.rowoffset = prevRowOff;
    }
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

            char* c = E.row[filerow].render.data() + E.coloffset;
            char* hl = E.row[filerow].highlight.data() + E.coloffset;

            int currentColour{-1};
            for (int j{0}; j < len; ++j)
            {
                if (hl[j] == HL_NORMAL)
                {
                    if (currentColour != -1)
                    {
                        buffer.append("\x1b[39m", 5);
                        currentColour = -1;
                    }
                    buffer.append(1, c[j]);
                }
                else
                {
                    int colour = editorSyntaxToColor(hl[j]);
                    if (colour != currentColour)
                    {
                        currentColour = colour;
                        std::string colourByte = std::format("\x1b[{}m", colour);
                        buffer.append(colourByte);
                    }
                    buffer.append(1, c[j]);
                }
            }
            // reset colour back to white
            buffer.append("\x1b[39m", 5);
        }

        buffer.append("\x1b[K", 3);
        buffer.append("\r\n", 2);
    }
}

void editorDrawStatusBar(std::string& buffer)
{
    buffer.append("\x1b[7m", 4);

    std::string status = std::format("{:20s} - {:d} lines {:s}", E.filename.empty() ? "[No Name]" : E.filename,
                                     E.numrows, E.dirty ? "(modified)" : "");
    std::size_t len{status.length() > E.screencols ? E.screencols : status.length()};

    std::string rStatus =
        std::format("{:s} | {:d}/{:d}", E.syntax ? E.syntax->filetype : "no ft", E.cursorY + 1, E.numrows);

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
    buffer.append("\r\n", 2);
}

void editorDrawMessageBar(std::string& buffer)
{
    buffer.append("\x1b[K", 3);
    int msgLen{static_cast<int>(E.statusmsg.length())};
    if (msgLen > E.screencols)
        msgLen = E.screencols;

    if (msgLen && std::time(nullptr) - E.statusmsg_time < 5)
    {

        buffer.append(E.statusmsg.c_str(), msgLen);
    }
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
    editorDrawMessageBar(buffer);

    std::string cursor = std::format("\x1b[{};{}H", (E.cursorY - E.rowoffset) + 1, (E.renderX - E.coloffset) + 1);
    buffer.append(cursor.c_str(), cursor.size());

    // display cursor
    buffer.append("\x1b[?25h", 6);

    write(STDOUT_FILENO, buffer.c_str(), buffer.size());
}

// // For info on variadic templates, see
// // (https://learn.microsoft.com/en-us/cpp/cpp/ellipses-and-variadic-templates?view=msvc-170)
// template <typename... Args> void editorSetStatusMessage(std::string_view fmt, Args&&... args)
// {
//   // TODO: revisit to see what std::forward and std::make_format_args() do
//   E.statusmsg = std::vformat(fmt, std::make_format_args(std::forward<Args>(args)...));
//   E.statusmsg_time = std::time(nullptr);
// }

void editorSetStatusMessage(std::string_view fmt, ...)
{
    std::va_list list;
    va_start(list, fmt);

    char buf[256];
    std::vsnprintf(buf, sizeof(buf), static_cast<std::string>(fmt).c_str(), list);
    va_end(list);

    E.statusmsg = buf;
    E.statusmsg_time = std::time(nullptr);
}

/* input */

std::string editorPrompt(std::string&& prompt, void (*callback)(std::string_view, int))
{
    std::string buf;
    while (true)
    {
        editorSetStatusMessage(prompt, buf.c_str());
        editorRefreshScreen();

        int c = editorReadKey();
        if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE)
        {
            if (!buf.empty())
            {
                buf.pop_back();
            }
        }
        else if (c == '\x1b')
        {
            editorSetStatusMessage("");
            if (callback)
            {
                callback(buf, c);
            }
            return "";
        }
        // if enter key
        else if (c == '\r')
        {
            if (!buf.empty())
            {

                editorSetStatusMessage("");
                if (callback)
                {
                    callback(buf, c);
                }
                return buf;
            }
        }
        else if (!iscntrl(c) && c < 128)
        {
            buf += c;
        }

        if (callback)
        {
            callback(buf, c);
        }
    }
}

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
    static int quit_times = KILO_QUIT_TIMES;

    int c = editorReadKey();

    switch (c)
    {
    case '\r':
        editorInsertNewline();
        break;

    case CTRL_KEY('q'):
        if (E.dirty && quit_times > 0)
        {
            editorSetStatusMessage("WARNING! File has unsaved changes. Press Ctrl-Q %d more %s to quit.", quit_times,
                                   quit_times == 1 ? "time" : "times");
            quit_times--;
            return;
        }
        write(STDOUT_FILENO, "\x1b[2J", 4);
        write(STDOUT_FILENO, "\x1b[H", 3);
        exit(0);
        break;

    case CTRL_KEY('s'):
        editorSave();
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

    case CTRL_KEY('f'):
        editorFind();
        break;

    case BACKSPACE:
    case CTRL_KEY('h'):
    case DEL_KEY:
        editorDelChar();
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

    case CTRL_KEY('l'):
    case '\x1b':
        break;

    default:
        editorInsertChar(c);
        break;
    }

    // reset quit_times if other key pressed
    quit_times = KILO_QUIT_TIMES;
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
    E.dirty = 0;
    E.filename = "";
    E.statusmsg = "";
    E.statusmsg_time = 0;
    E.syntax = nullptr;

    if (getWindowSize(E.screenrows, E.screencols) == -1)
        die("getWindowSize");

    // reserve a line at the bottom for our status bar
    E.screenrows -= 2;
}

int main(int argc, char* argv[])
{
    enableRawMode();
    initEditor();
    if (argc >= 2)
    {
        editorOpen(argv[1]);
    }

    editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find");

    while (1)
    {
        editorRefreshScreen();
        editorProcessKeypress();
    }
    return 0;
}
