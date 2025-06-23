#ifndef EDITOR_H
#define EDITOR_H

#include <string>
#include <ctime>
#include <vector>
#include <termios.h>

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
  std::string statusmsg;
  std::time_t statusmsg_time;
  termios original_termios;
};

#endif
