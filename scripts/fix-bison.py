#!/usr/bin/python3
"""
Modify files generated by bison to address potential issues and warnings.

The script performs the following tasks:

* Removes code that causes warnings about unused variables and functions.
* Fixes line numbers after filename renaming.
* Sets default values for variables in specific Bison versions.
* Modifies code for compatibility with different system library implementations.

Usage:
  bison -b foo_parser -p foo_parser_ -d -v foo_parser.y
  python3 fix-bison.pl foo_parser.tab.c

"""

import os
import re
import sys

EXTENSION = ".bak"


def fix(file):
    backup = file + EXTENSION
    os.rename(file, backup)
    with open(backup, "r") as infile, open(file, "w") as outfile:
        seen_yyerrlab1 = False
        syntax_error_has_default = False
        line_offset = 1

        # Read entire source lines
        s = list(enumerate(infile, start=1))
        while s:
            (line_number, line) = s.pop(0)
            # Remove block of code that causes a warning
            if "Suppress GCC warning that yyerrlab1" in line:
                while True:
                    line_offset -= 1  # Skip line
                    (line_number, next_line) = s.pop(0)
                    if next_line.startswith("#endif"):
                        line_offset -= 1  # Skip line
                        break
                # do not emit any of the lines in this block
                continue

            if "goto yyerrlab1" in line:
                seen_yyerrlab1 = True

            if not seen_yyerrlab1:
                line = re.sub(r"^yyerrlab1:", "", line)

            # Do not use macro name for a temporary variable
            line = line.replace(
                "unsigned int yylineno = ", "unsigned int yylineno_tmp = "
            )
            line = line.replace("yyrule - 1, yylineno)", "yyrule - 1, yylineno_tmp)")

            # Do not (re)define prototypes that the system did better
            if re.match(r"^void \*malloc\s*\(.*\)", line):
                line_offset -= 1  # skipped a line
                continue
            if re.match(r"^void free\s*\(.*\)", line):
                line_offset -= 1  # skipped a line
                continue

            # syntax error handler will have a default case already in Bison
            # 3.0.5+
            if "default: /* Avoid compiler warnings. */" in line:
                syntax_error_has_default = True

            if re.match(r"# undef YYCASE_\$", line) and not syntax_error_has_default:
                # Add a default value for yyformat on Bison <3.0.5, for
                # coverity CID 10838
                outfile.write('      default: yyformat = YY_("syntax error")\n')
                line_offset += 1  # extra line
                outfile.write(line)
                continue

            if "yysyntax_error_status = YYSYNTAX_ERROR" in line:
                # Set yytoken to non-negative value for coverity CID 29259
                outfile.write("if(yytoken < 0) yytoken = YYUNDEFTOK\n")
                line_offset += 1  # extra line
                outfile.write(line)
                continue

            # Suppress warnings about empty declarations
            line = re.sub(r"^(static int .*_init_globals.*);$", r"\1/", line)

            # Remove always false condition
            if "if (/*CONSTCOND*/ 0)" in line:
                line_offset -= 1  # skipped a line
                (line_number, line) = s.pop(0)
                line_offset -= 1  # skipped a line
                continue

            # Remove always false condition; this macro is #defined to 0
            if "(yytable_value_is_error (yyn)" in line:
                line_offset -= 1  # skipped a line
                (line_number, line) = s.pop(0)
                line_offset -= 1  # skipped a line
                continue

            # Fixup pending filename renaming, see above.
            # Fix line numbers.
            line = re.sub(
                r"^\#line \d+ (.*\.c)", rf"#line {line_number + line_offset} \1", line
            )

            # Remove all mention of unused var yynerrs
            line = re.sub(r"^(\s*)(.*yynerrs.*)", r"\1/* \2 */", line)
            outfile.write(line)


def main():
    for file in sys.argv[1:]:
        fix(file)


if __name__ == "__main__":
    main()