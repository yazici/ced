#include <curses.h>
#include <deque>
#include <vector>
#include "colors.h"
#include "log.h"
#include "terminal_collaborator.h"

TerminalCollaborator::TerminalCollaborator(
    const Buffer* buffer, const std::function<void()> invalidate)
    : Collaborator("terminal", absl::Seconds(0)),
      used_([this]() {
        mu_.AssertHeld();
        recently_used_ = true;
        invalidate_();
      }),
      invalidate_(invalidate),
      shutdown_(false),
      cursor_(String::Begin()),
      cursor_row_(0) {}

void TerminalCollaborator::Shutdown() {
  absl::MutexLock lock(&mu_);
  shutdown_ = true;
}

void TerminalCollaborator::Push(const EditNotification& notification) {
  {
    absl::MutexLock lock(&mu_);
    content_ = notification.content;
  }
  invalidate_();
}

EditResponse TerminalCollaborator::Pull() {
  EditResponse r;

  auto ready = [this]() {
    mu_.AssertHeld();
    return shutdown_ || !commands_.empty() || recently_used_;
  };

  mu_.LockWhen(absl::Condition(&ready));
  r.become_used = recently_used_ || !commands_.empty();
  r.commands.swap(commands_);
  recently_used_ = false;
  mu_.Unlock();

  return r;
}

void TerminalCollaborator::Render() {
  absl::MutexLock lock(&mu_);

  int fb_rows, fb_cols;
  getmaxyx(stdscr, fb_rows, fb_cols);

  if (cursor_row_ >= fb_rows) {
    cursor_row_ = fb_rows - 1;
  } else if (cursor_row_ < 0) {
    cursor_row_ = 0;
  }

  // generate a deque of lines by the ID of the starting element
  std::deque<ID> lines;
  String::Iterator it(content_, cursor_);
  cursor_ = it.id();
  while (!it.is_begin() && lines.size() < fb_rows) {
    if (it.value() == '\n') {
      lines.push_front(it.id());
    }
    it.MovePrev();
  }
  if (it.is_begin()) {
    lines.push_front(it.id());
  }
  int cursor_line_idx = static_cast<int>(lines.size()) - 1;
  it = String::Iterator(content_, cursor_);
  if (!it.is_end()) it.MoveNext();
  int tgt_lines = lines.size() + fb_rows;
  while (!it.is_end() && lines.size() < tgt_lines) {
    if (it.value() == '\n') {
      lines.push_back(it.id());
    }
    it.MoveNext();
  }

  // start rendering
  if (cursor_line_idx < cursor_row_) {
    cursor_row_ = cursor_line_idx;
  }

  int cursor_row = 0;
  int cursor_col = 0;

  for (int row = 0; row < fb_rows; row++) {
    int col = 0;
    int rend_row = row - cursor_row_ + cursor_line_idx;
    assert(rend_row >= 0);
    if (rend_row >= lines.size()) break;
    it = String::Iterator(content_, lines[rend_row]);
    if (it.id() == cursor_) {
      cursor_row = row;
      cursor_col = col;
    }
    it.MoveNext();
    for (;;) {
      if (it.id() == cursor_) {
        cursor_row = row;
        cursor_col = col + 1;
      }
      if (it.is_end()) break;
      if (it.value() == '\n') break;
      chtype attr = 0;
      Log() << "REND:" << row << "," << col << " ty=" << (int)it.token_type()
            << " ch=" << it.value();
      switch (it.token_type()) {
        case Token::UNSET:
          attr = COLOR_PAIR(ColorID::DEFAULT);
          break;
        case Token::IDENT:
          attr = COLOR_PAIR(ColorID::IDENT);
          break;
        case Token::KEYWORD:
          attr = COLOR_PAIR(ColorID::KEYWORD);
          break;
        case Token::SYMBOL:
          attr = COLOR_PAIR(ColorID::SYMBOL);
          break;
        case Token::LITERAL:
          attr = COLOR_PAIR(ColorID::LITERAL);
          break;
        case Token::COMMENT:
          attr = COLOR_PAIR(ColorID::COMMENT);
          break;
      }
      mvaddch(row, col, it.value() | attr);
      col++;
      it.MoveNext();
    }
  }

  move(cursor_row, cursor_col);
}

void TerminalCollaborator::ProcessKey(int key) {
  absl::MutexLock lock(&mu_);

  Log() << "TerminalCollaborator::ProcessKey: " << key;

  switch (key) {
    case KEY_LEFT: {
      String::Iterator it(content_, cursor_);
      it.MovePrev();
      cursor_ = it.id();
      used_();
    } break;
    case KEY_RIGHT: {
      String::Iterator it(content_, cursor_);
      it.MoveNext();
      cursor_ = it.id();
      used_();
    } break;
    case KEY_UP: {
      String::Iterator it(content_, cursor_);
      int col = 0;
      auto edge = [&it]() {
        return it.is_begin() || it.is_end() || it.value() == '\n';
      };
      while (!edge()) {
        it.MovePrev();
        col++;
      }
      Log() << "col:" << col;
      do {
        it.MovePrev();
      } while (!edge());
      it.MoveNext();
      for (; col > 0 && !edge(); col--) {
        it.MoveNext();
      }
      it.MovePrev();
      cursor_ = it.id();
      cursor_row_--;
      used_();
    } break;
    case KEY_DOWN: {
      String::Iterator it(content_, cursor_);
      int col = 0;
      auto edge = [&it]() {
        return it.is_begin() || it.is_end() || it.value() == '\n';
      };
      while (!edge()) {
        it.MovePrev();
        col++;
      }
      Log() << "col:" << col;
      it = String::Iterator(content_, cursor_);
      do {
        it.MoveNext();
      } while (!edge());
      it.MoveNext();
      for (; col > 0 && !edge(); col--) {
        it.MoveNext();
      }
      it.MovePrev();
      cursor_ = it.id();
      cursor_row_++;
      used_();
    } break;
    case 127:
    case KEY_BACKSPACE: {
      commands_.emplace_back(content_.MakeRemove(cursor_));
      String::Iterator it(content_, cursor_);
      it.MovePrev();
      cursor_ = it.id();
    } break;
    case 10: {
      auto cmd = content_.MakeInsert(site(), key, cursor_);
      cursor_ = cmd->id();
      cursor_row_++;
      commands_.emplace_back(std::move(cmd));
    } break;
    default: {
      if (key >= 32 && key < 127) {
        auto cmd = content_.MakeInsert(site(), key, cursor_);
        cursor_ = cmd->id();
        commands_.emplace_back(std::move(cmd));
      }
    } break;
  }
}