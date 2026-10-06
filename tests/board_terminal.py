"""Small VT screen observer for assertions against the actual curses display."""
import codecs
import re
import unicodedata

from board_routing import Tui


class Screen:
    def __init__(self, rows=24, columns=100):
        self.rows, self.columns = rows, columns
        self.cells = [[' '] * columns for _ in range(rows)]
        self.row = self.column = 0
        self.top, self.bottom = 0, rows - 1
        self.saved = (0, 0)
        self.pending = ''
        self.decoder = codecs.getincrementaldecoder('utf-8')('replace')

    def resize(self, rows, columns):
        self.cells = [(row[:columns] + [' '] * columns)[:columns] for row in self.cells[:rows]]
        self.cells += [[' '] * columns for _ in range(rows - len(self.cells))]
        self.rows, self.columns = rows, columns
        self.top, self.bottom = 0, rows - 1
        self.row, self.column = min(self.row, rows - 1), min(self.column, columns - 1)

    @property
    def text(self):
        return '\n'.join(''.join(row).rstrip() for row in self.cells)

    def scroll(self, count=1):
        region = self.cells[self.top:self.bottom + 1]
        count = max(-len(region), min(len(region), count))
        blank = [[' '] * self.columns for _ in range(abs(count))]
        self.cells[self.top:self.bottom + 1] = region[count:] + blank if count > 0 else blank + region[:count]

    def csi(self, args, command):
        values = [int(x) if x.isdigit() else 0 for x in args.lstrip('?').split(';')]
        first = values[0] if values else 0
        count = first or 1
        if command in 'Hf': self.row, self.column = count - 1, (values[1] or 1) - 1 if len(values) > 1 else 0
        elif command == 'A': self.row -= count
        elif command == 'B': self.row += count
        elif command == 'C': self.column += count
        elif command == 'D': self.column -= count
        elif command == 'G': self.column = count - 1
        elif command == 'd': self.row = count - 1
        elif command == 'K':
            left, right = (0, self.columns) if first == 2 else (0, self.column + 1) if first == 1 else (self.column, self.columns)
            self.cells[self.row][left:right] = [' '] * (right - left)
        elif command == 'J':
            for row in range(self.rows):
                if first == 2 or (first == 0 and row > self.row) or (first == 1 and row < self.row): self.cells[row] = [' '] * self.columns
            self.csi(str(first), 'K')
        elif command == 'r': self.top, self.bottom = count - 1, (values[1] or self.rows) - 1 if len(values) > 1 else self.rows - 1
        elif command == 'S': self.scroll(count)
        elif command == 'T': self.scroll(-count)
        elif command == 'P':
            row = self.cells[self.row]; row[self.column:] = (row[self.column + count:] + [' '] * count)[:self.columns - self.column]
        elif command == '@':
            row = self.cells[self.row]; row[self.column:] = ([' '] * count + row[self.column:])[:self.columns - self.column]
        elif command == 'X':
            right = min(self.columns, self.column + count)
            self.cells[self.row][self.column:right] = [' '] * (right - self.column)
        elif command in 'LM':
            top = self.top; self.top = self.row; self.scroll(-count if command == 'L' else count); self.top = top
        self.row, self.column = max(0, min(self.row, self.rows - 1)), max(0, min(self.column, self.columns - 1))

    def feed(self, data):
        self.pending += self.decoder.decode(data)
        i = 0
        while i < len(self.pending):
            char = self.pending[i]
            if char == '\x1b':
                if i + 1 == len(self.pending): break
                next_char = self.pending[i + 1]
                if next_char == '[':
                    match = re.match(r'\x1b\[([0-?]*)([ -/]*)([@-~])', self.pending[i:])
                    if not match: break
                    self.csi(match[1], match[3]); i += len(match[0]); continue
                if next_char == ']':
                    match = re.match(r'\x1b\].*?(?:\x07|\x1b\\)', self.pending[i:], re.S)
                    if not match: break
                    i += len(match[0]); continue
                if next_char in '()':
                    if i + 2 >= len(self.pending): break
                    i += 3; continue
                if next_char == '7': self.saved = (self.row, self.column)
                elif next_char == '8': self.row, self.column = self.saved
                elif next_char == 'M':
                    if self.row == self.top: self.scroll(-1)
                    else: self.row = max(0, self.row - 1)
                i += 2; continue
            if char == '\r': self.column = 0
            elif char == '\n':
                if self.row == self.bottom: self.scroll()
                else: self.row = min(self.row + 1, self.rows - 1)
            elif char == '\b': self.column = max(0, self.column - 1)
            elif char == '\t': self.column = min((self.column // 8 + 1) * 8, self.columns - 1)
            elif char >= ' ' and not unicodedata.combining(char):
                width = 2 if unicodedata.east_asian_width(char) in 'WF' else 1
                if self.column + width > self.columns: self.column = 0; self.row = min(self.row + 1, self.rows - 1)
                self.cells[self.row][self.column] = char
                if width == 2 and self.column + 1 < self.columns: self.cells[self.row][self.column + 1] = ''
                self.column += width
            i += 1
        self.pending = self.pending[i:]


class ViewTui(Tui):
    def __init__(self):
        self.screen = Screen()
        super().__init__()

    def drain(self, seconds=.2):
        data = super().drain(seconds)
        self.screen.feed(data)
        return data

    def resize(self, rows, columns):
        self.screen.resize(rows, columns)
        return super().resize(rows, columns)
