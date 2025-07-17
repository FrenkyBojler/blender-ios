/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 */

#include "BLI_string_ref.hh"
#include "BLI_string_utf8.h" /* own include */

#include "wcwidth.h"

struct OrderWeights {
  int codepoint;
  int weight;
  char alternate;
  char lettercase;
};

static const OrderWeights OrderWeightsTable[] = {
    /* European ordering rules  (EOR / EN 13710 NISO TR03-1999)
     * https://www.open-std.org/cen/tc304/EOR/eorhome.html
     * Three levels of weights for sort/collation. Primary considers "A" and "ã"
     * same, secondary differentiates these without considering case (Ã = ã),
     * tertiary is for case. Sorted by Unicode codepoint for quick lookup. */
    {0x0020, ' ', 0, 0},  /* Space. */
    {0x0021, 0, 25, 0},   /* Exclamation mark ignored. */
    {0x0022, 0, 15, 0},   /* Double quotation mark ignored. */
    {0x0023, '#', 0, 0},  /* Number sign. */
    {0x0024, '$', 1, 0},  /* Dollar sign. */
    {0x0025, '%', 2, 0},  /* Percent sign. */
    {0x0026, '&', 3, 0},  /* Ampersand sign. */
    {0x0027, 0, 14, 0},   /* Apostrophe ignored. */
    {0x0028, 0, 4, 0},    /* Open parenthesis ignored. */
    {0x0029, 0, 5, 0},    /* Close parenthesis ignored. */
    {0x002A, '*', 0, 0},  /* Asterisk sign. */
    {0x002B, '+', 0, 0},  /* Plus sign. */
    {0x002C, 0, 1, 0},    /* Comma ignored. */
    {0x002D, ' ', 14, 0}, /* Hyphen treated as space. */
    {0x002E, 0, 0, 0},    /* Period (full stop) ignored. */
    {0x002F, '/', 0, 0},  /* Forward slash. */
    {0x0030, '0', 0, 0},  /* Digit 0. */
    {0x0031, '1', 0, 0},  /* Digit 1. */
    {0x0032, '2', 0, 0},  /* Digit 2. */
    {0x0033, '3', 0, 0},  /* Digit 3. */
    {0x0034, '4', 0, 0},  /* Digit 4. */
    {0x0035, '5', 0, 0},  /* Digit 5. */
    {0x0036, '6', 0, 0},  /* Digit 6. */
    {0x0037, '7', 0, 0},  /* Digit 7. */
    {0x0038, '8', 0, 0},  /* Digit 8. */
    {0x0039, '9', 0, 0},  /* Digit 9. */
    {0x003A, 0, 3, 0},    /* Colon ignored. */
    {0x003B, 0, 2, 0},    /* Semi-colon ignored. */
    {0x003C, 0, 8, 0},    /* Open angle bracket ignored. */
    {0x003D, '=', 0, 0},  /* Equals sign. */
    {0x003E, 0, 9, 0},    /* Close angle bracket ignored. */
    {0x003F, 0, 27, 0},   /* Question mark ignored. */
    {0x0040, '@', 4, 0},  /* Commercial at. */
    {0x0041, 'a', 0, 1},  /* Capital Letter A. */
    {0x0042, 'b', 0, 1},  /* Capital Letter B. */
    {0x0043, 'c', 0, 1},  /* Capital Letter C. */
    {0x0044, 'd', 0, 1},  /* Capital Letter D. */
    {0x0045, 'e', 0, 1},  /* Capital Letter E. */
    {0x0046, 'f', 0, 1},  /* Capital Letter F. */
    {0x0047, 'g', 0, 1},  /* Capital Letter G. */
    {0x0048, 'h', 0, 1},  /* Capital Letter H. */
    {0x0049, 'i', 0, 1},  /* Capital Letter I. */
    {0x004A, 'j', 0, 1},  /* Capital Letter J. */
    {0x004B, 'k', 0, 1},  /* Capital Letter K. */
    {0x004C, 'l', 0, 1},  /* Capital Letter L. */
    {0x004D, 'm', 0, 1},  /* Capital Letter M. */
    {0x004E, 'n', 0, 1},  /* Capital Letter N. */
    {0x004F, 'o', 0, 1},  /* Capital Letter O. */
    {0x0050, 'p', 0, 1},  /* Capital Letter P. */
    {0x0051, 'q', 0, 1},  /* Capital Letter Q. */
    {0x0052, 'r', 0, 1},  /* Capital Letter R. */
    {0x0053, 's', 0, 1},  /* Capital Letter S. */
    {0x0054, 't', 0, 1},  /* Capital Letter T. */
    {0x0055, 'u', 0, 1},  /* Capital Letter U. */
    {0x0056, 'v', 0, 1},  /* Capital Letter V. */
    {0x0057, 'w', 0, 1},  /* Capital Letter W. */
    {0x0058, 'x', 0, 1},  /* Capital Letter X. */
    {0x0059, 'y', 0, 1},  /* Capital Letter Y. */
    {0x005A, 'z', 0, 1},  /* Capital Letter Z. */
    {0x005B, 0, 6, 0},    /* Open square brackets ignored. */
    {0x005C, '\\', 0, 0}, /* Backslash sign */
    {0x005D, 0, 7, 0},    /* Close square brackets ignored. */
    {0x005E, '^', 0, 0},  /* Caret sign. */
    {0x005F, '_', 0, 0},  /* Underscore sign. */
    {0x0061, 'a', 0, 0},  /* Small Letter a. */
    {0x0062, 'b', 0, 0},  /* Small Letter b. */
    {0x0063, 'c', 0, 0},  /* Small Letter c. */
    {0x0064, 'd', 0, 0},  /* Small Letter d. */
    {0x0065, 'e', 0, 0},  /* Small Letter e. */
    {0x0066, 'f', 0, 0},  /* Small Letter f. */
    {0x0067, 'g', 0, 0},  /* Small Letter g. */
    {0x0068, 'h', 0, 0},  /* Small Letter h. */
    {0x0069, 'i', 0, 0},  /* Small Letter i. */
    {0x006A, 'j', 0, 0},  /* Small Letter j. */
    {0x006B, 'k', 0, 0},  /* Small Letter k. */
    {0x006C, 'l', 0, 0},  /* Small Letter l. */
    {0x006D, 'm', 0, 0},  /* Small Letter m. */
    {0x006E, 'n', 0, 0},  /* Small Letter n. */
    {0x006F, 'o', 0, 0},  /* Small Letter o. */
    {0x0070, 'p', 0, 0},  /* Small Letter p. */
    {0x0071, 'q', 0, 0},  /* Small Letter q. */
    {0x0072, 'r', 0, 0},  /* Small Letter r. */
    {0x0073, 's', 0, 0},  /* Small Letter s. */
    {0x0074, 't', 0, 0},  /* Small Letter t. */
    {0x0075, 'u', 0, 0},  /* Small Letter u. */
    {0x0076, 'v', 0, 0},  /* Small Letter v. */
    {0x0077, 'w', 0, 0},  /* Small Letter w. */
    {0x0078, 'x', 0, 0},  /* Small Letter x. */
    {0x0079, 'y', 0, 0},  /* Small Letter y. */
    {0x007A, 'z', 0, 0},  /* Small Letter z. */
    {0x007B, 0, 12, 0},   /* Left curly bracket ignored. */
    {0x007C, '|', 0, 0},  /* Vertical bar sign. */
    {0x007D, 0, 13, 0},   /* Right curly bracket ignored. */
    {0x007E, '~', 0, 0},  /* Tilde sign. */
    {0x00A1, 0, 26, 0},   /* Inverted exclamation mark ignored. */
    {0x00A2, '$', 5, 0},  /* Cent. */
    {0x00A3, '$', 6, 0},  /* Pound Sign. */
    {0x00A4, '$', 7, 0},  /* Currency Sign. */
    {0x00A5, '$', 8, 0},  /* Yen Sign. */
    {0x00A9, '$', 9, 0},  /* Copyright Sign. */
    {0x00AB, 0, 10, 0},   /* Open double-angle bracket ignored. */
    {0x00AE, '$', 10, 0}, /* Registered Sign. */
    {0x00B2, '2', 1, 0},  /* Digit Superscript 2. */
    {0x00B3, '3', 1, 0},  /* Digit Superscript 3. */
    {0x00B5, 'μ', 1, 0},  /* Micro sign */
    {0x00B9, '1', 1, 0},  /* Digit Superscript 1. */
    {0x00BA, 'o', 1, 0},  /* Masculine Ordinal Indicator. */
    {0x00BC, '1', 4, 0},  /* 1/4 fraction. */
    {0x00BD, '1', 3, 0},  /* 1/2 fraction. */
    {0x00BE, '3', 3, 0},  /* 3/4 fraction. */
    {0x00BF, 0, 28, 0},   /* Inverted question mark ignored. */
    {0x00C0, 'a', 2, 1},  /* Capital Letter A Grave. */
    {0x00C1, 'a', 1, 1},  /* Capital Letter A Acute. */
    {0x00C2, 'a', 4, 1},  /* Capital Letter A Circumflex. */
    {0x00C3, 'a', 9, 1},  /* Capital Letter A Tilde. */
    {0x00C4, 'a', 7, 1},  /* Capital Letter A Diaeresis. */
    {0x00C5, 'a', 5, 1},  /* Capital Letter A Ring. */
    {0x00C6, 'a', 13, 1}, /* Capital Letter AE Ligature. */
    {0x00C7, 'c', 5, 1},  /* Capital Letter C with Cedilla. */
    {0x00C8, 'e', 2, 1},  /* Capital Letter E with Grave. */
    {0x00C9, 'e', 1, 1},  /* Capital Letter E with Acute. */
    {0x00CA, 'e', 4, 1},  /* Capital Letter E with Circumflex. */
    {0x00CB, 'e', 6, 1},  /* Capital Letter E with Diaeresis. */
    {0x00CC, 'i', 2, 1},  /* Capital Letter I with Grave. */
    {0x00CD, 'i', 1, 1},  /* Capital Letter I with Acute. */
    {0x00CE, 'i', 4, 1},  /* Capital Letter I with Circumflex. */
    {0x00CF, 'i', 5, 1},  /* Capital Letter I with Diaeresis. */
    {0x00D0, 'd', 4, 1},  /* Capital Letter Eth. */
    {0x00D1, 'n', 4, 1},  /* Capital Letter N with Tilde. */
    {0x00D2, 'o', 3, 1},  /* Capital Letter O with Grave. */
    {0x00D3, 'o', 2, 1},  /* Capital Letter O with Acute. */
    {0x00D4, 'o', 5, 1},  /* Capital Letter O with Circumflex. */
    {0x00D5, 'o', 8, 1},  /* Capital Letter O with Tilde. */
    {0x00D6, 'o', 6, 1},  /* Capital Letter O with Diaeresis. */
    {0x00D8, 'o', 12, 1}, /* Capital Letter O with Stroke. */
    {0x00DA, 'u', 1, 1},  /* Capital Letter U with Acute. */
    {0x00DB, 'u', 4, 1},  /* Capital Letter U with Circumflex. */
    {0x00DC, 'u', 6, 1},  /* Capital Letter U with Diaeresis. */
    {0x00DD, 'y', 1, 1},  /* Capital Letter Y with Acute. */
    {0x00DE, 'Þ', 0, 1},  /* Capital Letter Thorn. */
    {0x00DF, 's', 9, 0},  /* Small Letter Sharp s. */
    {0x00E0, 'a', 2, 0},  /* Small Letter a Grave. */
    {0x00E1, 'a', 1, 0},  /* Small Letter a Acute. */
    {0x00E2, 'a', 4, 0},  /* Small Letter a Circumflex. */
    {0x00E3, 'a', 9, 0},  /* Small Letter a Tilde. */
    {0x00E4, 'a', 7, 0},  /* Small Letter a Diaeresis. */
    {0x00E5, 'a', 5, 0},  /* Small Letter a Ring. */
    {0x00E6, 'a', 13, 0}, /* Small Letter ae Ligature. */
    {0x00E7, 'c', 5, 0},  /* Small Letter c with Cedilla. */
    {0x00E8, 'e', 2, 0},  /* Small Letter e with Grave. */
    {0x00E9, 'e', 1, 0},  /* Small Letter e with Acute. */
    {0x00EA, 'e', 4, 0},  /* Small Letter e with Circumflex. */
    {0x00EB, 'e', 6, 0},  /* Small Letter e with Diaeresis. */
    {0x00EC, 'i', 2, 0},  /* Small Letter i with Grave. */
    {0x00ED, 'i', 1, 0},  /* Small Letter i with Acute. */
    {0x00EE, 'i', 4, 0},  /* Small Letter i with Circumflex. */
    {0x00EF, 'i', 5, 0},  /* Small Letter i with Diaeresis. */
    {0x00F0, 'd', 4, 0},  /* Small Letter Eth. */
    {0x00F1, 'n', 4, 0},  /* Small Letter n with Tilde. */
    {0x00F2, 'o', 3, 0},  /* Small Letter o with Grave. */
    {0x00F3, 'o', 2, 0},  /* Small Letter o with Acute. */
    {0x00F4, 'o', 5, 0},  /* Small Letter o with Circumflex. */
    {0x00F5, 'o', 8, 0},  /* Small Letter o with Tilde. */
    {0x00F6, 'o', 6, 0},  /* Small Letter o with Diaeresis. */
    {0x00F8, 'o', 12, 0}, /* Small Letter o with Stroke. */
    {0x00FA, 'u', 1, 0},  /* Small Letter u with Acute. */
    {0x00FB, 'u', 4, 0},  /* Small Letter u with Circumflex. */
    {0x00FC, 'u', 6, 0},  /* Small Letter u with Diaeresis. */
    {0x00FD, 'y', 1, 0},  /* Small Letter y with Acute. */
    {0x00FE, 'þ', 0, 0},  /* Small Letter Thorn. */
    {0x00FF, 'y', 4, 0},  /* Small Letter y with Diaeresis. */
    {0x0100, 'a', 12, 1}, /* Capital Letter A Macron. */
    {0x0101, 'a', 12, 0}, /* Small Letter a Macron. */
    {0x0102, 'a', 3, 1},  /* Capital Letter A Breve. */
    {0x0103, 'a', 3, 0},  /* Small Letter a Breve. */
    {0x0104, 'a', 11, 1}, /* Capital Letter A Ogonek. */
    {0x0105, 'a', 11, 0}, /* Small Letter a Ogonek. */
    {0x0106, 'c', 1, 1},  /* Capital Letter C with Acute. */
    {0x0107, 'c', 1, 0},  /* Small Letter c with Acute. */
    {0x0108, 'c', 2, 1},  /* Capital Letter C with Circumflex. */
    {0x0109, 'c', 2, 0},  /* Small Letter c with Circumflex. */
    {0x010A, 'c', 4, 1},  /* Capital Letter C with Dot Above. */
    {0x010B, 'c', 4, 0},  /* Small Letter c with Dot Above. */
    {0x010C, 'c', 3, 1},  /* Capital Letter C with Caron. */
    {0x010D, 'c', 3, 0},  /* Small Letter c with Caron. */
    {0x010E, 'd', 1, 1},  /* Capital Letter D with Caron. */
    {0x010F, 'd', 1, 0},  /* Small Letter d with Caron. */
    {0x0110, 'd', 3, 1},  /* Capital Letter D with Stroke. */
    {0x0111, 'd', 3, 0},  /* Small Letter d with Stroke. */
    {0x0112, 'e', 8, 1},  /* Capital Letter E with Macron. */
    {0x0113, 'e', 8, 0},  /* Small Letter e with Macron. */
    {0x0114, 'e', 3, 1},  /* Capital Letter E with Breve. */
    {0x0115, 'e', 3, 0},  /* Small Letter e with Breve. */
    {0x0116, 'e', 9, 1},  /* Capital Letter E with Dot Above. */
    {0x0117, 'e', 9, 0},  /* Small Letter e with Dot Above. */
    {0x0118, 'e', 7, 1},  /* Capital Letter E with Ogonek. */
    {0x0119, 'e', 7, 0},  /* Small Letter e with Ogonek. */
    {0x011A, 'e', 5, 1},  /* Capital Letter E with Caron. */
    {0x011B, 'e', 5, 0},  /* Small Letter e with Caron. */
    {0x011C, 'g', 2, 1},  /* Capital Letter G with Circumflex. */
    {0x011D, 'g', 2, 0},  /* Small Letter g with Circumflex. */
    {0x011E, 'g', 1, 1},  /* Capital Letter G with Breve. */
    {0x011F, 'g', 1, 0},  /* Small Letter g with Breve. */
    {0x0120, 'g', 4, 1},  /* Capital Letter G with Dot Above. */
    {0x0121, 'g', 4, 0},  /* Small Letter g with Dot Above. */
    {0x0122, 'g', 5, 1},  /* Capital Letter G with Cedilla. */
    {0x0123, 'g', 5, 0},  /* Small Letter g with Cedilla. */
    {0x0124, 'h', 1, 1},  /* Capital Letter H with Circumflex. */
    {0x0125, 'h', 1, 0},  /* Small Letter h with Circumflex. */
    {0x0126, 'h', 3, 1},  /* Capital Letter H with Stroke. */
    {0x0127, 'h', 3, 0},  /* Small Letter h with Stroke. */
    {0x0128, 'i', 6, 1},  /* Capital Letter I with Tilde. */
    {0x0129, 'i', 6, 0},  /* Small Letter i with Tilde. */
    {0x012A, 'i', 9, 1},  /* Capital Letter I with Macron. */
    {0x012B, 'i', 9, 0},  /* Small Letter i with Macron. */
    {0x012C, 'i', 3, 1},  /* Capital Letter I with Breve. */
    {0x012D, 'i', 3, 0},  /* Small Letter i with Breve. */
    {0x012E, 'i', 8, 1},  /* Capital Letter I with Ogonek. */
    {0x012F, 'i', 8, 0},  /* Small Letter i with Ogonek. */
    {0x0130, 'i', 7, 1},  /* Capital Letter I with Dot Above. */
    {0x0131, 'i', 10, 0}, /* Small Letter Dotless i. */
    {0x0132, 'i', 11, 1}, /* Capital Ligature IJ. */
    {0x0133, 'i', 11, 0}, /* Small Ligature ij. */
    {0x0134, 'j', 1, 1},  /* Capital Letter J with Circumflex. */
    {0x0135, 'j', 1, 0},  /* Small Letter j with Circumflex. */
    {0x0136, 'k', 2, 1},  /* Capital Letter K with Cedilla. */
    {0x0137, 'k', 2, 0},  /* Small Letter k with Cedilla. */
    {0x0138, 'k', 3, 0},  /* Small Letter Kra. */
    {0x0139, 'l', 1, 1},  /* Capital Letter L with Acute. */
    {0x013A, 'l', 1, 0},  /* Small Letter l with Acute. */
    {0x013B, 'l', 3, 1},  /* Capital Letter L with Cedilla. */
    {0x013C, 'l', 3, 0},  /* Small Letter l with Cedilla. */
    {0x013D, 'l', 2, 1},  /* Capital Letter L with Caron. */
    {0x013E, 'l', 2, 0},  /* Small Letter l with Caron. */
    {0x013F, 'l', 5, 1},  /* Capital Letter L with Middle Dot. */
    {0x0140, 'l', 5, 0},  /* Small Letter l with Middle Dot. */
    {0x0141, 'l', 4, 1},  /* Capital Letter L with Stroke. */
    {0x0142, 'l', 4, 0},  /* Small Letter l with Stroke. */
    {0x0143, 'n', 2, 1},  /* Capital Letter N with Acute. */
    {0x0144, 'n', 2, 0},  /* Small Letter n with Acute. */
    {0x0145, 'n', 5, 1},  /* Capital Letter N with Cedilla. */
    {0x0146, 'n', 5, 0},  /* Small Letter n with Cedilla. */
    {0x0147, 'n', 3, 1},  /* Capital Letter N with Caron. */
    {0x0148, 'n', 3, 0},  /* Small Letter n with Caron. */
    {0x0149, 'n', 7, 0},  /* Small Letter n Preceded by Apostrophe. */
    {0x014A, 'n', 6, 1},  /* Capital Letter Eng. */
    {0x014B, 'n', 6, 0},  /* Small Letter Eng. */
    {0x014C, 'o', 11, 1}, /* Capital Letter O with Macron. */
    {0x014D, 'o', 11, 0}, /* Small Letter o with Macron. */
    {0x014E, 'o', 4, 1},  /* Capital Letter O with Breve. */
    {0x014F, 'o', 4, 0},  /* Small Letter o with Breve. */
    {0x0150, 'o', 7, 1},  /* Capital Letter O with Double Acute. */
    {0x0151, 'o', 7, 0},  /* Small Letter o with Double Acute. */
    {0x0152, 'o', 14, 1}, /* Capital Ligature OE. */
    {0x0153, 'o', 14, 0}, /* Small Ligature oe. */
    {0x0154, 'r', 1, 1},  /* Capital Letter R with Acute. */
    {0x0155, 'r', 1, 0},  /* Small Letter r with Acute. */
    {0x0156, 'r', 3, 1},  /* Capital Letter R with Cedilla. */
    {0x0157, 'r', 3, 0},  /* Small Letter r with Cedilla. */
    {0x0158, 'r', 2, 1},  /* Capital Letter R with Caron. */
    {0x0159, 'r', 2, 0},  /* Small Letter r with Caron. */
    {0x015A, 's', 1, 1},  /* Capital Letter S with Acute. */
    {0x015B, 's', 1, 0},  /* Small Letter s with Acute. */
    {0x015C, 's', 2, 1},  /* Capital Letter S with Circumflex. */
    {0x015D, 's', 2, 0},  /* Small Letter s with Circumflex. */
    {0x015E, 's', 5, 1},  /* Capital Letter S with Cedilla. */
    {0x015F, 's', 5, 0},  /* Small Letter s with Cedilla. */
    {0x0160, 's', 3, 1},  /* Capital Letter S with Caron. */
    {0x0161, 's', 3, 0},  /* Small Letter s with Caron. */
    {0x0162, 't', 3, 1},  /* Capital Letter T with Cedilla. */
    {0x0163, 't', 3, 0},  /* Small Letter t with Cedilla. */
    {0x0164, 't', 1, 1},  /* Capital Letter T with Caron. */
    {0x0165, 't', 1, 0},  /* Small Letter t with Caron. */
    {0x0166, 't', 5, 1},  /* Capital Letter T with Stroke. */
    {0x0167, 't', 5, 0},  /* Small Letter t with Stroke. */
    {0x0168, 'u', 8, 1},  /* Capital Letter U with Tilde. */
    {0x0169, 'u', 8, 0},  /* Small Letter u with Tilde. */
    {0x016A, 'u', 10, 1}, /* Capital Letter U with Macron. */
    {0x016B, 'u', 10, 0}, /* Small Letter u with Macron. */
    {0x016C, 'u', 3, 1},  /* Capital Letter U with Breve. */
    {0x016D, 'u', 3, 0},  /* Small Letter u with Breve. */
    {0x016E, 'u', 5, 1},  /* Capital Letter U with Ring Above. */
    {0x016F, 'u', 5, 0},  /* Small Letter u with Ring Above. */
    {0x0170, 'u', 7, 1},  /* Capital Letter U with Double Acute. */
    {0x0171, 'u', 7, 0},  /* Small Letter u with Double Acute. */
    {0x0172, 'u', 9, 1},  /* Capital Letter U with Ogonek. */
    {0x0173, 'u', 9, 0},  /* Small Letter u with Ogonek. */
    {0x0174, 'w', 3, 1},  /* Capital Letter W with Circumflex. */
    {0x0175, 'w', 3, 0},  /* Small Letter w with Circumflex. */
    {0x0176, 'y', 3, 1},  /* Capital Letter Y with Circumflex. */
    {0x0177, 'y', 3, 0},  /* Small Letter y with Circumflex. */
    {0x0178, 'y', 4, 1},  /* Capital Letter Y with Diaeresis. */
    {0x0179, 'z', 1, 1},  /* Capital Letter Z with Acute. */
    {0x017A, 'z', 1, 0},  /* Small Letter z with Acute. */
    {0x017B, 'z', 3, 1},  /* Capital Letter Z with Dot Above. */
    {0x017C, 'z', 3, 0},  /* Small Letter z with Dot Above. */
    {0x017D, 'z', 2, 1},  /* Capital Letter Z with Caron. */
    {0x017E, 'z', 2, 0},  /* Small Letter z with Caron. */
    {0x017F, 's', 7, 0},  /* Small Letter Long s. */
    {0x018F, 'e', 10, 1}, /* Capital Letter Schwa. */
    {0x0192, 'f', 2, 0},  /* Small Letter f with Hook. */
    {0x01B7, 'z', 4, 1},  /* Capital Letter Ezh. */
    {0x01DF, 'a', 8, 0},  /* Small Letter a Diaeresis Macron. */
    {0x01DE, 'a', 8, 1},  /* Capital Letter A Diaeresis Macron. */
    {0x01E0, 'a', 10, 1}, /* Capital Letter A Dot Above Macron. */
    {0x01E1, 'a', 10, 0}, /* Small Letter a Dot Above Macron. */
    {0x01E2, 'a', 15, 1}, /* Capital Letter AE Ligature Macron. */
    {0x01E3, 'a', 15, 0}, /* Small Letter ae Ligature Macron. */
    {0x01E4, 'g', 6, 1},  /* Capital Letter G with Stroke. */
    {0x01E5, 'g', 6, 0},  /* Small Letter g with Stroke. */
    {0x01E6, 'g', 3, 1},  /* Capital Letter G with Caron. */
    {0x01E7, 'g', 3, 0},  /* Small Letter g with Caron. */
    {0x01E8, 'k', 1, 1},  /* Capital Letter K with Caron. */
    {0x01E9, 'k', 1, 0},  /* Small Letter k with Caron. */
    {0x01EA, 'o', 9, 1},  /* Capital Letter O with Ogonek. */
    {0x01EB, 'o', 9, 0},  /* Small Letter o with Ogonek. */
    {0x01EC, 'o', 10, 1}, /* Capital Letter O with Ogonek and Macron. */
    {0x01ED, 'o', 10, 0}, /* Small Letter o with Ogonek and Macron. */
    {0x01EE, 'z', 5, 1},  /* Capital Letter Ezh with Caron. */
    {0x01EF, 'z', 5, 0},  /* Small Letter Ezh with Caron. */
    {0x01FB, 'a', 6, 0},  /* Small Letter Ring Acute. */
    {0x01FA, 'a', 6, 1},  /* Capital Letter Ring Acute. */
    {0x01FC, 'a', 14, 1}, /* Capital Letter AE Ligature Acute. */
    {0x01FD, 'a', 14, 0}, /* Small Letter ae Ligature Acute. */
    {0x01FF, 'o', 13, 0}, /* Small Letter o with Stroke and Acute. */
    {0x01FE, 'o', 13, 1}, /* Capital Letter O with Stroke and Acute. */
    {0x0218, 's', 6, 1},  /* Capital Letter S with Comma Below. */
    {0x0219, 's', 6, 0},  /* Small Letter s with Comma Below. */
    {0x021A, 't', 4, 1},  /* Capital Letter T with Comma Below. */
    {0x021B, 't', 4, 0},  /* Small Letter t with Comma Below. */
    {0x021E, 'h', 2, 1},  /* Capital Letter H with Caron. */
    {0x021F, 'h', 2, 0},  /* Small Letter h with Caron. */
    {0x0259, 'e', 10, 0}, /* Small Letter Schwa. */
    {0x027C, 'r', 4, 0},  /* Small Letter r with Long Leg. */
    {0x0292, 'z', 4, 0},  /* Small Letter Ezh. */
    {0x0386, 'α', 12, 1}, /* Greek capital alpha with tonos */
    {0x0388, 'ε', 9, 1},  /* Greek capital epsilon with tonos */
    {0x0389, 'η', 12, 1}, /* Greek capital eta with tonos */
    {0x038A, 'ι', 13, 1}, /* Greek capital iota with tonos */
    {0x038C, 'ο', 9, 1},  /* Greek capital omicron with tonos */
    {0x038E, 'υ', 12, 1}, /* Greek capital upsilon with tonos */
    {0x038F, 'ω', 13, 1}, /* Greek capital omega with tonos */
    {0x0390, 'ι', 19, 0}, /* Greek small iota with dialytika and tonos */
    {0x0391, 'α', 0, 1},  /* Capital Greek alpha. */
    {0x0392, 'β', 0, 1},  /* Greek capital beta */
    {0x0393, 'γ', 0, 1},  /* Greek capital gamma */
    {0x0394, 'δ', 0, 1},  /* Greek capital delta */
    {0x0395, 'ε', 0, 1},  /* Greek capital epsilon */
    {0x0396, 'ζ', 0, 1},  /* Greek capital zeta */
    {0x0397, 'η', 0, 1},  /* Greek capital eta */
    {0x0398, 'θ', 0, 1},  /* Greek capital theta */
    {0x0399, 'ι', 0, 1},  /* Greek capital iota */
    {0x039A, 'κ', 0, 1},  /* Greek capital kappa */
    {0x039B, 'λ', 0, 1},  /* Greek capital lamda */
    {0x039C, 'μ', 0, 1},  /* Greek capital mu */
    {0x039D, 'ν', 0, 1},  /* Greek capital nu */
    {0x039E, 'ξ', 0, 1},  /* Greek capital xi */
    {0x039F, 'ο', 0, 1},  /* Greek capital omicron */
    {0x03A0, 'π', 0, 1},  /* Greek capital pi */
    {0x03A1, 'ρ', 0, 1},  /* Greek capital rho */
    {0x03A3, 'ς', 0, 1},  /* Greek capital sigma */
    {0x03A4, 'τ', 0, 1},  /* Greek capital tau */
    {0x03A5, 'υ', 0, 1},  /* Greek capital upsilon */
    {0x03A6, 'φ', 0, 1},  /* Greek capital phi */
    {0x03A7, 'χ', 0, 1},  /* Greek capital chi */
    {0x03A8, 'ψ', 0, 1},  /* Greek capital psi */
    {0x03A9, 'ω', 0, 1},  /* Greek capital omega */
    {0x03AA, 'ι', 15, 1}, /* Greek capital iota with dialytika */
    {0x03AB, 'υ', 14, 1}, /* Greek capital upsilon with dialytika */
    {0x03AC, 'α', 12, 0}, /* Greek small alpha with tonos */
    {0x03AD, 'ε', 9, 0},  /* Greek small epsilon with tonos */
    {0x03AE, 'η', 12, 0}, /* Greek small eta with tonos */
    {0x03AF, 'ι', 13, 0}, /* Greek small iota with tonos */
    {0x03B0, 'υ', 18, 0}, /* Greek small upsilon with dialytika and tonos */
    {0x03B1, 'α', 0, 0},  /* Small Greek alpha. */
    {0x03B2, 'β', 0, 0},  /* Greek small beta */
    {0x03B3, 'γ', 0, 0},  /* Greek small gamma */
    {0x03B4, 'δ', 0, 0},  /* Greek small delta */
    {0x03B5, 'ε', 0, 0},  /* Greek small epsilon */
    {0x03B6, 'ζ', 0, 0},  /* Greek small zeta */
    {0x03B7, 'η', 0, 0},  /* Greek small eta */
    {0x03B8, 'θ', 0, 0},  /* Greek small theta */
    {0x03B9, 'ι', 0, 0},  /* Greek small iota */
    {0x03BA, 'κ', 0, 0},  /* Greek small kappa */
    {0x03BB, 'λ', 0, 0},  /* Greek small lamda */
    {0x03BC, 'μ', 0, 0},  /* Greek small mu */
    {0x03BD, 'ν', 0, 0},  /* Greek small nu */
    {0x03BE, 'ξ', 0, 0},  /* Greek small xi */
    {0x03BF, 'ο', 0, 0},  /* Greek small omicron */
    {0x03C0, 'π', 0, 0},  /* Greek small pi */
    {0x03C1, 'ρ', 0, 0},  /* Greek small rho */
    {0x03C2, 'ς', 1, 0},  /* Greek small final sigma */
    {0x03C3, 'ς', 0, 0},  /* Greek small sigma */
    {0x03C4, 'τ', 0, 0},  /* Greek small tau */
    {0x03C5, 'υ', 0, 0},  /* Greek small upsilon */
    {0x03C6, 'φ', 0, 0},  /* Greek small phi */
    {0x03C7, 'χ', 0, 0},  /* Greek small chi */
    {0x03C8, 'ψ', 0, 0},  /* Greek small psi */
    {0x03C9, 'ω', 0, 0},  /* Greek small omega */
    {0x03CA, 'ι', 15, 0}, /* Greek small iota with dialytika */
    {0x03CB, 'υ', 14, 0}, /* Greek small upsilon with dialytika */
    {0x03CC, 'ο', 9, 0},  /* Greek small omicron with tonos */
    {0x03CD, 'υ', 12, 0}, /* Greek small upsilon with tonos */
    {0x03CE, 'ω', 13, 0}, /* Greek small omega with tonos */
    {0x03D0, 'β', 0, 0},  /* Greek small beta symbol */
    {0x03D1, 'θ', 1, 0},  /* Greek theta symbol */
    {0x03D6, 'π', 1, 0},  /* Greek pi symbol */
    {0x03D7, 'κ', 2, 0},  /* Greek kai symbol */
    {0x03DA, 'ϛ', 0, 1},  /* Greek capital stigma */
    {0x03DB, 'ϛ', 0, 0},  /* Greek small stigma */
    {0x03DC, 'ϝ', 0, 1},  /* Greek capital digamma */
    {0x03DD, 'ϝ', 0, 0},  /* Greek small digamma */
    {0x03DE, 'ϙ', 0, 1},  /* Greek capital koppa */
    {0x03DF, 'ϙ', 0, 0},  /* Greek small koppa */
    {0x03E0, 'ϡ', 0, 1},  /* Greek capital sampi */
    {0x03E1, 'ϡ', 0, 0},  /* Greek small sampi */
    {0x03F0, 'κ', 1, 0},  /* Greek kappa symbol */
    {0x03F1, 'ρ', 1, 0},  /* Greek rho symbol */
    {0x0400, 'е', 1, 1},  /* Cyrillic capital letter ie with grave */
    {0x0401, 'е', 2, 1},  /* Cyrillic capital letter io */
    {0x0402, 'ђ', 0, 1},  /* Cyrillic capital letter dje */
    {0x0403, 'ђ', 1, 1},  /* Cyrillic capital letter gje */
    {0x0404, 'е', 4, 1},  /* Cyrillic capital letter ukrainian ie */
    {0x0405, 'ѕ', 0, 1},  /* Cyrillic capital letter dze */
    {0x0406, 'и', 4, 1},  /* Cyrillic capital letter byelorussian-ukrainian i */
    {0x0407, 'ї', 0, 1},  /* Cyrillic capital letter yi */
    {0x0408, 'ј', 0, 1},  /* Cyrillic capital letter je */
    {0x0409, 'љ', 0, 1},  /* Cyrillic capital letter lje */
    {0x040A, 'њ', 0, 1},  /* Cyrillic capital letter nje */
    {0x040B, 'ћ', 0, 1},  /* Cyrillic capital letter tshe */
    {0x040C, 'ќ', 1, 1},  /* Cyrillic capital letter kje */
    {0x040D, 'и', 1, 1},  /* Cyrillic capital letter i with grave */
    {0x040E, 'у', 2, 1},  /* Cyrillic capital letter short u */
    {0x040F, 'џ', 0, 1},  /* Cyrillic capital letter dzhe */
    {0x0410, 'а', 0, 1},  /* Cyrillic capital letter a */
    {0x0411, 'б', 0, 1},  /* Cyrillic capital letter be */
    {0x0412, 'в', 0, 1},  /* Cyrillic capital letter ve */
    {0x0413, 'г', 0, 1},  /* Cyrillic capital letter ghe */
    {0x0414, 'д', 0, 1},  /* Cyrillic capital letter de */
    {0x0415, 'е', 0, 1},  /* Cyrillic capital letter ie */
    {0x0416, 'ж', 0, 1},  /* Cyrillic capital letter zhe */
    {0x0417, 'з', 0, 1},  /* Cyrillic capital letter ze */
    {0x0418, 'и', 0, 1},  /* Cyrillic capital letter i */
    {0x0419, 'и', 5, 1},  /* Cyrillic capital letter short i */
    {0x041A, 'к', 0, 1},  /* Cyrillic capital letter ka */
    {0x041B, 'л', 0, 1},  /* Cyrillic capital letter el */
    {0x041C, 'м', 0, 1},  /* Cyrillic capital letter em */
    {0x041D, 'н', 0, 1},  /* Cyrillic capital letter en */
    {0x041E, 'о', 0, 1},  /* Cyrillic capital letter o */
    {0x041F, 'п', 0, 1},  /* Cyrillic capital letter pe */
    {0x0420, 'р', 0, 1},  /* Cyrillic capital letter er */
    {0x0421, 'с', 0, 1},  /* Cyrillic capital letter es */
    {0x0422, 'т', 0, 1},  /* Cyrillic capital letter te */
    {0x0423, 'у', 0, 1},  /* Cyrillic capital letter u */
    {0x0424, 'ф', 0, 1},  /* Cyrillic capital letter ef */
    {0x0425, 'х', 0, 1},  /* Cyrillic capital letter ha */
    {0x0426, 'ц', 0, 1},  /* Cyrillic capital letter tse */
    {0x0427, 'ч', 0, 1},  /* Cyrillic capital letter che */
    {0x0428, 'ш', 0, 1},  /* Cyrillic capital letter sha */
    {0x0429, 'щ', 0, 1},  /* Cyrillic capital letter shcha */
    {0x042A, 'ъ', 0, 1},  /* Cyrillic capital letter hard sign */
    {0x042B, 'ы', 0, 1},  /* Cyrillic capital letter yeru */
    {0x042C, 'ь', 0, 1},  /* Cyrillic capital letter soft sign */
    {0x042D, 'э', 0, 1},  /* Cyrillic capital letter e */
    {0x042E, 'ю', 0, 1},  /* Cyrillic capital letter yu */
    {0x042F, 'я', 0, 1},  /* Cyrillic capital letter ya */
    {0x0430, 'а', 0, 0},  /* Cyrillic small letter a */
    {0x0431, 'б', 0, 0},  /* Cyrillic small letter be */
    {0x0432, 'в', 0, 0},  /* Cyrillic small letter ve */
    {0x0433, 'г', 0, 0},  /* Cyrillic small letter ghe */
    {0x0434, 'д', 0, 0},  /* Cyrillic small letter de */
    {0x0435, 'е', 0, 0},  /* Cyrillic small letter ie */
    {0x0436, 'ж', 0, 0},  /* Cyrillic small letter zhe */
    {0x0437, 'з', 0, 0},  /* Cyrillic small letter ze */
    {0x0438, 'и', 0, 0},  /* Cyrillic small letter i */
    {0x0439, 'и', 5, 0},  /* Cyrillic small letter short i */
    {0x043A, 'к', 0, 0},  /* Cyrillic small letter ka */
    {0x043B, 'л', 0, 0},  /* Cyrillic small letter el */
    {0x043C, 'м', 0, 0},  /* Cyrillic small letter em */
    {0x043D, 'н', 0, 0},  /* Cyrillic small letter en */
    {0x043E, 'о', 0, 0},  /* Cyrillic small letter o */
    {0x043F, 'п', 0, 0},  /* Cyrillic small letter pe */
    {0x0440, 'р', 0, 0},  /* Cyrillic small letter er */
    {0x0441, 'с', 0, 0},  /* Cyrillic small letter es */
    {0x0442, 'т', 0, 0},  /* Cyrillic small letter te */
    {0x0443, 'у', 0, 0},  /* Cyrillic small letter u */
    {0x0444, 'ф', 0, 0},  /* Cyrillic small letter ef */
    {0x0445, 'х', 0, 0},  /* Cyrillic small letter ha */
    {0x0446, 'ц', 0, 0},  /* Cyrillic small letter tse */
    {0x0447, 'ч', 0, 0},  /* Cyrillic small letter che */
    {0x0448, 'ш', 0, 0},  /* Cyrillic small letter sha */
    {0x0449, 'щ', 0, 0},  /* Cyrillic small letter shcha */
    {0x044A, 'ъ', 0, 0},  /* Cyrillic small letter hard sign */
    {0x044B, 'ы', 0, 0},  /* Cyrillic small letter yeru */
    {0x044C, 'ь', 0, 0},  /* Cyrillic small letter soft sign */
    {0x044D, 'э', 0, 0},  /* Cyrillic small letter e */
    {0x044E, 'ю', 0, 0},  /* Cyrillic small letter yu */
    {0x044F, 'я', 0, 0},  /* Cyrillic small letter ya */
    {0x0450, 'е', 1, 0},  /* Cyrillic small letter ie with grave */
    {0x0451, 'е', 2, 0},  /* Cyrillic small letter io */
    {0x0452, 'ђ', 0, 0},  /* Cyrillic small letter dje */
    {0x0453, 'ђ', 1, 0},  /* Cyrillic small letter gje */
    {0x0454, 'е', 4, 0},  /* Cyrillic small letter ukrainian ie */
    {0x0455, 'ѕ', 0, 0},  /* Cyrillic small letter dze */
    {0x0456, 'и', 4, 0},  /* Cyrillic small letter byelorussian-ukrainian i */
    {0x0457, 'ї', 0, 0},  /* Cyrillic small letter yi */
    {0x0458, 'ј', 0, 0},  /* Cyrillic small letter je */
    {0x0459, 'љ', 0, 0},  /* Cyrillic small letter lje */
    {0x045A, 'њ', 0, 0},  /* Cyrillic small letter nje */
    {0x045B, 'ћ', 0, 0},  /* Cyrillic small letter tshe */
    {0x045C, 'ќ', 1, 0},  /* Cyrillic small letter kje */
    {0x045D, 'и', 1, 0},  /* Cyrillic small letter i with grave */
    {0x045E, 'у', 2, 0},  /* Cyrillic small letter short u */
    {0x045F, 'џ', 0, 0},  /* Cyrillic small letter dzhe */
    {0x0490, 'г', 1, 1},  /* Cyrillic capital letter ghe with upturn */
    {0x0491, 'г', 1, 0},  /* Cyrillic small letter ghe with upturn */
    {0x0492, 'г', 2, 1},  /* Cyrillic capital letter ghe with stroke */
    {0x0493, 'г', 2, 0},  /* Cyrillic small letter ghe with stroke */
    {0x0494, 'г', 3, 1},  /* Cyrillic capital letter ghe with middle hook */
    {0x0495, 'г', 3, 0},  /* Cyrillic small letter ghe with middle hook */
    {0x0496, 'ж', 3, 1},  /* Cyrillic capital letter zhe with descender */
    {0x0497, 'ж', 3, 0},  /* Cyrillic small letter zhe with descender */
    {0x0498, 'з', 1, 1},  /* Cyrillic capital letter ze with descender */
    {0x0499, 'з', 1, 0},  /* Cyrillic small letter ze with descender */
    {0x049A, 'к', 1, 1},  /* Cyrillic capital letter ka with descender */
    {0x049B, 'к', 1, 0},  /* Cyrillic small letter ka with descender */
    {0x049C, 'к', 5, 1},  /* Cyrillic capital letter ka with vertical stroke */
    {0x049D, 'к', 5, 0},  /* Cyrillic small letter ka with vertical stroke */
    {0x049E, 'к', 4, 1},  /* Cyrillic capital letter ka with stroke */
    {0x049F, 'к', 4, 0},  /* Cyrillic small letter ka with stroke */
    {0x04A0, 'к', 3, 1},  /* Cyrillic capital letter bashkir ka */
    {0x04A1, 'к', 3, 0},  /* Cyrillic small letter bashkir ka */
    {0x04A2, 'н', 1, 1},  /* Cyrillic capital letter en with descender */
    {0x04A3, 'н', 1, 0},  /* Cyrillic small letter en with descender */
    {0x04A4, 'н', 3, 1},  /* Cyrillic capital ligature en ghe */
    {0x04A5, 'н', 3, 0},  /* Cyrillic small ligature en ghe */
    {0x04A6, 'п', 1, 1},  /* Cyrillic capital letter pe with middle hook */
    {0x04A7, 'п', 1, 0},  /* Cyrillic small letter pe with middle hook */
    {0x04A8, 'ҩ', 0, 1},  /* Cyrillic capital letter abkhasian ha */
    {0x04A9, 'ҩ', 0, 0},  /* Cyrillic small letter abkhasian ha */
    {0x04AA, 'с', 1, 1},  /* Cyrillic capital letter es with descender */
    {0x04AB, 'с', 1, 0},  /* Cyrillic small letter es with descender */
    {0x04AC, 'т', 1, 1},  /* Cyrillic capital letter te with descender */
    {0x04AD, 'т', 1, 0},  /* Cyrillic small letter te with descender */
    {0x04AE, 'у', 5, 1},  /* Cyrillic capital letter straight u */
    {0x04AF, 'у', 5, 0},  /* Cyrillic small letter straight u */
    {0x04B0, 'у', 6, 1},  /* Cyrillic capital letter straight u with stroke */
    {0x04B1, 'у', 6, 0},  /* Cyrillic small letter straight u with stroke */
    {0x04B2, 'х', 1, 1},  /* Cyrillic capital letter ha with descender */
    {0x04B3, 'х', 1, 0},  /* Cyrillic small letter ha with descender */
    {0x04B4, 'ц', 1, 1},  /* Cyrillic capital ligature te tse */
    {0x04B5, 'ц', 1, 0},  /* Cyrillic small ligature te tse */
    {0x04B6, 'ч', 2, 1},  /* Cyrillic capital letter che with descender */
    {0x04B7, 'ч', 2, 0},  /* Cyrillic small letter che with descender */
    {0x04B8, 'ч', 4, 1},  /* Cyrillic capital letter che with vertical stroke */
    {0x04B9, 'ч', 4, 0},  /* Cyrillic small letter che with vertical stroke */
    {0x04BA, 'һ', 0, 1},  /* Cyrillic capital letter shha */
    {0x04BB, 'һ', 0, 0},  /* Cyrillic small letter shha */
    {0x04BC, 'ч', 5, 1},  /* Cyrillic capital letter abkhasian che */
    {0x04BD, 'ч', 5, 0},  /* Cyrillic small letter abkhasian che */
    {0x04BE, 'ч', 6, 1},  /* Cyrillic capital letter abkhasian che with descender */
    {0x04BF, 'ч', 6, 0},  /* Cyrillic small letter abkhasian che with descender */
    {0x04C0, 'Ӏ', 0, 1},  /* Cyrillic letter palochka */
    {0x04C1, 'ж', 1, 1},  /* Cyrillic capital letter zhe with breve */
    {0x04C2, 'ж', 1, 0},  /* Cyrillic small letter zhe with breve */
    {0x04C3, 'к', 2, 1},  /* Cyrillic capital letter ka with hook */
    {0x04C4, 'к', 2, 0},  /* Cyrillic small letter ka with hook */
    {0x04C7, 'н', 2, 1},  /* Cyrillic capital letter en with hook */
    {0x04C8, 'н', 2, 0},  /* Cyrillic small letter en with hook */
    {0x04CB, 'ч', 3, 1},  /* Cyrillic capital letter khakassian che */
    {0x04CC, 'ч', 3, 0},  /* Cyrillic small letter khakassian che */
    {0x04D0, 'а', 1, 1},  /* Cyrillic capital letter a with breve */
    {0x04D1, 'а', 1, 0},  /* Cyrillic small letter a with breve */
    {0x04D2, 'а', 2, 1},  /* Cyrillic capital letter a with diaeresis */
    {0x04D3, 'а', 2, 0},  /* Cyrillic small letter a with diaeresis */
    {0x04D4, 'ӕ', 0, 1},  /* Cyrillic capital ligature a ie */
    {0x04D5, 'ӕ', 0, 0},  /* Cyrillic small ligature a ie */
    {0x04D6, 'е', 3, 1},  /* Cyrillic capital letter ie with breve */
    {0x04D7, 'е', 3, 0},  /* Cyrillic small letter ie with breve */
    {0x04D8, 'ә', 0, 1},  /* Cyrillic capital letter schwa */
    {0x04D9, 'ә', 0, 0},  /* Cyrillic small letter schwa */
    {0x04DA, 'ә', 1, 1},  /* Cyrillic capital letter schwa with diaeresis */
    {0x04DB, 'ә', 1, 0},  /* Cyrillic small letter schwa with diaeresis */
    {0x04DC, 'ж', 2, 1},  /* Cyrillic capital letter zhe with diaeresis */
    {0x04DD, 'ж', 2, 0},  /* Cyrillic small letter zhe with diaeresis */
    {0x04DE, 'з', 2, 1},  /* Cyrillic capital letter ze with diaeresis */
    {0x04DF, 'з', 2, 0},  /* Cyrillic small letter ze with diaeresis */
    {0x04E0, 'ѕ', 1, 1},  /* Cyrillic capital letter abkhasian dze */
    {0x04E1, 'ѕ', 1, 0},  /* Cyrillic small letter abkhasian dze */
    {0x04E2, 'и', 2, 1},  /* Cyrillic capital letter i with macron */
    {0x04E3, 'и', 2, 0},  /* Cyrillic small letter i with macron */
    {0x04E4, 'и', 3, 1},  /* Cyrillic capital letter i with diaeresis */
    {0x04E5, 'и', 3, 0},  /* Cyrillic small letter i with diaeresis */
    {0x04E6, 'о', 1, 1},  /* Cyrillic capital letter o with diaeresis */
    {0x04E7, 'о', 1, 0},  /* Cyrillic small letter o with diaeresis */
    {0x04E8, 'о', 2, 1},  /* Cyrillic capital letter barred o */
    {0x04E9, 'о', 2, 0},  /* Cyrillic small letter barred o */
    {0x04EA, 'о', 3, 1},  /* Cyrillic capital letter barred o with diaeresis */
    {0x04EB, 'о', 3, 0},  /* Cyrillic small letter barred o with diaeresis */
    {0x04EE, 'у', 1, 1},  /* Cyrillic capital letter u with macron */
    {0x04EF, 'у', 1, 0},  /* Cyrillic small letter u with macron */
    {0x04F0, 'у', 3, 1},  /* Cyrillic capital letter u with diaeresis */
    {0x04F1, 'у', 3, 0},  /* Cyrillic small letter u with diaeresis */
    {0x04F2, 'у', 4, 1},  /* Cyrillic capital letter u with double acute */
    {0x04F3, 'у', 4, 0},  /* Cyrillic small letter u with double acute */
    {0x04F4, 'ч', 1, 1},  /* Cyrillic capital letter che with diaeresis */
    {0x04F5, 'ч', 1, 0},  /* Cyrillic small letter che with diaeresis */
    {0x04F8, 'ы', 1, 1},  /* Cyrillic capital letter yeru with diaeresis */
    {0x04F9, 'ы', 1, 0},  /* Cyrillic small letter yeru with diaeresis */
    {0x2002, ' ', 1, 0},  /* Space En. */
    {0x2003, ' ', 2, 0},  /* Space Em. */
    {0x2004, ' ', 3, 0},  /* Space Thick. */
    {0x2005, ' ', 4, 0},  /* Space Mid. */
    {0x2006, ' ', 6, 0},  /* Space Tiny. */
    {0x2007, ' ', 8, 0},  /* Space Figure. */
    {0x2008, ' ', 9, 0},  /* Space Puncuation. */
    {0x2009, ' ', 5, 0},  /* Space Thin. */
    {0x200A, ' ', 7, 0},  /* Space Hair. */
    {0x2010, ' ', 15, 0}, /* Hyphen treated as space. */
    {0x2011, ' ', 16, 0}, /* Hyphen treated as space. */
    {0x2012, ' ', 17, 0}, /* Hyphen treated as space. */
    {0x2013, ' ', 18, 0}, /* En Dash treated as space. */
    {0x2014, ' ', 19, 0}, /* Em Dash treated as space. */
    {0x2015, ' ', 20, 0}, /* Horizontal Bar treated as space. */
    {0x2018, 0, 16, 0},   /* Left single quotation mark ignored. */
    {0x2019, 0, 17, 0},   /* Right single quotation mark ignored. */
    {0x201A, 0, 18, 0},   /* Single low-9 quotation mark ignored. */
    {0x201B, 0, 19, 0},   /* Single high reversed 9 quotation mark ignored. */
    {0x201C, 0, 20, 0},   /* Left double quotation mark ignored. */
    {0x201D, 0, 21, 0},   /* Right double quotation mark ignored. */
    {0x201E, 0, 22, 0},   /* Double low 9 quotation mark ignored. */
    {0x2030, '$', 11, 0}, /* Per-mille Sign. */
    {0x2039, 0, 23, 0},   /* Left single angle quotation mark ignored. */
    {0x203A, 0, 24, 0},   /* right single angle quotation mark ignored. */
    {0x205F, ' ', 10, 0}, /* Space Math. */
    {0x20A3, '$', 12, 0}, /* French Franc Sign. */
    {0x20A4, '$', 13, 0}, /* Lira Sign. */
    {0x20A7, '$', 14, 0}, /* Peseta Sign. */
    {0x2105, '$', 15, 0}, /* Care of Sign. */
    {0x2116, 'n', 8, 0},  /* Numero Sign. */
    {0x2122, 't', 6, 2},  /* Trade Mark Sign. */
    {0x2126, 'ω', 1, 1},  /* Ohm sign */
    {0x215B, '1', 5, 0},  /* 1/8 fraction. */
    {0x215C, '3', 4, 0},  /* 3/8 fraction. */
    {0x215D, '5', 3, 0},  /* 5/8 fraction. */
    {0x215E, '7', 3, 0},  /* 7/8 fraction */
    {0x2212, ' ', 21, 0}, /* Minus Sign treated as space. */
    {0x3000, ' ', 11, 0}, /* Space Ideographic. */
    {0xFB01, 'f', 3, 0},  /* Small Ligature fi. */
    {0xFB02, 'f', 4, 0},  /* Small Ligature fl. */
    {0xFE50, 0, 29, 0},   /* Small Form Variant Comma ignored. */
    {0xFE52, 0, 30, 0},   /* Small Form Variant Period ignored. */
    {0xFE54, 0, 31, 0},   /* Small Form Variant Semi-colon ignored. */
    {0xFE55, 0, 32, 0},   /* Small Form Variant colon ignored. */
    {0xFE56, 0, 33, 0},   /* Small Form Variant question mark ignored. */
    {0xFE57, 0, 34, 0},   /* Small Form Variant exclamation mark ignored. */
    {0xFE5F, '#', 0, 0},  /* Small Form Variant Number sign. */
    {0xFE61, '*', 0, 0},  /* Small Form Variant asterisk. */
    {0xFF01, 0, 35, 0},   /* Half width exclamation mark ignored. */
    {0xFF02, 0, 36, 0},   /* Half width Double quotation mark ignored. */
    {0xFF07, 0, 37, 0},   /* Half width Apostrophe ignored. */
    {0xFF08, 0, 38, 0},   /* Half width Open parenthesis ignored. */
    {0xFF09, 0, 39, 0},   /* Half width Close parenthesis ignored. */
    {0xFF0A, '*', 0, 0},  /* Half width asterisk. */
    {0xFF0B, '+', 0, 0},  /* Half width Plus sign. */
    {0xFF0C, 0, 40, 0},   /* Half width comma ignored. */
    {0xFF0E, 0, 41, 0},   /* Half width period ignored. */
    {0xFF10, '0', 3, 0},  /* Full width digit 0. */
    {0xFF11, '1', 6, 0},  /* Half width digit 1. */
    {0xFF12, '2', 3, 0},  /* Half width digit 2. */
    {0xFF13, '3', 5, 0},  /* Half width digit 3. */
    {0xFF14, '4', 3, 0},  /* Half width digit 4. */
    {0xFF15, '5', 4, 0},  /* Half width digit 5. */
    {0xFF16, '6', 3, 0},  /* Half width digit 6. */
    {0xFF17, '7', 4, 0},  /* Half width digit 7. */
    {0xFF18, '8', 3, 0},  /* Half width digit 8. */
    {0xFF19, '9', 3, 0},  /* Half width digit 9. */
    {0xFF1D, '=', 0, 0},  /* Half width Equals sign. */
    {0xFF21, 'a', 16, 1}, /* Half width Capital Letter A. */
    {0xFF22, 'b', 2, 1},  /* Half width Capital Letter B. */
    {0xFF23, 'c', 2, 1},  /* Half width Capital Letter C. */
    {0xFF24, 'd', 5, 1},  /* Half width Capital Letter D. */
    {0xFF25, 'e', 11, 1}, /* Half width Capital Letter E. */
    {0xFF26, 'f', 5, 1},  /* Half width Capital Letter F. */
    {0xFF27, 'g', 7, 1},  /* Half width Capital Letter G. */
    {0xFF28, 'h', 4, 1},  /* Half width Capital Letter H. */
    {0xFF29, 'i', 12, 1}, /* Half width Capital Letter I. */
    {0xFF2A, 'j', 2, 1},  /* Half width Capital Letter J. */
    {0xFF2B, 'k', 4, 1},  /* Half width Capital Letter K. */
    {0xFF2C, 'l', 6, 1},  /* Half width Capital Letter L. */
    {0xFF2D, 'm', 2, 1},  /* Half width Capital Letter M. */
    {0xFF2E, 'n', 9, 1},  /* Half width Capital Letter N. */
    {0xFF2F, 'o', 15, 1}, /* Half width Capital Letter O. */
    {0xFF30, 'p', 2, 1},  /* Half width Capital Letter P. */
    {0xFF31, 'q', 1, 1},  /* Half width Capital Letter Q. */
    {0xFF32, 'r', 5, 1},  /* Half width Capital Letter R. */
    {0xFF33, 's', 10, 1}, /* Half width Capital Letter S. */
    {0xFF34, 't', 7, 1},  /* Half width Capital Letter T. */
    {0xFF35, 'u', 11, 1}, /* Half width Capital Letter U. */
    {0xFF36, 'v', 1, 1},  /* Half width Capital Letter V. */
    {0xFF37, 'w', 5, 1},  /* Half width Capital Letter W. */
    {0xFF38, 'x', 1, 1},  /* Half width Capital Letter X. */
    {0xFF39, 'y', 5, 1},  /* Half width Capital Letter Y. */
    {0xFF3A, 'z', 6, 1},  /* Half width Capital Letter Z. */
    {0xFF41, 'a', 16, 0}, /* Half width Small Letter A. */
    {0xFF42, 'b', 2, 0},  /* Half width Small Letter B. */
    {0xFF43, 'c', 2, 0},  /* Half width Small Letter C. */
    {0xFF44, 'd', 5, 0},  /* Half width Small Letter D. */
    {0xFF45, 'e', 11, 0}, /* Half width Small Letter E. */
    {0xFF46, 'f', 5, 0},  /* Half width Small Letter F. */
    {0xFF47, 'g', 7, 0},  /* Half width Small Letter G. */
    {0xFF48, 'h', 4, 0},  /* Half width Small Letter H. */
    {0xFF49, 'i', 12, 0}, /* Half width Small Letter I. */
    {0xFF4A, 'j', 2, 0},  /* Half width Small Letter J. */
    {0xFF4B, 'k', 4, 0},  /* Half width Small Letter K. */
    {0xFF4C, 'l', 6, 0},  /* Half width Small Letter L. */
    {0xFF4D, 'm', 2, 0},  /* Half width Small Letter M. */
    {0xFF4E, 'n', 9, 0},  /* Half width Small Letter N. */
    {0xFF4F, 'o', 15, 0}, /* Half width Small Letter O. */
    {0xFF50, 'p', 2, 0},  /* Half width Small Letter P. */
    {0xFF51, 'q', 1, 0},  /* Half width Small Letter Q. */
    {0xFF52, 'r', 5, 0},  /* Half width Small Letter R. */
    {0xFF53, 's', 10, 0}, /* Half width Small Letter S. */
    {0xFF54, 't', 7, 0},  /* Half width Small Letter T. */
    {0xFF55, 'u', 11, 0}, /* Half width Small Letter U. */
    {0xFF56, 'v', 1, 0},  /* Half width Small Letter V. */
    {0xFF57, 'w', 5, 0},  /* Half width Small Letter W. */
    {0xFF58, 'x', 1, 0},  /* Half width Small Letter X. */
    {0xFF59, 'y', 5, 0},  /* Half width Small Letter Y. */
    {0xFF5A, 'z', 6, 0},  /* Half width Small Letter Z. */
};

static const OrderWeights *bli_str_utf32_orderweights(char32_t codepoint)
{
  size_t left = 0;
  size_t right = sizeof(OrderWeightsTable) / sizeof(OrderWeightsTable[0]);
  while (left < right) {
    size_t mid = left + (right - left) / 2;
    if (OrderWeightsTable[mid].codepoint == int(codepoint)) {
      return &OrderWeightsTable[mid];
    }
    if (OrderWeightsTable[mid].codepoint < int(codepoint)) {
      left = mid + 1;
    }
    else {
      right = mid;
    }
  }

  return nullptr;
}

static int bli_str_utf32_weight(const OrderWeights *weights,
                                const bool alternates,
                                const bool lettercase)
{
  if (!weights) {
    return 0;
  }

  int weight = weights->weight;
  if (alternates) {
    weight += weights->alternate;
  }
  if (lettercase) {
    weight += weights->lettercase;
  }
  return weight;
}

struct Ligature {
  int codepoint;
  int replace[3] = {0};
  char uppercase;
};

static const std::array<Ligature, 30> LigatureTable{{
    {0x00BC, {'1', '/', '4'}, 0}, /* Vulgar fraction one quarter. */
    {0x00BD, {'1', '/', '2'}, 0}, /* Vulgar fraction one half. */
    {0x00BE, {'3', '/', '4'}, 0}, /* Vulgar fraction three quarters. */
    {0x00C6, {'a', 'e'}, 1},      /* Latin capital letter AE */
    {0x00DF, {'s', 's'}, 0},      /* Latin small letter sharp S. */
    {0x00E6, {'a', 'e'}, 0},      /* Latin small letter AE */
    {0x0132, {'i', 'j'}, 1},      /* Latin capital ligature FL. */
    {0x0133, {'i', 'j'}, 0},      /* Latin small ligature FL. */
    {0x0152, {'o', 'e'}, 1},      /* Latin capital ligature OE. */
    {0x0153, {'o', 'e'}, 0},      /* Latin small ligature OE. */
    {0x01E2, {'a', 'e'}, 1},      /* Latin capital letter AE with macron. */
    {0x01E3, {'a', 'e'}, 0},      /* Latin small letter AE with macron. */
    {0x01FC, {'a', 'e'}, 1},      /* Latin capital letter AE with acute. */
    {0x01FD, {'a', 'e'}, 0},      /* Latin small letter AE with acute. */
    {0x04A4, {'н', 'г'}, 1},      /* Cyrillic capital ligature EN GHE. */
    {0x04A5, {'н', 'г'}, 0},      /* Cyrillic small ligature EN GHE. */
    {0x04B4, {'т', 'ц'}, 1},      /* Cyrillic capital ligature TE TSE. */
    {0x04B5, {'т', 'ц'}, 0},      /* Cyrillic small ligature TE TSE. */
    {0x04D4, {'а', 'е'}, 1},      /* Cyrillic capital ligature A IE.  */
    {0x04D5, {'а', 'е'}, 0},      /* Cyrillic small ligature A IE.  */
    {0x2105, {'c', 'o'}, 0},      /* Care of. */
    {0x2116, {'n', 'o'}, 0},      /* Numero sign. */
    {0x2122, {'t', 'm'}, 0},      /* Trade mark sign. */
    {0x215B, {'1', '/', '8'}, 0}, /* Vulgar fraction one eighth. */
    {0x215C, {'3', '/', '8'}, 0}, /* Vulgar fraction three eighths. */
    {0x215D, {'3', '/', '5'}, 0}, /* Vulgar fraction three fifths. */
    {0x215E, {'7', '/', '8'}, 0}, /* Vulgar fraction seven eighths. */
    {0xFB01, {'f', 'i'}, 0},      /* Latin small ligature FI. */
    {0xFB02, {'f', 'l'}, 0},      /* Latin small ligature FL. */
}};

static const Ligature *bli_str_utf32_ligature(char32_t codepoint)
{
  auto ligature = std::lower_bound(LigatureTable.begin(),
                                   LigatureTable.end(),
                                   codepoint,
                                   [](const Ligature &s, int val) { return s.codepoint < val; });
  if (ligature != LigatureTable.end() && ligature->codepoint == codepoint) {
    return &(*ligature);
  }
  return nullptr;
}

std::string BLI_str_utf8_normalized(const blender::StringRef str, bool case_sensitive)
{
  std::string result;
  const size_t len = str.size();
  result.reserve(len);
  char utf8_buf[4];
  size_t utf8_buf_len;
  const Ligature *ligature = nullptr;

  size_t i = 0;
  while (i < len && str[i]) {
    char32_t wc = BLI_str_utf8_as_unicode_step_safe(str.data(), len, &i);
    ligature = bli_str_utf32_ligature(wc);
    if (ligature) {
      const bool ucase = case_sensitive && ligature->uppercase;
      utf8_buf_len = BLI_str_utf8_from_unicode(
          ucase ? BLI_str_utf32_char_to_upper(ligature->replace[0]) : ligature->replace[0],
          utf8_buf,
          sizeof(utf8_buf));
      result.append(utf8_buf, utf8_buf_len);
      utf8_buf_len = BLI_str_utf8_from_unicode(
          ucase ? BLI_str_utf32_char_to_upper(ligature->replace[1]) : ligature->replace[1],
          utf8_buf,
          sizeof(utf8_buf));
      result.append(utf8_buf, utf8_buf_len);
      if (ligature->replace[2]) {
        utf8_buf_len = BLI_str_utf8_from_unicode(
            ucase ? BLI_str_utf32_char_to_upper(ligature->replace[2]) : ligature->replace[2],
            utf8_buf,
            sizeof(utf8_buf));
        result.append(utf8_buf, utf8_buf_len);
      }
      continue;
    }

    const OrderWeights *weights = bli_str_utf32_orderweights(wc);
    const bool ucase = case_sensitive && weights && weights->lettercase;
    int normalized = weights ? bli_str_utf32_weight(weights, false, false) : wc;
    if (!weights && mk_wcwidth(wc) < 1) {
      normalized = 0; /* No weight for combining characters. */
    }

    if (normalized) {
      size_t utf8_buf_len = BLI_str_utf8_from_unicode(
          ucase ? BLI_str_utf32_char_to_upper(normalized) : normalized,
          utf8_buf,
          sizeof(utf8_buf));
      result.append(utf8_buf, utf8_buf_len);
    }
  }

  result.shrink_to_fit();
  return result;
}

bool BLI_str_utf8_contains(const char *s, const char *find, bool case_sensitive)
{
  const std::string full = BLI_str_utf8_normalized(s, case_sensitive);
  return full.find(BLI_str_utf8_normalized(find, case_sensitive)) != std::string::npos;
}

/** \} */
