/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 */

#include "BLI_map.hh"
#include "BLI_string_ref.hh"
#include "BLI_string_utf8.h" /* own include */

#include "wcwidth.h"

namespace blender {

struct OrderWeight {
  char32_t weight = 0;    /* primary weight as code_point or 0 */
  uint8_t alternate = 0;  /* secondary differenciation without case */
  bool uppercase = false; /* True if upper case, false if lowercase*/
};

static const Map<uint32_t, OrderWeight> &weights()
{
  static const Map<uint32_t, OrderWeight> weights = []() {
    Map<uint32_t, OrderWeight> weights;
    weights.add_as(0x0021, 0, 0);     /* Exclamation mark ignored. */
    weights.add_as(0x0022, 0, 1);     /* Double quotation mark ignored. */
    weights.add_as(0x0027, 0, 2);     /* Apostrophe ignored. */
    weights.add_as(0x0028, 0, 3);     /* Open parenthesis ignored. */
    weights.add_as(0x0029, 0, 4);     /* Close parenthesis ignored. */
    weights.add_as(0x002C, 0, 5);     /* Comma ignored. */
    weights.add_as(0x002E, 0, 6);     /* Period (full stop) ignored. */
    weights.add_as(0x003A, 0, 7);     /* Colon ignored. */
    weights.add_as(0x003B, 0, 8);     /* Semi-colon ignored. */
    weights.add_as(0x003C, 0, 9);     /* Open angle bracket ignored. */
    weights.add_as(0x003E, 0, 10);    /* Close angle bracket ignored. */
    weights.add_as(0x003F, 0, 11);    /* Question mark ignored. */
    weights.add_as(0x005B, 0, 12);    /* Open square brackets ignored. */
    weights.add_as(0x005D, 0, 13);    /* Close square brackets ignored. */
    weights.add_as(0x007B, 0, 14);    /* Left curly bracket ignored. */
    weights.add_as(0x007D, 0, 15);    /* Right curly bracket ignored. */
    weights.add_as(0x00A1, 0, 16);    /* Inverted exclamation mark ignored. */
    weights.add_as(0x00AB, 0, 17);    /* Open double-angle bracket ignored. */
    weights.add_as(0x00BF, 0, 18);    /* Inverted question mark ignored. */
    weights.add_as(0x2018, 0, 19);    /* Left single quotation mark ignored. */
    weights.add_as(0x2019, 0, 20);    /* Right single quotation mark ignored. */
    weights.add_as(0x201A, 0, 21);    /* Single low-9 quotation mark ignored. */
    weights.add_as(0x201B, 0, 22);    /* Single high reversed 9 quotation mark ignored. */
    weights.add_as(0x201C, 0, 23);    /* Left double quotation mark ignored. */
    weights.add_as(0x201D, 0, 24);    /* Right double quotation mark ignored. */
    weights.add_as(0x201E, 0, 25);    /* Double low 9 quotation mark ignored. */
    weights.add_as(0x2039, 0, 26);    /* Left single angle quotation mark ignored. */
    weights.add_as(0x203A, 0, 27);    /* right single angle quotation mark ignored. */
    weights.add_as(0xFE50, 0, 28);    /* Small Form Variant Comma ignored. */
    weights.add_as(0xFE52, 0, 29);    /* Small Form Variant Period ignored. */
    weights.add_as(0xFE54, 0, 30);    /* Small Form Variant Semi-colon ignored. */
    weights.add_as(0xFE55, 0, 31);    /* Small Form Variant colon ignored. */
    weights.add_as(0xFE56, 0, 32);    /* Small Form Variant question mark ignored. */
    weights.add_as(0xFE57, 0, 33);    /* Small Form Variant exclamation mark ignored. */
    weights.add_as(0xFF01, 0, 34);    /* Half width exclamation mark ignored. */
    weights.add_as(0xFF02, 0, 35);    /* Half width Double quotation mark ignored. */
    weights.add_as(0xFF07, 0, 36);    /* Half width Apostrophe ignored. */
    weights.add_as(0xFF08, 0, 37);    /* Half width Open parenthesis ignored. */
    weights.add_as(0xFF09, 0, 38);    /* Half width Close parenthesis ignored. */
    weights.add_as(0xFF0C, 0, 39);    /* Half width comma ignored. */
    weights.add_as(0xFF0E, 0, 40);    /* Half width period ignored. */
    weights.add_as(0x0020, U' ', 0);  /* Space. */
    weights.add_as(0x002D, U' ', 1);  /* Hyphen treated as space. */
    weights.add_as(0x2002, U' ', 2);  /* Space En. */
    weights.add_as(0x2003, U' ', 3);  /* Space Em. */
    weights.add_as(0x2004, U' ', 4);  /* Space Thick. */
    weights.add_as(0x2005, U' ', 5);  /* Space Mid. */
    weights.add_as(0x2006, U' ', 6);  /* Space Tiny. */
    weights.add_as(0x2007, U' ', 7);  /* Space Figure. */
    weights.add_as(0x2008, U' ', 8);  /* Space Puncuation. */
    weights.add_as(0x2009, U' ', 9);  /* Space Thin. */
    weights.add_as(0x200A, U' ', 10); /* Space Hair. */
    weights.add_as(0x2010, U' ', 11); /* Hyphen treated as space. */
    weights.add_as(0x2011, U' ', 12); /* Hyphen treated as space. */
    weights.add_as(0x2012, U' ', 13); /* Hyphen treated as space. */
    weights.add_as(0x2013, U' ', 14); /* En Dash treated as space. */
    weights.add_as(0x2014, U' ', 15); /* Em Dash treated as space. */
    weights.add_as(0x2015, U' ', 16); /* Horizontal Bar treated as space. */
    weights.add_as(0x205F, U' ', 17); /* Space Math. */
    weights.add_as(0x2212, U' ', 18); /* Minus Sign treated as space. */
    weights.add_as(0x3000, U' ', 19); /* Space Ideographic. */

    weights.add_as(0x0023, U'#', 0); /* Number sign. */
    weights.add_as(0xFE5F, U'#', 1); /* Small Form Variant Number sign. */
    weights.add_as(0x002A, U'*', 0); /* Asterisk sign. */
    weights.add_as(0xFE61, U'*', 1); /* Small Form Variant asterisk. */
    weights.add_as(0xFF0A, U'*', 2); /* Half width asterisk. */
    weights.add_as(0x002B, U'+', 0); /* Plus sign. */
    weights.add_as(0xFF0B, U'+', 1); /* Half width Plus sign. */
    weights.add_as(0x003D, U'=', 0); /* Equals sign. */
    weights.add_as(0xFF1D, U'=', 1); /* Half width Equals sign. */

    weights.add_as(0x0025, U'%', 0);  /* Percent sign. */
    weights.add_as(0x0026, U'&', 0);  /* Ampersand sign. */
    weights.add_as(0x002F, U'/', 0);  /* Forward slash. */
    weights.add_as(0x0040, U'@', 0);  /* Commercial at. */
    weights.add_as(0x005C, U'\\', 0); /* Backslash sign */
    weights.add_as(0x005E, U'^', 0);  /* Caret sign. */
    weights.add_as(0x005F, U'_', 0);  /* Underscore sign. */
    weights.add_as(0x007C, U'|', 0);  /* Vertical bar sign. */
    weights.add_as(0x007E, U'~', 0);  /* Tilde sign. */

    weights.add_as(0x0024, U'$', 0);  /* Dollar sign. */
    weights.add_as(0x00A2, U'$', 1);  /* Cent. */
    weights.add_as(0x00A3, U'$', 2);  /* Pound Sign. */
    weights.add_as(0x00A4, U'$', 3);  /* Currency Sign. */
    weights.add_as(0x00A5, U'$', 4);  /* Yen Sign. */
    weights.add_as(0x00A9, U'$', 5);  /* Copyright Sign. */
    weights.add_as(0x00AE, U'$', 6);  /* Registered Sign. */
    weights.add_as(0x2030, U'$', 7);  /* Per-mille Sign. */
    weights.add_as(0x20A3, U'$', 8);  /* French Franc Sign. */
    weights.add_as(0x20A4, U'$', 9);  /* Lira Sign. */
    weights.add_as(0x20A7, U'$', 10); /* Peseta Sign. */
    weights.add_as(0x2105, U'$', 11); /* Care of Sign. */

    weights.add_as(0x0030, U'0', 0); /* Digit 0. */
    weights.add_as(0xFF10, U'0', 1); /* Full width digit 0. */
    weights.add_as(0x0031, U'1', 0); /* Digit 1. */
    weights.add_as(0x00B9, U'1', 1); /* Digit Superscript 1. */
    weights.add_as(0x00BC, U'1', 2); /* 1/4 fraction. */
    weights.add_as(0x00BD, U'1', 3); /* 1/2 fraction. */
    weights.add_as(0x215B, U'1', 4); /* 1/8 fraction. */
    weights.add_as(0xFF11, U'1', 5); /* Half width digit 1. */
    weights.add_as(0x0032, U'2', 0); /* Digit 2. */
    weights.add_as(0x00B2, U'2', 1); /* Digit Superscript 2. */
    weights.add_as(0xFF12, U'2', 2); /* Half width digit 2. */
    weights.add_as(0x0033, U'3', 0); /* Digit 3. */
    weights.add_as(0x00B3, U'3', 1); /* Digit Superscript 3. */
    weights.add_as(0x00BE, U'3', 2); /* 3/4 fraction. */
    weights.add_as(0x215C, U'3', 3); /* 3/8 fraction. */
    weights.add_as(0xFF13, U'3', 4); /* Half width digit 3. */
    weights.add_as(0x0034, U'4', 0); /* Digit 4. */
    weights.add_as(0xFF14, U'4', 1); /* Half width digit 4. */
    weights.add_as(0x0035, U'5', 0); /* Digit 5. */
    weights.add_as(0x215D, U'5', 1); /* 5/8 fraction. */
    weights.add_as(0xFF15, U'5', 2); /* Half width digit 5. */
    weights.add_as(0x0036, U'6', 0); /* Digit 6. */
    weights.add_as(0xFF16, U'6', 1); /* Half width digit 6. */
    weights.add_as(0x0037, U'7', 0); /* Digit 7. */
    weights.add_as(0x215E, U'7', 1); /* 7/8 fraction */
    weights.add_as(0xFF17, U'7', 2); /* Half width digit 7. */
    weights.add_as(0x0038, U'8', 0); /* Digit 8. */
    weights.add_as(0xFF18, U'8', 1); /* Half width digit 8. */
    weights.add_as(0x0039, U'9', 0); /* Digit 9. */
    weights.add_as(0xFF19, U'9', 1); /* Half width digit 9. */

    weights.add_as(0x0041, U'a', 0, true);   /* Capital Letter A. */
    weights.add_as(0x0061, U'a', 0, false);  /* Small Letter a. */
    weights.add_as(0x00C0, U'a', 1, true);   /* Capital Letter A Grave. */
    weights.add_as(0x00E0, U'a', 1, false);  /* Small Letter a Grave. */
    weights.add_as(0x01FA, U'a', 2, true);   /* Capital Letter Ring Acute. */
    weights.add_as(0x01FB, U'a', 2, false);  /* Small Letter Ring Acute. */
    weights.add_as(0x01FC, U'a', 3, true);   /* Capital Letter AE Ligature Acute. */
    weights.add_as(0x01FD, U'a', 3, false);  /* Small Letter ae Ligature Acute. */
    weights.add_as(0x00C1, U'a', 4, true);   /* Capital Letter A Acute. */
    weights.add_as(0x00E1, U'a', 4, false);  /* Small Letter a Acute. */
    weights.add_as(0x00C2, U'a', 5, true);   /* Capital Letter A Circumflex. */
    weights.add_as(0x00E2, U'a', 5, false);  /* Small Letter a Circumflex. */
    weights.add_as(0x00C3, U'a', 6, true);   /* Capital Letter A Tilde. */
    weights.add_as(0x00E3, U'a', 6, false);  /* Small Letter a Tilde. */
    weights.add_as(0x00C4, U'a', 7, true);   /* Capital Letter A Diaeresis. */
    weights.add_as(0x00E4, U'a', 7, false);  /* Small Letter a Diaeresis. */
    weights.add_as(0x00C5, U'a', 8, true);   /* Capital Letter A Ring. */
    weights.add_as(0x00E5, U'a', 8, false);  /* Small Letter a Ring. */
    weights.add_as(0x00C6, U'a', 9, true);   /* Capital Letter AE Ligature. */
    weights.add_as(0x00E6, U'a', 9, false);  /* Small Letter ae Ligature. */
    weights.add_as(0x0100, U'a', 10, true);  /* Capital Letter A Macron. */
    weights.add_as(0x0101, U'a', 10, false); /* Small Letter a Macron. */
    weights.add_as(0x0102, U'a', 11, true);  /* Capital Letter A Breve. */
    weights.add_as(0x0103, U'a', 11, false); /* Small Letter a Breve. */
    weights.add_as(0x0104, U'a', 12, true);  /* Capital Letter A Ogonek. */
    weights.add_as(0x0105, U'a', 12, false); /* Small Letter a Ogonek. */
    weights.add_as(0x01DE, U'a', 13, true);  /* Capital Letter A Diaeresis Macron. */
    weights.add_as(0x01DF, U'a', 13, false); /* Small Letter a Diaeresis Macron. */
    weights.add_as(0x01E0, U'a', 14, true);  /* Capital Letter A Dot Above Macron. */
    weights.add_as(0x01E1, U'a', 14, false); /* Small Letter a Dot Above Macron. */
    weights.add_as(0x01E2, U'a', 15, true);  /* Capital Letter AE Ligature Macron. */
    weights.add_as(0x01E3, U'a', 15, false); /* Small Letter ae Ligature Macron. */
    weights.add_as(0xFF21, U'a', 16, true);  /* Half width Capital Letter A. */
    weights.add_as(0xFF41, U'a', 16, false); /* Half width Small Letter A. */

    weights.add_as(0x0042, U'b', 0, true);  /* Capital Letter B. */
    weights.add_as(0x0062, U'b', 0, false); /* Small Letter b. */
    weights.add_as(0xFF22, U'b', 1, true);  /* Half width Capital Letter B. */
    weights.add_as(0xFF42, U'b', 1, false); /* Half width Small Letter B. */

    weights.add_as(0x0043, U'c', 0, true);  /* Capital Letter C. */
    weights.add_as(0x0063, U'c', 0, false); /* Small Letter c. */
    weights.add_as(0x00C7, U'c', 1, true);  /* Capital Letter C with Cedilla. */
    weights.add_as(0x00E7, U'c', 1, false); /* Small Letter c with Cedilla. */
    weights.add_as(0x0106, U'c', 2, true);  /* Capital Letter C with Acute. */
    weights.add_as(0x0107, U'c', 2, false); /* Small Letter c with Acute. */
    weights.add_as(0x0108, U'c', 3, true);  /* Capital Letter C with Circumflex. */
    weights.add_as(0x0109, U'c', 3, false); /* Small Letter c with Circumflex. */
    weights.add_as(0x010A, U'c', 4, true);  /* Capital Letter C with Dot Above. */
    weights.add_as(0x010B, U'c', 4, false); /* Small Letter c with Dot Above. */
    weights.add_as(0x010C, U'c', 5, true);  /* Capital Letter C with Caron. */
    weights.add_as(0x010D, U'c', 5, false); /* Small Letter c with Caron. */
    weights.add_as(0xFF23, U'c', 6, true);  /* Half width Capital Letter C. */
    weights.add_as(0xFF43, U'c', 6, false); /* Half width Small Letter C. */

    weights.add_as(0x0044, U'd', 0, true);  /* Capital Letter D. */
    weights.add_as(0x0064, U'd', 0, false); /* Small Letter d. */
    weights.add_as(0x00D0, U'd', 1, true);  /* Capital Letter Eth. */
    weights.add_as(0x00F0, U'd', 1, false); /* Small Letter Eth. */
    weights.add_as(0x010E, U'd', 2, true);  /* Capital Letter D with Caron. */
    weights.add_as(0x010F, U'd', 3, false); /* Small Letter d with Caron. */
    weights.add_as(0x0110, U'd', 3, true);  /* Capital Letter D with Stroke. */
    weights.add_as(0x0111, U'd', 3, false); /* Small Letter d with Stroke. */
    weights.add_as(0xFF24, U'd', 4, true);  /* Half width Capital Letter D. */
    weights.add_as(0xFF44, U'd', 4, false); /* Half width Small Letter D. */

    weights.add_as(0x0045, U'e', 0, true);   /* Capital Letter E. */
    weights.add_as(0x0065, U'e', 0, false);  /* Small Letter e. */
    weights.add_as(0x00C8, U'e', 1, true);   /* Capital Letter E with Grave. */
    weights.add_as(0x00E8, U'e', 1, false);  /* Small Letter e with Grave. */
    weights.add_as(0x00C9, U'e', 2, true);   /* Capital Letter E with Acute. */
    weights.add_as(0x00E9, U'e', 2, false);  /* Small Letter e with Acute. */
    weights.add_as(0x00CA, U'e', 3, true);   /* Capital Letter E with Circumflex. */
    weights.add_as(0x00EA, U'e', 3, false);  /* Small Letter e with Circumflex. */
    weights.add_as(0x00CB, U'e', 4, true);   /* Capital Letter E with Diaeresis. */
    weights.add_as(0x00EB, U'e', 4, false);  /* Small Letter e with Diaeresis. */
    weights.add_as(0x0112, U'e', 5, true);   /* Capital Letter E with Macron. */
    weights.add_as(0x0113, U'e', 5, false);  /* Small Letter e with Macron. */
    weights.add_as(0x0114, U'e', 6, true);   /* Capital Letter E with Breve. */
    weights.add_as(0x0115, U'e', 6, false);  /* Small Letter e with Breve. */
    weights.add_as(0x0116, U'e', 7, true);   /* Capital Letter E with Dot Above. */
    weights.add_as(0x0117, U'e', 7, false);  /* Small Letter e with Dot Above. */
    weights.add_as(0x0118, U'e', 8, true);   /* Capital Letter E with Ogonek. */
    weights.add_as(0x0119, U'e', 8, false);  /* Small Letter e with Ogonek. */
    weights.add_as(0x011A, U'e', 9, true);   /* Capital Letter E with Caron. */
    weights.add_as(0x011B, U'e', 9, false);  /* Small Letter e with Caron. */
    weights.add_as(0x018F, U'e', 10, true);  /* Capital Letter Schwa. */
    weights.add_as(0x0259, U'e', 10, false); /* Small Letter Schwa. */
    weights.add_as(0xFF25, U'e', 11, true);  /* Half width Capital Letter E. */
    weights.add_as(0xFF45, U'e', 11, false); /* Half width Small Letter E. */

    weights.add_as(0x0046, U'f', 0, true);  /* Capital Letter F. */
    weights.add_as(0x0066, U'f', 0, false); /* Small Letter f. */
    weights.add_as(0x0192, U'f', 1, false); /* Small Letter f with Hook. */
    weights.add_as(0xFB01, U'f', 2, false); /* Small Ligature fi. */
    weights.add_as(0xFB02, U'f', 3, false); /* Small Ligature fl. */
    weights.add_as(0xFF26, U'f', 4, true);  /* Half width Capital Letter F. */
    weights.add_as(0xFF46, U'f', 4, false); /* Half width Small Letter F. */

    weights.add_as(0x0047, U'g', 0, true);  /* Capital Letter G. */
    weights.add_as(0x0067, U'g', 0, false); /* Small Letter g. */
    weights.add_as(0x011C, U'g', 1, true);  /* Capital Letter G with Circumflex. */
    weights.add_as(0x011D, U'g', 1, false); /* Small Letter g with Circumflex. */
    weights.add_as(0x011E, U'g', 2, true);  /* Capital Letter G with Breve. */
    weights.add_as(0x011F, U'g', 2, false); /* Small Letter g with Breve. */
    weights.add_as(0x0120, U'g', 3, true);  /* Capital Letter G with Dot Above. */
    weights.add_as(0x0121, U'g', 3, false); /* Small Letter g with Dot Above. */
    weights.add_as(0x0122, U'g', 4, true);  /* Capital Letter G with Cedilla. */
    weights.add_as(0x0123, U'g', 4, false); /* Small Letter g with Cedilla. */
    weights.add_as(0x01E4, U'g', 5, true);  /* Capital Letter G with Stroke. */
    weights.add_as(0x01E5, U'g', 5, false); /* Small Letter g with Stroke. */
    weights.add_as(0x01E6, U'g', 6, true);  /* Capital Letter G with Caron. */
    weights.add_as(0x01E7, U'g', 6, false); /* Small Letter g with Caron. */
    weights.add_as(0xFF27, U'g', 7, true);  /* Half width Capital Letter G. */
    weights.add_as(0xFF47, U'g', 7, false); /* Half width Small Letter G. */

    weights.add_as(0x0048, U'h', 0, true);  /* Capital Letter H. */
    weights.add_as(0x0068, U'h', 0, false); /* Small Letter h. */
    weights.add_as(0x0124, U'h', 1, true);  /* Capital Letter H with Circumflex. */
    weights.add_as(0x0125, U'h', 1, false); /* Small Letter h with Circumflex. */
    weights.add_as(0x0126, U'h', 2, true);  /* Capital Letter H with Stroke. */
    weights.add_as(0x0127, U'h', 2, false); /* Small Letter h with Stroke. */
    weights.add_as(0x021E, U'h', 3, true);  /* Capital Letter H with Caron. */
    weights.add_as(0x021F, U'h', 3, false); /* Small Letter H with Caron. */
    weights.add_as(0xFF28, U'h', 4, true);  /* Half width Capital Letter H. */
    weights.add_as(0xFF48, U'h', 4, false); /* Half width Small Letter H. */

    weights.add_as(0x0049, U'i', 0, true);   /* Capital Letter I. */
    weights.add_as(0x0069, U'i', 0, false);  /* Small Letter i. */
    weights.add_as(0x00CC, U'i', 1, true);   /* Capital Letter I with Grave. */
    weights.add_as(0x00EC, U'i', 1, false);  /* Small Letter i with Grave. */
    weights.add_as(0x00CD, U'i', 2, true);   /* Capital Letter I with Acute. */
    weights.add_as(0x00ED, U'i', 2, false);  /* Small Letter i with Acute. */
    weights.add_as(0x00CE, U'i', 3, true);   /* Capital Letter I with Circumflex. */
    weights.add_as(0x00EE, U'i', 3, false);  /* Small Letter i with Circumflex. */
    weights.add_as(0x00CF, U'i', 4, true);   /* Capital Letter I with Diaeresis. */
    weights.add_as(0x00EF, U'i', 4, false);  /* Small Letter i with Diaeresis. */
    weights.add_as(0x0128, U'i', 5, true);   /* Capital Letter I with Tilde. */
    weights.add_as(0x0129, U'i', 5, false);  /* Small Letter i with Tilde. */
    weights.add_as(0x012A, U'i', 6, true);   /* Capital Letter I with Macron. */
    weights.add_as(0x012B, U'i', 6, false);  /* Small Letter i with Macron. */
    weights.add_as(0x012C, U'i', 7, true);   /* Capital Letter I with Breve. */
    weights.add_as(0x012D, U'i', 7, false);  /* Small Letter i with Breve. */
    weights.add_as(0x012E, U'i', 8, true);   /* Capital Letter I with Ogonek. */
    weights.add_as(0x012F, U'i', 8, false);  /* Small Letter i with Ogonek. */
    weights.add_as(0x0130, U'i', 9, true);   /* Capital Letter I with Dot Above. */
    weights.add_as(0x0131, U'i', 10, false); /* Small Letter Dotless i. */
    weights.add_as(0x0132, U'i', 11, true);  /* Capital Ligature IJ. */
    weights.add_as(0x0133, U'i', 11, false); /* Small Ligature ij. */
    weights.add_as(0xFF29, U'i', 12, true);  /* Half width Capital Letter I. */
    weights.add_as(0xFF49, U'i', 12, false); /* Half width Small Letter I. */

    weights.add_as(0x004A, U'j', 0, true);  /* Capital Letter J. */
    weights.add_as(0x006A, U'j', 0, false); /* Small Letter j. */
    weights.add_as(0x0134, U'j', 1, true);  /* Capital Letter J with Circumflex. */
    weights.add_as(0x0135, U'j', 1, false); /* Small Letter j with Circumflex. */
    weights.add_as(0xFF2A, U'j', 2, true);  /* Half width Capital Letter J. */
    weights.add_as(0xFF4A, U'j', 2, false); /* Half width Small Letter J. */

    weights.add_as(0x004B, U'k', 0, true);  /* Capital Letter K. */
    weights.add_as(0x006B, U'k', 0, false); /* Small Letter k. */
    weights.add_as(0x0136, U'k', 1, true);  /* Capital Letter K with Cedilla. */
    weights.add_as(0x0137, U'k', 1, false); /* Small Letter k with Cedilla. */
    weights.add_as(0x0138, U'k', 2, false); /* Small Letter Kra. */
    weights.add_as(0x01E8, U'k', 3, true);  /* Capital Letter K with Caron. */
    weights.add_as(0x01E9, U'k', 3, false); /* Small Letter k with Caron. */
    weights.add_as(0xFF2B, U'k', 4, true);  /* Half width Capital Letter K. */
    weights.add_as(0xFF4B, U'k', 4, false); /* Half width Small Letter K. */

    weights.add_as(0x004C, U'l', 0, true);  /* Capital Letter L. */
    weights.add_as(0x006C, U'l', 0, false); /* Small Letter l. */
    weights.add_as(0x0139, U'l', 1, true);  /* Capital Letter L with Acute. */
    weights.add_as(0x013A, U'l', 1, false); /* Small Letter l with Acute. */
    weights.add_as(0x013B, U'l', 2, true);  /* Capital Letter L with Cedilla. */
    weights.add_as(0x013C, U'l', 2, false); /* Small Letter l with Cedilla. */
    weights.add_as(0x013D, U'l', 3, true);  /* Capital Letter L with Caron. */
    weights.add_as(0x013E, U'l', 3, false); /* Small Letter l with Caron. */
    weights.add_as(0x013F, U'l', 4, true);  /* Capital Letter L with Middle Dot. */
    weights.add_as(0x0140, U'l', 4, false); /* Small Letter l with Middle Dot. */
    weights.add_as(0x0141, U'l', 5, true);  /* Capital Letter L with Stroke. */
    weights.add_as(0x0142, U'l', 5, false); /* Small Letter l with Stroke. */
    weights.add_as(0xFF2C, U'l', 6, true);  /* Half width Capital Letter L. */
    weights.add_as(0xFF4C, U'l', 6, false); /* Half width Small Letter L. */

    weights.add_as(0x004D, U'm', 0, true);  /* Capital Letter M. */
    weights.add_as(0x006D, U'm', 0, false); /* Small Letter m. */
    weights.add_as(0xFF2D, U'm', 1, true);  /* Half width Capital Letter M. */
    weights.add_as(0xFF4D, U'm', 1, false); /* Half width Small Letter M. */

    weights.add_as(0x004E, U'n', 0, true);  /* Capital Letter N. */
    weights.add_as(0x006E, U'n', 0, false); /* Small Letter n with Tilde. */
    weights.add_as(0x0143, U'n', 2, true);  /* Capital Letter N with Acute. */
    weights.add_as(0x0144, U'n', 2, false); /* Small Letter n with Acute. */
    weights.add_as(0x0145, U'n', 3, true);  /* Capital Letter N with Cedilla. */
    weights.add_as(0x0146, U'n', 3, false); /* Small Letter n with Cedilla. */
    weights.add_as(0x0147, U'n', 4, true);  /* Capital Letter N with Caron. */
    weights.add_as(0x0148, U'n', 4, false); /* Small Letter n with Caron. */
    weights.add_as(0x0149, U'n', 5, false); /* Small Letter n Preceded by Apostrophe. */
    weights.add_as(0x014A, U'n', 6, true);  /* Capital Letter Eng. */
    weights.add_as(0x014B, U'n', 6, false); /* Small Letter Eng. */
    weights.add_as(0x2116, U'n', 7, false); /* Numero Sign. */
    weights.add_as(0xFF2E, U'n', 8, true);  /* Half width Capital Letter N. */
    weights.add_as(0xFF4E, U'n', 8, false); /* Half width Small Letter N. */

    weights.add_as(0x004F, U'o', 0, true);   /* Capital Letter O. */
    weights.add_as(0x006F, U'o', 0, false);  /* Small Letter o. */
    weights.add_as(0x00BA, U'o', 1, false);  /* Masculine Ordinal Indicator. */
    weights.add_as(0x00D2, U'o', 2, true);   /* Capital Letter O with Grave. */
    weights.add_as(0x00F2, U'o', 2, false);  /* Small Letter o with Grave. */
    weights.add_as(0x00D3, U'o', 3, true);   /* Capital Letter O with Acute. */
    weights.add_as(0x00F3, U'o', 3, false);  /* Small Letter o with Acute. */
    weights.add_as(0x00D4, U'o', 4, true);   /* Capital Letter O with Circumflex. */
    weights.add_as(0x00F4, U'o', 4, false);  /* Small Letter o with Circumflex. */
    weights.add_as(0x00D5, U'o', 5, true);   /* Capital Letter O with Tilde. */
    weights.add_as(0x00F5, U'o', 5, false);  /* Small Letter o with Tilde. */
    weights.add_as(0x00D6, U'o', 6, true);   /* Capital Letter O with Diaeresis. */
    weights.add_as(0x00F6, U'o', 6, false);  /* Small Letter o with Diaeresis. */
    weights.add_as(0x00D8, U'o', 7, true);   /* Capital Letter O with Stroke. */
    weights.add_as(0x00F8, U'o', 7, false);  /* Small Letter o with Stroke. */
    weights.add_as(0x014C, U'o', 8, true);   /* Capital Letter O with Macron. */
    weights.add_as(0x014D, U'o', 8, false);  /* Small Letter o with Macron. */
    weights.add_as(0x014E, U'o', 9, true);   /* Capital Letter O with Breve. */
    weights.add_as(0x014F, U'o', 9, false);  /* Small Letter o with Breve. */
    weights.add_as(0x0150, U'o', 10, true);  /* Capital Letter O with Double Acute. */
    weights.add_as(0x0151, U'o', 10, false); /* Small Letter o with Double Acute. */
    weights.add_as(0x0152, U'o', 11, true);  /* Capital Ligature OE. */
    weights.add_as(0x0153, U'o', 11, false); /* Small Ligature oe. */
    weights.add_as(0x01EA, U'o', 12, true);  /* Capital Letter O with Ogonek. */
    weights.add_as(0x01EB, U'o', 12, false); /* Small Letter o with Ogonek. */
    weights.add_as(0x01EC, U'o', 13, true);  /* Capital Letter O with Ogonek and Macron. */
    weights.add_as(0x01ED, U'o', 13, false); /* Small Letter o with Ogonek and Macron. */
    weights.add_as(0x01FE, U'o', 14, true);  /* Capital Letter O with Stroke and Acute. */
    weights.add_as(0x01FF, U'o', 14, false); /* Small Letter o with Stroke and Acute. */
    weights.add_as(0xFF2F, U'o', 15, true);  /* Half width Capital Letter O. */
    weights.add_as(0xFF4F, U'o', 15, false); /* Half width Small Letter O. */

    weights.add_as(0x0050, U'p', 0, true);  /* Capital Letter P. */
    weights.add_as(0x0070, U'p', 0, false); /* Small Letter p. */
    weights.add_as(0xFF30, U'p', 1, true);  /* Half width Capital Letter P. */
    weights.add_as(0xFF50, U'p', 1, false); /* Half width Small Letter P. */

    weights.add_as(0x0051, U'q', 0, true);  /* Capital Letter Q. */
    weights.add_as(0x0071, U'q', 0, false); /* Small Letter q. */
    weights.add_as(0xFF31, U'q', 1, true);  /* Half width Capital Letter Q. */
    weights.add_as(0xFF51, U'q', 1, false); /* Half width Small Letter Q. */

    weights.add_as(0x0052, U'r', 0, true);  /* Capital Letter R. */
    weights.add_as(0x0072, U'r', 0, false); /* Small Letter r. */
    weights.add_as(0x0154, U'r', 1, true);  /* Capital Letter R with Acute. */
    weights.add_as(0x0155, U'r', 1, false); /* Small Letter r with Acute. */
    weights.add_as(0x0156, U'r', 2, true);  /* Capital Letter R with Cedilla. */
    weights.add_as(0x0157, U'r', 2, false); /* Small Letter r with Cedilla. */
    weights.add_as(0x0158, U'r', 3, true);  /* Capital Letter R with Caron. */
    weights.add_as(0x0159, U'r', 3, false); /* Small Letter r with Caron. */
    weights.add_as(0x027C, U'r', 4, false); /* Small Letter r with Long Leg. */
    weights.add_as(0xFF32, U'r', 5, true);  /* Half width Capital Letter R. */
    weights.add_as(0xFF52, U'r', 5, false); /* Half width Small Letter R. */

    weights.add_as(0x0053, U's', 0, true);  /* Capital Letter S. */
    weights.add_as(0x0073, U's', 0, false); /* Small Letter s. */
    weights.add_as(0x00DF, U's', 1, false); /* Small Letter Sharp s. */
    weights.add_as(0x015A, U's', 2, true);  /* Capital Letter S with Acute. */
    weights.add_as(0x015B, U's', 2, false); /* Small Letter s with Acute. */
    weights.add_as(0x015C, U's', 3, true);  /* Capital Letter S with Circumflex. */
    weights.add_as(0x015D, U's', 3, false); /* Small Letter s with Circumflex. */
    weights.add_as(0x015E, U's', 4, true);  /* Capital Letter S with Cedilla. */
    weights.add_as(0x015F, U's', 4, false); /* Small Letter s with Cedilla. */
    weights.add_as(0x0160, U's', 5, true);  /* Capital Letter S with Caron. */
    weights.add_as(0x0161, U's', 5, false); /* Small Letter s with Caron. */
    weights.add_as(0x017F, U's', 6, false); /* Small Letter Long s. */
    weights.add_as(0x0218, U's', 7, true);  /* Capital Letter S with Comma Below. */
    weights.add_as(0x0219, U's', 7, false); /* Small Letter s with Comma Below. */
    weights.add_as(0xFF33, U's', 8, true);  /* Half width Capital Letter S. */
    weights.add_as(0xFF53, U's', 8, false); /* Half width Small Letter S. */

    weights.add_as(0x0054, U't', 0, true);   /* Capital Letter T. */
    weights.add_as(0x0074, U't', 0, false);  /* Small Letter t. */
    weights.add_as(0x0162, U't', 1, true);   /* Capital Letter T with Cedilla. */
    weights.add_as(0x0163, U't', 1, false);  /* Small Letter t with Cedilla. */
    weights.add_as(0x0164, U't', 2, true);   /* Capital Letter T with Caron. */
    weights.add_as(0x0165, U't', 2, false);  /* Small Letter t with Caron. */
    weights.add_as(0x0166, U't', 3, true);   /* Capital Letter T with Stroke. */
    weights.add_as(0x0167, U't', 3, false);  /* Small Letter t with Stroke. */
    weights.add_as(0x021A, U't', 4, true);   /* Capital Letter T with Comma Below. */
    weights.add_as(0x021B, U't', 4, false);  /* Small Letter t with Comma Below. */
    weights.add_as(0x2122, U't', 5, false);  /* Trade Mark Sign. */
    weights.add_as(0xFF34, U't', 6, true);   /* Half width Capital Letter T. */
    weights.add_as(0xFF54, U't', 6, false);  /* Half width Small Letter T. */
    weights.add_as(0x0055, U'u', 0, true);   /* Capital Letter U. */
    weights.add_as(0x0075, U'u', 0, false);  /* Small Letter u. */
    weights.add_as(0x00DA, U'u', 1, true);   /* Capital Letter U with Acute. */
    weights.add_as(0x00FA, U'u', 1, false);  /* Small Letter u with Acute. */
    weights.add_as(0x00DB, U'u', 2, true);   /* Capital Letter U with Circumflex. */
    weights.add_as(0x00FB, U'u', 2, false);  /* Small Letter u with Circumflex. */
    weights.add_as(0x00DC, U'u', 3, true);   /* Capital Letter U with Diaeresis. */
    weights.add_as(0x00FC, U'u', 3, false);  /* Small Letter u with Diaeresis. */
    weights.add_as(0x0168, U'u', 4, true);   /* Capital Letter U with Tilde. */
    weights.add_as(0x0169, U'u', 4, false);  /* Small Letter u with Tilde. */
    weights.add_as(0x016A, U'u', 5, true);   /* Capital Letter U with Macron. */
    weights.add_as(0x016B, U'u', 5, false);  /* Small Letter u with Macron. */
    weights.add_as(0x016C, U'u', 6, true);   /* Capital Letter U with Breve. */
    weights.add_as(0x016D, U'u', 6, false);  /* Small Letter U with Breve. */
    weights.add_as(0x016E, U'u', 7, true);   /* Capital Letter U with Ring Above. */
    weights.add_as(0x016F, U'u', 7, false);  /* Small Letter U with Ring Above. */
    weights.add_as(0x0170, U'u', 8, true);   /* Capital Letter U with Double Acute. */
    weights.add_as(0x0171, U'u', 8, false);  /* Small Letter U with Double Acute. */
    weights.add_as(0x0172, U'u', 9, true);   /* Capital Letter U with Ogonek. */
    weights.add_as(0x0173, U'u', 9, false);  /* Small Letter U with Ogonek. */
    weights.add_as(0xFF35, U'u', 10, true);  /* Half width Capital Letter U. */
    weights.add_as(0xFF55, U'u', 10, false); /* Half width Small Letter U. */

    weights.add_as(0x0056, U'v', 0, true);  /* Capital Letter V. */
    weights.add_as(0x0076, U'v', 0, false); /* Small Letter V. */
    weights.add_as(0xFF36, U'v', 1, true);  /* Half width Capital Letter V. */
    weights.add_as(0xFF56, U'v', 1, false); /* Half width Small Letter V. */

    weights.add_as(0x0057, U'w', 0, true);  /* Capital Letter W. */
    weights.add_as(0x0077, U'w', 0, false); /* Small Letter W. */
    weights.add_as(0x0174, U'w', 1, true);  /* Capital Letter W with Circumflex. */
    weights.add_as(0x0175, U'w', 1, false); /* Small Letter W with Circumflex. */
    weights.add_as(0xFF37, U'w', 2, true);  /* Half width Capital Letter W. */
    weights.add_as(0xFF57, U'w', 2, false); /* Half width Small Letter W. */

    weights.add_as(0x0058, U'x', 0, true);  /* Capital Letter X. */
    weights.add_as(0x0078, U'x', 0, false); /* Small Letter X. */
    weights.add_as(0xFF38, U'x', 1, true);  /* Half width Capital Letter X. */
    weights.add_as(0xFF58, U'x', 1, false); /* Half width Small Letter X. */

    weights.add_as(0x0059, U'y', 0, true);  /* Capital Letter Y. */
    weights.add_as(0x0079, U'y', 0, false); /* Small Letter Y. */
    weights.add_as(0x00DD, U'y', 1, true);  /* Capital Letter Y with Acute. */
    weights.add_as(0x00FD, U'y', 1, false); /* Small Letter Y with Acute. */
    weights.add_as(0x0178, U'y', 2, true);  /* Capital Letter Y with Diaeresis. */
    weights.add_as(0x00FF, U'y', 2, false); /* Small Letter Y with Diaeresis. */
    weights.add_as(0x0176, U'y', 3, true);  /* Capital Letter Y with Circumflex. */
    weights.add_as(0x0177, U'y', 3, false); /* Small Letter Y with Circumflex. */
    weights.add_as(0xFF39, U'y', 4, true);  /* Half width Capital Letter Y. */
    weights.add_as(0xFF59, U'y', 4, false); /* Half width Small Letter Y. */

    weights.add_as(0x005A, U'z', 0, true);  /* Capital Letter Z. */
    weights.add_as(0x007A, U'z', 0, false); /* Small Letter z. */
    weights.add_as(0x0179, U'z', 1, true);  /* Capital Letter Z with Acute. */
    weights.add_as(0x017A, U'z', 1, false); /* Small Letter z with Acute. */
    weights.add_as(0x017B, U'z', 2, true);  /* Capital Letter Z with Dot Above. */
    weights.add_as(0x017C, U'z', 2, false); /* Small Letter z with Dot Above. */
    weights.add_as(0x017D, U'z', 3, true);  /* Capital Letter Z with Caron. */
    weights.add_as(0x017E, U'z', 3, false); /* Small Letter z with Caron. */
    weights.add_as(0x01B7, U'z', 4, true);  /* Capital Letter Ezh. */
    weights.add_as(0x0292, U'z', 4, false); /* Small Letter Ezh. */
    weights.add_as(0x01EE, U'z', 5, true);  /* Capital Letter Ezh with Caron. */
    weights.add_as(0x01EF, U'z', 5, false); /* Small Letter Ezh with Caron. */
    weights.add_as(0xFF3A, U'z', 6, true);  /* Half width Capital Letter Z. */
    weights.add_as(0xFF5A, U'z', 6, false); /* Half width Small Letter Z. */

    weights.add_as(0x00DE, U'Þ', 0, true);  /* Capital Letter Thorn. */
    weights.add_as(0x00FE, U'þ', 0, false); /* Small Letter Thorn. */
    weights.add_as(0x00B5, U'μ', 0, false); /* Micro sign */
    weights.add_as(0x039C, U'μ', 1, true);  /* Greek capital mu */
    weights.add_as(0x03BC, U'μ', 1, false); /* Greek small mu */

    weights.add_as(0x0391, U'α', 0, true);  /* Capital Greek alpha. */
    weights.add_as(0x03B1, U'α', 0, false); /* Small Greek alpha. */
    weights.add_as(0x0386, U'α', 1, true);  /* Greek capital alpha with tonos */
    weights.add_as(0x03AC, U'α', 1, false); /* Greek small alpha with tonos */
    weights.add_as(0x0395, U'ε', 0, true);  /* Greek capital epsilon */
    weights.add_as(0x03B5, U'ε', 0, false); /* Greek small epsilon */
    weights.add_as(0x0388, U'ε', 1, true);  /* Greek capital epsilon with tonos */
    weights.add_as(0x03AD, U'ε', 1, false); /* Greek small epsilon with tonos */
    weights.add_as(0x0397, U'η', 0, true);  /* Greek capital eta */
    weights.add_as(0x03B7, U'η', 0, false); /* Greek small eta */
    weights.add_as(0x0389, U'η', 1, true);  /* Greek capital eta with tonos */
    weights.add_as(0x03AE, U'η', 1, false); /* Greek small eta with tonos */
    weights.add_as(0x0399, U'ι', 0, true);  /* Greek capital iota */
    weights.add_as(0x03B9, U'ι', 0, false); /* Greek small iota */
    weights.add_as(0x038A, U'ι', 1, true);  /* Greek capital iota with tonos */
    weights.add_as(0x03AF, U'ι', 1, false); /* Greek small iota with tonos */
    weights.add_as(0x03AA, U'ι', 2, true);  /* Greek capital iota with dialytika */
    weights.add_as(0x03CA, U'ι', 2, false); /* Greek small iota with dialytika */
    weights.add_as(0x0390, U'ι', 3, false); /* Greek small iota with dialytika and tonos */
    weights.add_as(0x039F, U'ο', 0, true);  /* Greek capital omicron */
    weights.add_as(0x03BF, U'ο', 0, false); /* Greek small omicron */
    weights.add_as(0x038C, U'ο', 1, true);  /* Greek capital omicron with tonos */
    weights.add_as(0x03CC, U'ο', 1, false); /* Greek small omicron with tonos */
    weights.add_as(0x03A5, U'υ', 0, true);  /* Greek capital upsilon */
    weights.add_as(0x03C5, U'υ', 0, false); /* Greek small upsilon */
    weights.add_as(0x038E, U'υ', 1, true);  /* Greek capital upsilon with tonos */
    weights.add_as(0x03CD, U'υ', 1, false); /* Greek small upsilon with tonos */
    weights.add_as(0x03AB, U'υ', 2, true);  /* Greek capital upsilon with dialytika */
    weights.add_as(0x03CB, U'υ', 2, false); /* Greek small upsilon with dialytika */
    weights.add_as(0x03B0, U'υ', 3, false); /* Greek small upsilon with dialytika and tonos */
    weights.add_as(0x03A9, U'ω', 0, true);  /* Greek capital omega */
    weights.add_as(0x03C9, U'ω', 0, false); /* Greek small omega */
    weights.add_as(0x038F, U'ω', 1, true);  /* Greek capital omega with tonos */
    weights.add_as(0x03CE, U'ω', 1, false); /* Greek small omega with tonos */
    weights.add_as(0x2126, U'ω', 2, false); /* Ohm sign */
    weights.add_as(0x0392, U'β', 0, true);  /* Greek capital beta */
    weights.add_as(0x03B2, U'β', 0, false); /* Greek small beta */
    weights.add_as(0x03D0, U'β', 1, false); /* Greek small beta symbol */
    weights.add_as(0x0393, U'γ', 0, true);  /* Greek capital gamma */
    weights.add_as(0x03B3, U'γ', 0, false); /* Greek small gamma */
    weights.add_as(0x0394, U'δ', 0, true);  /* Greek capital delta */
    weights.add_as(0x03B4, U'δ', 0, false); /* Greek small delta */
    weights.add_as(0x0396, U'ζ', 0, true);  /* Greek capital zeta */
    weights.add_as(0x03B6, U'ζ', 0, false); /* Greek small zeta */
    weights.add_as(0x0398, U'θ', 0, true);  /* Greek capital theta */
    weights.add_as(0x03B8, U'θ', 0, false); /* Greek small theta */
    weights.add_as(0x03D1, U'θ', 1, false); /* Greek theta symbol */
    weights.add_as(0x039A, U'κ', 0, true);  /* Greek capital kappa */
    weights.add_as(0x03BA, U'κ', 0, false); /* Greek small kappa */
    weights.add_as(0x03D7, U'κ', 1, false); /* Greek kai symbol */
    weights.add_as(0x03F0, U'κ', 2, false); /* Greek kappa symbol */
    weights.add_as(0x039B, U'λ', 0, true);  /* Greek capital lamda */
    weights.add_as(0x03BB, U'λ', 0, false); /* Greek small lamda */
    weights.add_as(0x039D, U'ν', 0, true);  /* Greek capital nu */
    weights.add_as(0x03BD, U'ν', 0, false); /* Greek small nu */
    weights.add_as(0x039E, U'ξ', 0, true);  /* Greek capital xi */
    weights.add_as(0x03BE, U'ξ', 0, false); /* Greek small xi */
    weights.add_as(0x03A0, U'π', 0, true);  /* Greek capital pi */
    weights.add_as(0x03C0, U'π', 0, false); /* Greek small pi */
    weights.add_as(0x03D6, U'π', 1, false); /* Greek pi symbol */
    weights.add_as(0x03A1, U'ρ', 0, true);  /* Greek capital rho */
    weights.add_as(0x03C1, U'ρ', 0, false); /* Greek small rho */
    weights.add_as(0x03F1, U'ρ', 1, false); /* Greek rho symbol */
    weights.add_as(0x03A3, U'ς', 0, true);  /* Greek capital sigma */
    weights.add_as(0x03C3, U'ς', 0, false); /* Greek small sigma */
    weights.add_as(0x03C2, U'ς', 1, false); /* Greek small final sigma */
    weights.add_as(0x03A4, U'τ', 0, true);  /* Greek capital tau */
    weights.add_as(0x03C4, U'τ', 0, false); /* Greek small tau */
    weights.add_as(0x03A6, U'φ', 0, true);  /* Greek capital phi */
    weights.add_as(0x03C6, U'φ', 0, false); /* Greek small phi */
    weights.add_as(0x03A7, U'χ', 0, true);  /* Greek capital chi */
    weights.add_as(0x03C7, U'χ', 0, false); /* Greek small chi */
    weights.add_as(0x03A8, U'ψ', 0, true);  /* Greek capital psi */
    weights.add_as(0x03C8, U'ψ', 0, false); /* Greek small psi */
    weights.add_as(0x03DA, U'ϛ', 0, true);  /* Greek capital stigma */
    weights.add_as(0x03DB, U'ϛ', 0, false); /* Greek small stigma */
    weights.add_as(0x03DC, U'ϝ', 0, true);  /* Greek capital digamma */
    weights.add_as(0x03DD, U'ϝ', 0, false); /* Greek small digamma */
    weights.add_as(0x03DE, U'ϙ', 0, true);  /* Greek capital koppa */
    weights.add_as(0x03DF, U'ϙ', 0, false); /* Greek small koppa */
    weights.add_as(0x03E0, U'ϡ', 0, true);  /* Greek capital sampi */
    weights.add_as(0x03E1, U'ϡ', 0, false); /* Greek small sampi */

    weights.add_as(0x0415, U'е', 0, true);  /* Cyrillic capital letter ie */
    weights.add_as(0x0435, U'е', 0, false); /* Cyrillic small letter ie */
    weights.add_as(0x0404, U'е', 1, true);  /* Cyrillic capital letter ukrainian ie */
    weights.add_as(0x0454, U'е', 1, false); /* Cyrillic small letter ukrainian ie */
    weights.add_as(0x0400, U'е', 2, true);  /* Cyrillic capital letter ie with grave */
    weights.add_as(0x0450, U'е', 2, false); /* Cyrillic small letter ie with grave */
    weights.add_as(0x04D6, U'е', 3, true);  /* Cyrillic capital letter ie with breve */
    weights.add_as(0x04D7, U'е', 4, false); /* Cyrillic small letter ie with breve */
    weights.add_as(0x0401, U'е', 4, true);  /* Cyrillic capital letter io */
    weights.add_as(0x0451, U'е', 4, false); /* Cyrillic small letter io */
    weights.add_as(0x0402, U'ђ', 0, true);  /* Cyrillic capital letter dje */
    weights.add_as(0x0452, U'ђ', 0, false); /* Cyrillic small letter dje */
    weights.add_as(0x0403, U'ђ', 1, true);  /* Cyrillic capital letter gje */
    weights.add_as(0x0453, U'ђ', 1, false); /* Cyrillic small letter gje */
    weights.add_as(0x0405, U'ѕ', 0, true);  /* Cyrillic capital letter dze */
    weights.add_as(0x0455, U'ѕ', 0, false); /* Cyrillic small letter dze */
    weights.add_as(0x04E0, U'ѕ', 1, true);  /* Cyrillic capital letter abkhasian dze */
    weights.add_as(0x04E1, U'ѕ', 1, false); /* Cyrillic small letter abkhasian dze */
    weights.add_as(0x0418, U'и', 0, true);  /* Cyrillic capital letter i */
    weights.add_as(0x0438, U'и', 0, false); /* Cyrillic small letter i */
    weights.add_as(0x0419, U'и', 1, true);  /* Cyrillic capital letter short i */
    weights.add_as(0x0439, U'и', 1, false); /* Cyrillic small letter short i */
    weights.add_as(0x0406, U'и', 2, true);  /* Cyrillic capital letter byelorussian-ukrainian i */
    weights.add_as(0x0456, U'и', 2, false); /* Cyrillic small letter byelorussian-ukrainian i */
    weights.add_as(0x040D, U'и', 3, true);  /* Cyrillic capital letter i with grave */
    weights.add_as(0x045D, U'и', 3, false); /* Cyrillic small letter i with grave */
    weights.add_as(0x04E2, U'и', 4, true);  /* Cyrillic capital letter i with macron */
    weights.add_as(0x04E3, U'и', 4, false); /* Cyrillic small letter i with macron */
    weights.add_as(0x04E4, U'и', 5, true);  /* Cyrillic capital letter i with diaeresis */
    weights.add_as(0x04E5, U'и', 5, false); /* Cyrillic small letter i with diaeresis */
    weights.add_as(0x0407, U'ї', 0, true);  /* Cyrillic capital letter yi */
    weights.add_as(0x0457, U'ї', 0, false); /* Cyrillic small letter yi */
    weights.add_as(0x0408, U'ј', 0, true);  /* Cyrillic capital letter je */
    weights.add_as(0x0458, U'ј', 0, false); /* Cyrillic small letter je */
    weights.add_as(0x0409, U'љ', 0, true);  /* Cyrillic capital letter lje */
    weights.add_as(0x0459, U'љ', 0, false); /* Cyrillic small letter lje */
    weights.add_as(0x040A, U'њ', 0, true);  /* Cyrillic capital letter nje */
    weights.add_as(0x045A, U'њ', 0, false); /* Cyrillic small letter nje */
    weights.add_as(0x040B, U'ћ', 0, true);  /* Cyrillic capital letter tshe */
    weights.add_as(0x045B, U'ћ', 0, false); /* Cyrillic small letter tshe */
    weights.add_as(0x040C, U'ќ', 0, true);  /* Cyrillic capital letter kje */
    weights.add_as(0x045C, U'ќ', 0, false); /* Cyrillic small letter kje */
    weights.add_as(0x0423, U'у', 0, true);  /* Cyrillic capital letter u */
    weights.add_as(0x0443, U'у', 0, false); /* Cyrillic small letter u */
    weights.add_as(0x040E, U'у', 1, true);  /* Cyrillic capital letter short u */
    weights.add_as(0x045E, U'у', 1, false); /* Cyrillic small letter short u */
    weights.add_as(0x04AE, U'у', 2, true);  /* Cyrillic capital letter straight u */
    weights.add_as(0x04AF, U'у', 2, false); /* Cyrillic small letter straight u */
    weights.add_as(0x04B0, U'у', 3, true);  /* Cyrillic capital letter straight u with stroke */
    weights.add_as(0x04B1, U'у', 3, false); /* Cyrillic small letter straight u with stroke */
    weights.add_as(0x04EE, U'у', 4, true);  /* Cyrillic capital letter u with macron */
    weights.add_as(0x04EF, U'у', 4, false); /* Cyrillic small letter u with macron */
    weights.add_as(0x04F0, U'у', 5, true);  /* Cyrillic capital letter u with diaeresis */
    weights.add_as(0x04F1, U'у', 5, false); /* Cyrillic small letter u with diaeresis */
    weights.add_as(0x04F2, U'у', 6, true);  /* Cyrillic capital letter u with double acute */
    weights.add_as(0x04F3, U'у', 6, false); /* Cyrillic small letter u with double acute */
    weights.add_as(0x040F, U'џ', 0, true);  /* Cyrillic capital letter dzhe */
    weights.add_as(0x045F, U'џ', 0, false); /* Cyrillic small letter dzhe */
    weights.add_as(0x0410, U'а', 0, true);  /* Cyrillic capital letter a */
    weights.add_as(0x0430, U'а', 0, false); /* Cyrillic small letter a */
    weights.add_as(0x04D0, U'а', 1, true);  /* Cyrillic capital letter a with breve */
    weights.add_as(0x04D1, U'а', 1, false); /* Cyrillic small letter a with breve */
    weights.add_as(0x04D2, U'а', 2, true);  /* Cyrillic capital letter a with diaeresis */
    weights.add_as(0x04D3, U'а', 2, false); /* Cyrillic small letter a with diaeresis */
    weights.add_as(0x0411, U'б', 0, true);  /* Cyrillic capital letter be */
    weights.add_as(0x0431, U'б', 0, false); /* Cyrillic small letter be */
    weights.add_as(0x0412, U'в', 0, true);  /* Cyrillic capital letter ve */
    weights.add_as(0x0432, U'в', 0, false); /* Cyrillic small letter ve */
    weights.add_as(0x0413, U'г', 0, true);  /* Cyrillic capital letter ghe */
    weights.add_as(0x0433, U'г', 0, false); /* Cyrillic small letter ghe */
    weights.add_as(0x0490, U'г', 1, true);  /* Cyrillic capital letter ghe with upturn */
    weights.add_as(0x0491, U'г', 1, false); /* Cyrillic small letter ghe with upturn */
    weights.add_as(0x0492, U'г', 2, true);  /* Cyrillic capital letter ghe with stroke */
    weights.add_as(0x0493, U'г', 2, false); /* Cyrillic small letter ghe with stroke */
    weights.add_as(0x0494, U'г', 3, true);  /* Cyrillic capital letter ghe with middle hook */
    weights.add_as(0x0495, U'г', 3, false); /* Cyrillic small letter ghe with middle hook */
    weights.add_as(0x0414, U'д', 0, true);  /* Cyrillic capital letter de */
    weights.add_as(0x0434, U'д', 0, false); /* Cyrillic small letter de */
    weights.add_as(0x0416, U'ж', 0, true);  /* Cyrillic capital letter zhe */
    weights.add_as(0x0436, U'ж', 0, false); /* Cyrillic small letter zhe */
    weights.add_as(0x0496, U'ж', 1, true);  /* Cyrillic capital letter zhe with descender */
    weights.add_as(0x0497, U'ж', 1, false); /* Cyrillic small letter zhe with descender */
    weights.add_as(0x04C1, U'ж', 2, true);  /* Cyrillic capital letter zhe with breve */
    weights.add_as(0x04C2, U'ж', 2, false); /* Cyrillic small letter zhe with breve */
    weights.add_as(0x04DC, U'ж', 3, true);  /* Cyrillic capital letter zhe with diaeresis */
    weights.add_as(0x04DD, U'ж', 3, false); /* Cyrillic small letter zhe with diaeresis */
    weights.add_as(0x0417, U'з', 0, true);  /* Cyrillic capital letter ze */
    weights.add_as(0x0437, U'з', 0, false); /* Cyrillic small letter ze */
    weights.add_as(0x0498, U'з', 1, true);  /* Cyrillic capital letter ze with descender */
    weights.add_as(0x0499, U'з', 1, false); /* Cyrillic small letter ze with descender */
    weights.add_as(0x04DE, U'з', 2, true);  /* Cyrillic capital letter ze with diaeresis */
    weights.add_as(0x04DF, U'з', 2, false); /* Cyrillic small letter ze with diaeresis */
    weights.add_as(0x041A, U'к', 0, true);  /* Cyrillic capital letter ka */
    weights.add_as(0x043A, U'к', 0, false); /* Cyrillic small letter ka */
    weights.add_as(0x049A, U'к', 1, true);  /* Cyrillic capital letter ka with descender */
    weights.add_as(0x049B, U'к', 1, false); /* Cyrillic small letter ka with descender */
    weights.add_as(0x049C, U'к', 2, true);  /* Cyrillic capital letter ka with vertical stroke */
    weights.add_as(0x049D, U'к', 2, false); /* Cyrillic small letter ka with vertical stroke */
    weights.add_as(0x049E, U'к', 3, true);  /* Cyrillic capital letter ka with stroke */
    weights.add_as(0x049F, U'к', 3, false); /* Cyrillic small letter ka with stroke */
    weights.add_as(0x04A0, U'к', 4, true);  /* Cyrillic capital letter bashkir ka */
    weights.add_as(0x04A1, U'к', 4, false); /* Cyrillic small letter bashkir ka */
    weights.add_as(0x04C3, U'к', 5, true);  /* Cyrillic capital letter ka with hook */
    weights.add_as(0x04C4, U'к', 5, false); /* Cyrillic small letter ka with hook */
    weights.add_as(0x041B, U'л', 0, true);  /* Cyrillic capital letter el */
    weights.add_as(0x043B, U'л', 0, false); /* Cyrillic small letter el */
    weights.add_as(0x041C, U'м', 0, true);  /* Cyrillic capital letter em */
    weights.add_as(0x043C, U'м', 0, false); /* Cyrillic small letter em */
    weights.add_as(0x041D, U'н', 0, true);  /* Cyrillic capital letter en */
    weights.add_as(0x043D, U'н', 0, false); /* Cyrillic small letter en */
    weights.add_as(0x04A2, U'н', 1, true);  /* Cyrillic capital letter en with descender */
    weights.add_as(0x04A3, U'н', 1, false); /* Cyrillic small letter en with descender */
    weights.add_as(0x04A4, U'н', 2, true);  /* Cyrillic capital ligature en ghe */
    weights.add_as(0x04A5, U'н', 2, false); /* Cyrillic small ligature en ghe */
    weights.add_as(0x04C7, U'н', 3, true);  /* Cyrillic capital letter en with hook */
    weights.add_as(0x04C8, U'н', 3, false); /* Cyrillic small letter en with hook */
    weights.add_as(0x041E, U'о', 0, true);  /* Cyrillic capital letter o */
    weights.add_as(0x043E, U'о', 0, false); /* Cyrillic small letter o */
    weights.add_as(0x04E6, U'о', 1, true);  /* Cyrillic capital letter o with diaeresis */
    weights.add_as(0x04E7, U'о', 1, false); /* Cyrillic small letter o with diaeresis */
    weights.add_as(0x04E8, U'о', 2, true);  /* Cyrillic capital letter barred o */
    weights.add_as(0x04E9, U'о', 2, false); /* Cyrillic small letter barred o */
    weights.add_as(0x04EA, U'о', 3, true);  /* Cyrillic capital letter barred o with diaeresis */
    weights.add_as(0x04EB, U'о', 3, false); /* Cyrillic small letter barred o with diaeresis */
    weights.add_as(0x041F, U'п', 0, true);  /* Cyrillic capital letter pe */
    weights.add_as(0x043F, U'п', 0, false); /* Cyrillic small letter pe */
    weights.add_as(0x04A6, U'п', 1, true);  /* Cyrillic capital letter pe with middle hook */
    weights.add_as(0x04A7, U'п', 1, false); /* Cyrillic small letter pe with middle hook */
    weights.add_as(0x0420, U'р', 0, true);  /* Cyrillic capital letter er */
    weights.add_as(0x0440, U'р', 0, false); /* Cyrillic small letter er */
    weights.add_as(0x0421, U'с', 0, true);  /* Cyrillic capital letter es */
    weights.add_as(0x0441, U'с', 0, false); /* Cyrillic small letter es */
    weights.add_as(0x04AA, U'с', 1, true);  /* Cyrillic capital letter es with descender */
    weights.add_as(0x04AB, U'с', 1, false); /* Cyrillic small letter es with descender */
    weights.add_as(0x0422, U'т', 0, true);  /* Cyrillic capital letter te */
    weights.add_as(0x0442, U'т', 0, false); /* Cyrillic small letter te */
    weights.add_as(0x04AC, U'т', 1, true);  /* Cyrillic capital letter te with descender */
    weights.add_as(0x04AD, U'т', 1, false); /* Cyrillic small letter te with descender */
    weights.add_as(0x0424, U'ф', 0, true);  /* Cyrillic capital letter ef */
    weights.add_as(0x0444, U'ф', 0, false); /* Cyrillic small letter ef */
    weights.add_as(0x0425, U'х', 0, true);  /* Cyrillic capital letter ha */
    weights.add_as(0x0445, U'х', 0, false); /* Cyrillic small letter ha */
    weights.add_as(0x04B2, U'х', 1, true);  /* Cyrillic capital letter ha with descender */
    weights.add_as(0x04B3, U'х', 1, false); /* Cyrillic small letter ha with descender */
    weights.add_as(0x0426, U'ц', 0, true);  /* Cyrillic capital letter tse */
    weights.add_as(0x0446, U'ц', 0, false); /* Cyrillic small letter tse */
    weights.add_as(0x04B4, U'ц', 1, true);  /* Cyrillic capital ligature te tse */
    weights.add_as(0x04B5, U'ц', 1, false); /* Cyrillic small ligature te tse */
    weights.add_as(0x0427, U'ч', 0, true);  /* Cyrillic capital letter che */
    weights.add_as(0x0447, U'ч', 0, false); /* Cyrillic small letter che */
    weights.add_as(0x04B6, U'ч', 1, true);  /* Cyrillic capital letter che with descender */
    weights.add_as(0x04B7, U'ч', 1, false); /* Cyrillic small letter che with descender */
    weights.add_as(0x04B8, U'ч', 2, true);  /* Cyrillic capital letter che with vertical stroke */
    weights.add_as(0x04B9, U'ч', 2, false); /* Cyrillic small letter che with vertical stroke */
    weights.add_as(0x04BC, U'ч', 3, true);  /* Cyrillic capital letter abkhasian che */
    weights.add_as(0x04BD, U'ч', 3, false); /* Cyrillic small letter abkhasian che */
    weights.add_as(
        0x04BE, U'ч', 4, true); /* Cyrillic capital letter abkhasian che with descender */
    weights.add_as(
        0x04BF, U'ч', 4, false);           /* Cyrillic small letter abkhasian che with descender */
    weights.add_as(0x04CB, U'ч', 5, true); /* Cyrillic capital letter khakassian che */
    weights.add_as(0x04CC, U'ч', 5, false); /* Cyrillic small letter khakassian che */
    weights.add_as(0x04F4, U'ч', 6, true);  /* Cyrillic capital letter che with diaeresis */
    weights.add_as(0x04F5, U'ч', 6, false); /* Cyrillic small letter che with diaeresis */
    weights.add_as(0x0428, U'ш', 0, true);  /* Cyrillic capital letter sha */
    weights.add_as(0x0448, U'ш', 0, false); /* Cyrillic small letter sha */
    weights.add_as(0x0429, U'щ', 0, true);  /* Cyrillic capital letter shcha */
    weights.add_as(0x0449, U'щ', 0, false); /* Cyrillic small letter shcha */
    weights.add_as(0x042A, U'ъ', 0, true);  /* Cyrillic capital letter hard sign */
    weights.add_as(0x044A, U'ъ', 0, false); /* Cyrillic small letter hard sign */
    weights.add_as(0x042B, U'ы', 0, true);  /* Cyrillic capital letter yeru */
    weights.add_as(0x044B, U'ы', 0, false); /* Cyrillic small letter yeru */
    weights.add_as(0x04F8, U'ы', 1, true);  /* Cyrillic capital letter yeru with diaeresis */
    weights.add_as(0x04F9, U'ы', 1, false); /* Cyrillic small letter yeru with diaeresis */
    weights.add_as(0x042C, U'ь', 0, true);  /* Cyrillic capital letter soft sign */
    weights.add_as(0x044C, U'ь', 0, false); /* Cyrillic small letter soft sign */
    weights.add_as(0x042D, U'э', 0, true);  /* Cyrillic capital letter e */
    weights.add_as(0x044D, U'э', 0, false); /* Cyrillic small letter e */
    weights.add_as(0x042E, U'ю', 0, true);  /* Cyrillic capital letter yu */
    weights.add_as(0x044E, U'ю', 0, false); /* Cyrillic small letter yu */
    weights.add_as(0x042F, U'я', 0, true);  /* Cyrillic capital letter ya */
    weights.add_as(0x044F, U'я', 0, false); /* Cyrillic small letter ya */
    weights.add_as(0x04A8, U'ҩ', 0, true);  /* Cyrillic capital letter abkhasian ha */
    weights.add_as(0x04A9, U'ҩ', 0, false); /* Cyrillic small letter abkhasian ha */
    weights.add_as(0x04BA, U'һ', 0, true);  /* Cyrillic capital letter shha */
    weights.add_as(0x04BB, U'һ', 0, false); /* Cyrillic small letter shha */
    weights.add_as(0x04C0, U'Ӏ', 0, true);  /* Cyrillic letter palochka */
    weights.add_as(0x04D4, U'ӕ', 0, true);  /* Cyrillic capital ligature a ie */
    weights.add_as(0x04D5, U'ӕ', 0, false); /* Cyrillic small ligature a ie */
    weights.add_as(0x04D8, U'ә', 0, true);  /* Cyrillic capital letter schwa */
    weights.add_as(0x04D9, U'ә', 0, false); /* Cyrillic small letter schwa */
    weights.add_as(0x04DA, U'ә', 1, true);  /* Cyrillic capital letter schwa with diaeresis */
    weights.add_as(0x04DB, U'ә', 1, false); /* Cyrillic small letter schwa with diaeresis */
    return weights;
  }();

  return weights;
}

static int bli_str_utf32_weight(const OrderWeight *weights,
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
    weight += weights->uppercase ? 1 : 0;
  }
  return weight;
}

struct Ligature {
  char32_t replace1 = 0;
  char32_t replace2 = 0;
  char32_t replace3 = 0;
  bool uppercase;
};

static const Map<uint32_t, Ligature> &ligatures()
{
  static const Map<uint32_t, Ligature> ligatures = []() {
    Map<uint32_t, Ligature> ligatures;
    ligatures.add_as(0x215B, U'1', U'/', U'8', false); /* Vulgar fraction one eighth. */
    ligatures.add_as(0x00BC, U'1', U'/', U'4', false); /* Vulgar fraction one quarter. */
    ligatures.add_as(0x215C, U'3', U'/', U'8', false); /* Vulgar fraction three eighths. */
    ligatures.add_as(0x00BD, U'1', U'/', U'2', false); /* Vulgar fraction one half. */
    ligatures.add_as(0x215D, U'3', U'/', U'5', false); /* Vulgar fraction three fifths. */
    ligatures.add_as(0x00BE, U'3', U'/', U'4', false); /* Vulgar fraction three quarters. */
    ligatures.add_as(0x215E, U'7', U'/', U'8', false); /* Vulgar fraction seven eighths. */
    ligatures.add_as(0x00C6, U'a', U'e', 0, true);     /* Latin capital letter AE */
    ligatures.add_as(0x00E6, U'a', U'e', 0, false);    /* Latin small letter AE */
    ligatures.add_as(0x00DF, U's', U's', 0, false);    /* Latin small letter sharp S. */
    ligatures.add_as(0x0132, U'i', U'j', 0, true);     /* Latin capital ligature FL. */
    ligatures.add_as(0x0133, U'i', U'j', 0, false);    /* Latin small ligature FL. */
    ligatures.add_as(0x0152, U'o', U'e', 0, true);     /* Latin capital ligature OE. */
    ligatures.add_as(0x0153, U'o', U'e', 0, false);    /* Latin small ligature OE. */
    ligatures.add_as(0x01E2, U'a', U'e', 0, true);     /* Latin capital letter AE with macron. */
    ligatures.add_as(0x01E3, U'a', U'e', 0, false);    /* Latin small letter AE with macron. */
    ligatures.add_as(0x01FC, U'a', U'e', 0, true);     /* Latin capital letter AE with acute. */
    ligatures.add_as(0x01FD, U'a', U'e', 0, false);    /* Latin small letter AE with acute. */
    ligatures.add_as(0xFB01, U'f', U'i', 0, false);    /* Latin small ligature FI. */
    ligatures.add_as(0xFB02, U'f', U'l', 0, false);    /* Latin small ligature FL. */
    ligatures.add_as(0x04A4, U'н', U'г', 0, true);     /* Cyrillic capital ligature EN GHE. */
    ligatures.add_as(0x04A5, U'н', U'г', 0, false);    /* Cyrillic small ligature EN GHE. */
    ligatures.add_as(0x04B4, U'т', U'ц', 0, true);     /* Cyrillic capital ligature TE TSE. */
    ligatures.add_as(0x04B5, U'т', U'ц', 0, false);    /* Cyrillic small ligature TE TSE. */
    ligatures.add_as(0x04D4, U'а', U'е', 0, true);     /* Cyrillic capital ligature A IE.  */
    ligatures.add_as(0x04D5, U'а', U'е', 0, false);    /* Cyrillic small ligature A IE.  */
    ligatures.add_as(0x2105, U'c', U'o', 0, false);    /* Care of. */
    ligatures.add_as(0x2116, U'n', U'o', 0, false);    /* Numero sign. */
    ligatures.add_as(0x2122, U't', U'm', 0, false);    /* Trade mark sign. */
    return ligatures;
  }();

  return ligatures;
}

std::string BLI_str_utf8_normalized(const blender::StringRef str, bool case_sensitive)
{
  std::string result;
  const size_t len = str.size();
  result.reserve(len);
  char utf8_buf[4];
  size_t utf8_buf_len = 0;
  const Ligature *ligature = nullptr;

  size_t i = 0;
  while (i < len && str[i]) {
    char32_t wc = BLI_str_utf8_as_unicode_step_safe(str.data(), len, &i);
    ligature = ligatures().lookup_ptr(wc);
    if (ligature) {
      const bool ucase = case_sensitive && ligature->uppercase;
      utf8_buf_len = BLI_str_utf8_from_unicode(
          ucase ? BLI_str_utf32_char_to_upper(ligature->replace1) : ligature->replace1,
          utf8_buf,
          sizeof(utf8_buf));
      result.append(utf8_buf, utf8_buf_len);
      utf8_buf_len = BLI_str_utf8_from_unicode(
          ucase ? BLI_str_utf32_char_to_upper(ligature->replace2) : ligature->replace2,
          utf8_buf,
          sizeof(utf8_buf));
      result.append(utf8_buf, utf8_buf_len);
      if (ligature->replace3) {
        utf8_buf_len = BLI_str_utf8_from_unicode(
            ucase ? BLI_str_utf32_char_to_upper(ligature->replace3) : ligature->replace3,
            utf8_buf,
            sizeof(utf8_buf));
        result.append(utf8_buf, utf8_buf_len);
      }
      continue;
    }

    const OrderWeight *weight = weights().lookup_ptr(wc);

    const bool ucase = case_sensitive && weight && weight->uppercase;
    int normalized = weight ? bli_str_utf32_weight(weight, false, false) : wc;
    if (weight && normalized == 0) {
      normalized = wc; /* For search use original code point if no weight is defined. */
    }
    if (!weight && mk_wcwidth(wc) < 1) {
      normalized = 0; /* No weight for combining characters. */
    }

    if (normalized) {
      utf8_buf_len = BLI_str_utf8_from_unicode(ucase ? BLI_str_utf32_char_to_upper(normalized) :
                                                       normalized,
                                               utf8_buf,
                                               sizeof(utf8_buf));
      result.append(utf8_buf, utf8_buf_len);
    }
  }

  result.shrink_to_fit();
  return result;
}

/** \} */

}  // namespace blender
