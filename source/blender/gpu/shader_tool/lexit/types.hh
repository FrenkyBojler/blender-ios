/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

namespace lexit {

/* Make sure to declare this enum as being a char.
 * This is allow casting to string possible. */
enum TokenType : unsigned char {
  Invalid = 0,
  /* Use printable ascii chars to store them in string, and for easy debugging / testing. */
  Word = 'w',
  NewLine = '\n',
  Space = ' ',
  Dot = '.',
  Hash = '#',
  Ampersand = '&',
  Number = '0',
  String = '_', /* TODO(fclem): Move String to a DoubleQuote */
  DoubleQuote = '"',
  SingleQuote = '\'',
  ParOpen = '(',
  ParClose = ')',
  BracketOpen = '{',
  BracketClose = '}',
  SquareOpen = '[',
  SquareClose = ']',
  AngleOpen = '<',
  AngleClose = '>',
  Assign = '=',
  SemiColon = ';',
  Question = '?',
  Not = '!',
  Colon = ':',
  Comma = ',',
  Star = '*',
  Plus = '+',
  Minus = '-',
  Divide = '/',
  Tilde = '~',
  Caret = '^',
  Pipe = '|',
  Percent = '%',
  Backslash = '\\',
  /* Mark end of stream. */
  EndOfFile = '\0',

  /* --- Keywords --- */

  LogicalAnd = 'a',
  DoubleHash = 'A',
  Break = 'b',
  // Unused = 'B',
  Const = 'c',
  Constexpr = 'C',
  Do = 'd',
  Decrement = 'D',
  Deref = 'D', /* TODO(fclem): Deduplicate. */
  NotEqual = 'e',
  Equal = 'E',
  For = 'f',
  While = 'F',
  LogicalOr = 'g',
  GEqual = 'G',
  Switch = 'h',
  Case = 'H',
  If = 'i',
  Else = 'I',
  // Unused = 'j',
  // Unused = 'J',
  // Unused = 'k',
  // Unused = 'K',
  Inline = 'l',
  LEqual = 'L',
  Static = 'm',
  Enum = 'M',
  Namespace = 'n',
  PreprocessorNewline = 'N', /* TODO(fclem): Remove. */
  Union = 'o',
  Continue = 'O',
  // Unused = 'p',
  Increment = 'P',
  // Unused = 'q',
  // Unused = 'Q',
  Return = 'r',
  // Unused = 'R',
  Struct = 's',
  Class = 'S',
  Template = 't',
  This = 'T',
  Using = 'u',
  // Unused = 'U',
  Private = 'v',
  Public = 'V',
  // Word = 'w',
  // Unused = 'W',
  // Unused = 'x',
  // Unused = 'X',
  // Unused = 'y',
  // Unused = 'Y',
  // Unused = 'z',
  // Unused = 'Z',
  // Number = '0',
  // Unused = '1',
  // Unused = '2',
  // Unused = '3',
  // Unused = '4',
  // Unused = '5',
  // Unused = '6',
  // Unused = '7',
  // Unused = '8',
  // Unused = '9',

  /* Aliases. */
  Multiply = Star,
  And = Ampersand,
  Or = Pipe,
  Xor = Caret,
  GThan = AngleClose,
  LThan = AngleOpen,
  BitwiseNot = Tilde,
  Modulo = Percent,

  /* Last bit of the byte is used for various flag for different stages. */

  /* Flag for char-to-token tables to tell the tokenizer to agglomerate sequences of char
   * of the same token type. */
  Merge = (1 << 7),
};

}  // namespace lexit
