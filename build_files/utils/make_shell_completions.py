#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

'''
This script generates the shell completions for Blender based on the --help
output from a blender executable. Invoke it as follows:

    make_shell_completions.py --blender <path-to-blender>\
                              --shell   <completion-flavor>\
                              --output  <output-filename>

Where:
<path-to-blender>   is the path to the Blender executable,
<completion-flavor> is the shell syntax in which completions are generated
                    (can be 'zsh' or 'bash' currently)
<output-filename>   is the path to write the generated completion script.
'''

import argparse
import os
import sys
import subprocess
import time
import re
import logging
import shlex
from collections import defaultdict
from dataclasses import dataclass, field
from typing import (
    Dict,
    TextIO,
    List,
    Optional,
    Union,
)

# prettify log output
class BasicColorlogFormatter(logging.Formatter):
    def __init__(self, include_filelines=False):
        super().__init__()

        levelname = "{levelname:5}"
        fileline = " {filename}:{lineno}" if include_filelines else ""

        dim      = "\033[2m"
        blue     = "\033[34m"
        yellow   = "\033[93m"
        red      = "\033[31m"
        bold_red = "\033[31;1m"
        reset    = "\033[0m"

        self.colorized_formatters = {l: logging.Formatter(f, style='{') for l, f in {
            logging.DEBUG:    dim     +levelname+fileline+reset+" {message}",
            logging.INFO:     blue    +levelname+reset+dim+fileline+reset+" {message}",
            logging.WARNING:  yellow  +levelname+reset+dim+fileline+reset+" {message}",
            logging.ERROR:    red     +levelname+reset+dim+fileline+reset+" {message}",
            logging.CRITICAL: bold_red+levelname+fileline+" {message}"+reset,
        }.items()}

    def format(self, record):
        return self.colorized_formatters[record.levelno].format(record)

log = logging.getLogger(__name__)

# utility functions
def isblank(string: str) -> bool:
    return len(string.strip()) == 0

def filterjoin(items, sep:str=" "):
    return sep.join(filter(lambda x: x is not None and len(x) != 0, items))

def backslash_escape(string: str, characters: str) -> str:
    string = string.replace("\\", "\\\\")
    for char in characters:
        string = string.replace(char, "\\" + char)
    return string

def strip_all(string: str, chars: str):
    return "".join(c for c in string if c not in chars)

def surround(string: str, start: str, end: str, strip_conflicts=False) -> str:
    if strip_conflicts:
        string = strip_all(string, start+end)
    if len(string) > 0:
        return start + string + end
    else:
        return ""

# we try to continue as much as possible even under unexpected conditions but where that's not possible we'll throw this
class CompletionGenerationError(RuntimeError):
    """an error which indicates completion generation was impossible"""

# below we define an abstract representation of blender's cli and shell-agnostic specifications on how to generation
# completions to arguments

class CompletionSpec:
    """a specification for providing values to complete an argument"""

@dataclass
class BlenderArgument:
    """represents a positional argument, such as [file] or an argument to an option"""
    name: str
    completion_spec: Optional[CompletionSpec] = None
    optional: bool = False

    def __repr__(self):
        return self.name

@dataclass
class BlenderOption:
    """represents a command line option typically signified with a -short and/or --long flag"""

    category:    Optional[str]
    short:       Optional[str]
    long:        Optional[str]
    args:        List[BlenderArgument]
    doc_lines:   List[str]

    # Indicates that no further completion should take place if this option is already present (think --help, --version)
    excludes_all: bool = False

    # If this option can be meaningfully repeated; otherwise it should not be considered for completion after it is
    # already present
    repeatable: bool = False

    # List of options or categories which should not be considered for completion if this option is present
    excludes: List[Union[str,'BlenderOption']] = field(default_factory=list, init=False)

    def __repr__(self):
        return filterjoin(
            (self.short, self.long, " ".join(map(str, self.args)))
        ) + ": " + self.doc_lines[0] if len(self.doc_lines) > 0 else ""

@dataclass
class PathCompletion(CompletionSpec):
    """complete using standard path completion"""
    file_extension: Optional[str] = None
    dir_only: bool = False

class BPathCompletion(PathCompletion):
    """complete paths, with awareness of blender-style //file/relative/paths"""

@dataclass
class WordlistCompletion(CompletionSpec):
    """
    a static list of words to offer for completion.
    a dict may be used to provide descriptions for each word (keyed by word).
    """
    items: list[str] | dict[str, str]

@dataclass
class OptionCompletion(CompletionSpec):
    """complete using sub options"""
    options: List['BlenderOption']

@dataclass
class CommandCompletion(CompletionSpec):
    """a command which prints a newline separated list of completeable values on stdout"""
    command_line: list[str]

    # prefix to filter the list with.
    #
    # we use this hack for completion commands which launch blender in the background and capture stdout, since there's
    # a risk other (unwanted) output could also end up in the same stream.  we could alternatively set up a fifo or
    # other form of ipc, but this is simply easier.
    #
    # it should be noted that an addon *could* print with this same prefix to hijack suggestions, but we also run
    # blender with --factory-startup and the only thing an addon would accomplish is inserting extra values into the
    # possible completions
    prefix: Optional[str] = None

def extract_blender_info(blender_bin: str) -> Dict[str, Union[str, str]]:
    """run blender and capture output of --help and --version"""

    blender_env = {
        "ASAN_OPTIONS": "exitcode=0:" + os.environ.get("ASAN_OPTIONS", ""),
    }

    log.debug("reading --help message")
    blender_help = subprocess.run(
        [blender_bin, "--help"],
        env=blender_env,
        check=True,
        stdout=subprocess.PIPE,
    ).stdout.decode(encoding="utf-8")

    log.debug("reading --version message")
    blender_version_output = subprocess.run(
        [blender_bin, "--version"],
        env=blender_env,
        check=True,
        stdout=subprocess.PIPE,
    ).stdout.decode(encoding="utf-8")

    # Extract information from the version string.
    # Note that some internal modules may print errors (e.g. color management),
    # check for each lines prefix to ensure these aren't included.
    blender_version = ""
    blender_date = ""
    blender_hash = ""
    for l in blender_version_output.split("\n"):
        if l.startswith("Blender "):
            # Remove 'Blender' prefix.
            blender_version = l.split(" ", 1)[1].strip()
        elif l.lstrip().startswith("build date:"):
            # Remove 'build date:' prefix.
            blender_date = l.split(":", 1)[1].strip()
        elif l.lstrip().startswith("build hash:"):
            # Remove 'build date:' prefix.
            blender_hash = l.split(":", 1)[1].strip()
        if blender_version and blender_date and blender_hash:
            break

    if not blender_date:
        # Happens when built without WITH_BUILD_INFO e.g.
        date_string = time.strftime("%B %d, %Y", time.gmtime(int(os.environ.get('SOURCE_DATE_EPOCH',
                                                                                time.time()))))
    else:
        date_string = time.strftime("%B %d, %Y", time.strptime(blender_date, "%Y-%m-%d"))

    return {
        "help": blender_help,
        "version": blender_version,
        "commit": blender_hash,
        "date": date_string,
    }


def scene_completion() -> CommandCompletion:
    return CommandCompletion([])

def engine_completion() -> CommandCompletion:
    return CommandCompletion([])

def addon_completion() -> CommandCompletion:
    # print names of addons on stdout with a prefix to avoid catching other noise
    python_expr_print_addons = \
    'import bpy, addon_utils;'+\
    '[print(f"_SHELL_COMPLETION:{x}") for x in '+\
    '(x.__name__ for x in addon_utils.modules())'

    # to print disabled addons
    # 'set(x.__name__ for x in addon_utils.modules()) - '+\
    # 'set(bpy.context.preferences.addons.keys())]'

    # use --factory-startup as a security precaution
    # since tab completion may not be expected to execute blender and addons
    args = [
        "blender", # should be replaced with $0 in shell-specific code to call the blender binary we're completing for
        "--factory-startup", 
        "--background",
        "--python-expr",
        shlex.quote(python_expr_print_addons)
    ]
    return CommandCompletion(args, prefix="_SHELL_COMPLETION")


# def zsh_addon_plugin():
    # args = list_addons_cmd()
    # args.insert(0, "2>/dev/null")
    # print("disabled_addons=(${(MA)$(%s):#COMPLETION_FOR_DISABLED_ADDONS*})" % ' '.join(args))
    # print("for addon in ${disabled_addons}; do print -r ${addon##COMPLETION_FOR_DISABLED_ADDONS:}; done")

def check_syntax(shell_flavor: str, filepath: str):
    cmd = [shell_flavor, '-n', filepath]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        raise CompletionGenerationError(f"{cmd} had non-zero exit: {proc.returncode}\n{proc.stderr}")

# there is also an "/?" option (help on windows) which is left out as we only look at options starting with "-" to begin
# with
def is_windows_only(option: BlenderOption) -> bool:
    return "Windows only" in "\n".join(option.doc_lines)

def parse_options_from_help(help_text: str) -> List[BlenderOption]:
    current_category = None
    current_option = None
    consecutive_blank_count = 0

    all_options = []

    log.debug("parsing help")

    lines = help_text.splitlines()
    for n, line in enumerate(lines):
        try:
            if not line.startswith('-'):
                # Collect category from lines like "Render Options:"
                m = re.match(r"^(?P<cat_name>.+) Options:$", line)
                if m:
                    current_category = m.group("cat_name")
                    log.debug("parsed category: %s", current_category)
                continue


            m = re.match(r"^"
                         # Match a short option, if it exists. Note some short
                         # options are longer than one char (-setaudio)
                         # use a lookahead to handle -- as a short option
                         r"(?P<short>-(?:[a-zA-Z0-9]+|-(?=\s)))?"
                         # Skip over the string " or " if it exists
                         r"(?: or )?"
                         # Match long option, if there is one
                         r"(?P<long>--\S+)?"
                         # Skip one whitespace character if there is one
                         r"\s?"
                         # If there's more non-whitespace, capture the in the args group 
                         # discard trailing whitespace
                         r"(?P<args>.+)?\S*$", line)

            if m is None:
                raise CompletionGenerationError("Regex didn't match on hyphen line")

            short = m.group("short")
            long  = m.group("long")
            args  = m.group("args")

            # Sanity checks
            if short is not None:
                if len(short.split()) > 1:
                    raise CompletionGenerationError(f"Parsed short option '{short}' didn't pass sanity check")

            if long is not None:
                if len(long.split()) > 1:
                    raise CompletionGenerationError(f"Parsed long option '{short}' didn't pass sanity check")

            if args is not None:
                args = args.split()
                for arg in args:
                    if not arg.startswith('<') or not arg.endswith('>'):
                        raise CompletionGenerationError(f"Parsed arg '{arg}' didn't pass sanity check")
            else:
                args = []

            if current_category is None:
                raise CompletionGenerationError("Option not expected outside of category")

            doc_lines = []

            consecutive_blank_count = 0

            for line in lines[n+1:]:
                if isblank(line):
                    consecutive_blank_count += 1
                else:
                    consecutive_blank_count = 0

                if consecutive_blank_count >= 2:
                    doc_lines.pop() # remove last blank
                    break

                if line.startswith("-") or line.startswith("/"):
                     break

                doc_lines.append(line.strip())

            current_option = BlenderOption(
                current_category,
                short,
                long,
                [BlenderArgument(name) for name in args],
                doc_lines)

            if all(map(isblank, doc_lines)):
                log.warn("No documentation detected for option %s", current_option)

            all_options.append(current_option)
            log.debug("parsed argument: %s", current_option)


        except CompletionGenerationError as e:
            raise CompletionGenerationError(f"Argument parse failed at line {n}: {line}") from e

    return all_options

def write_comment_header(blender_info: dict, output_file: TextIO) -> None:
    now = time.asctime()

    msg1 = "This file was autogenerated by build_files/utils/make_completions.py" # type: ignore
    msg2 = f"on {now} for blender {blender_info['version']} ({blender_info['commit']})"

    msg_width = max(map(len, (msg1, msg2)))
    box_width = msg_width+2

    output_file.write("\n")
    output_file.write(f"# .{'-'*box_width}.\n")
    output_file.write(f"# |{' '*box_width}|\n")
    output_file.write(f"# | {msg1:{msg_width}} |\n")
    output_file.write(f"# | {msg2:{msg_width}} |\n")
    output_file.write(f"# |{' '*box_width}|\n")
    output_file.write(f"# '{' edits may be overwritten ':-^{box_width}}'\n")
    output_file.write("\n")


def make_bash(blender_info: dict,
              arguments: List[BlenderArgument],
              options: List[BlenderOption],
              output_file: TextIO) -> None:
    log.debug("generating bash completions")
    output_file.write("# blender completion\n")
    write_comment_header(blender_info, output_file)

    output_file.write("_blender() {\n")
    output_file.write("  local cur prev words cword split\n")
    output_file.write("  _init_completion -s || return\n")
    output_file.write("\n")

    output_file.write("  short=(\n")
    for op in options:
        if op.short:
            output_file.write(f"    {op.short}\n")
    output_file.write("  )\n\n")

    output_file.write("  long=(\n")
    for op in options:
        if op.long:
            output_file.write(f"    {op.long}\n")
    output_file.write("  )\n\n")

    output_file.write("  if [[ $cur == --* ]]; then\n")
    output_file.write('     COMPREPLY=($(compgen -W "${long[*]}" -- "$cur"))\n')
    output_file.write("  elif [[ $cur == -* ]]; then\n")
    output_file.write('     COMPREPLY=($(compgen -W "${short[*]}" -- "$cur"))\n')
    output_file.write('     COMPREPLY+=($(compgen -W "${long[*]}" -- "$cur"))\n')
    output_file.write("  else\n")
    output_file.write('     _filedir blend\n')
    output_file.write("  fi\n")
    output_file.write("}\n\n")
    output_file.write("complete -F _blender blender\n")
    

def make_zsh(blender_info: dict,
             arguments: List[BlenderArgument],
             options: List[BlenderOption],
             output_file: TextIO,
             extended_explanations=False) -> None:

    log.debug("generating zsh completions")

    # we will use the zsh _arguments function, documented in zshcompsys(1), to provide completions.  _arguments lets us
    # describe pretty rich completion behavior via "specs", where each spec is written as single argument to _arguments.
    # these specs are documented in detail under specs: overview, but briefly, specs can be broken down into these two
    # sub specs:
    #
    # - argspec: a positional argument, in the form
    #
    #      :message:action
    #
    #   where `message` is a short text to display while showing possible completions, and `action` describes how to
    #   generate those completions. an example argspec looks like:
    #
    #      :blender file:_files -g "*.blend"
    #
    # - opspec: an option, in the form
    #
    #      (excludes)--optname[explanation]
    #
    #   where the parenthesized list of option names (can also contain argument numbers) are completions to be excluded
    #   from consideration when --optname is present on the command line.  this list is optional but we'll use it often
    #   because we'll also use a brace expansion to match long and short options together, and while matched optnames
    #   are normally excluded (unless the optspec is prefixed with an *), this does not exclude all the other possible
    #   matches to the brace expansion. an example of an optspec:
    #
    #      (-excluded --option-names)--optname[explanation]
    # 
    #   if an option itself takes an arguments, argspecs can be appended to the end of the opspec, like
    #
    #      (-excluded --option-names)--optname[explanation]:message:action:message2:action2
    #
    # in addition, we will use groups (described under Grouping Options) to group optspecs by category to make excluding
    # whole categories more readable. groups are introduced with an argument preceded by a '+'. 
    #
    # there are other complexities (which i will try to explain as we come accross them) but hopefully if you're reading
    # this, i've helped you find your bearings at least a little!

    # utility lambdas
    ignore_none = lambda x: x is not None

    # group names can't have whitespace
    group_safe = lambda n: strip_all(n, ' \t')

    # zshcompsys(1) specifies that colons in optname, explanation, or action must be backslash escaped. from testing,
    # square braces [] can also be backslash escaped.
    # further, as explained in zshcompwid(1), we should escape % as %% to avoid unintended expansions by compadd
    explanation_safe = lambda x: backslash_escape(x, '[]:').replace('%', '%%')

    # this function renders an argument as a spec, described briefly above and documented in zshcompsys(1) under specs:
    # overview.
    # extended_doc_lines is a list of strings which, if extended_explanations is set, will be added to `message`
    # this can be used to add extra information from (e.g.) a preceding option's help during argument completion
    def make_argspec(arg: BlenderArgument, extended_doc_lines:List[str] = []) -> str:
        arg_prefix = ':'
        message = ''
        action = ' '

        # starting an argspec with a double colon '::' indicates that it's optional
        if arg.optional:
            arg_prefix = '::'

        # with extended explanations, we cram all the help text into message.  this looks a little ugly in the file
        # since there's no way to encode newlines other than literally, but it works and can be pretty helpful.
        if extended_explanations and len(extended_doc_lines)>0:
            ansi_clear = '\033[0m'
            ansi_dim = '\033[2m'
            message = f"{arg.name}\\:" +\
                ansi_clear+ansi_dim+'\n'+\
                "\n  ".join(map(lambda l: backslash_escape(l, '[]:'), extended_doc_lines)) +\
                ansi_clear
        else:
            message = arg.name

        match arg.completion_spec:
            # case BPathCompletion() as spec:

            case PathCompletion() as spec:
                action = "_files"
                if spec.file_extension is not None:
                    action = f'_files -g "*.{spec.file_extension}"'
                if spec.dir_only:
                    action = '_files -/'

            case WordlistCompletion(items):
                if isinstance(items, list):
                    # if we just have items without explanations; _arguments expects these as simply (foo bar baz)
                    action = surround(" ".join(items), '(', ')', strip_conflicts=True)
                elif isinstance(items, dict):
                    # if wordlist has explanations per word; _arguments expects these in the form ((a\:bar b\:baz)),
                    # backslash included
                    action = surround(" ".join(
                        strip_all(item, ': \t') + '\\:' +\
                        explanation_safe(strip_all(explanation, ': \t'))
                        for item, explanation in items.items()
                    ), '((', '))', strip_conflicts=True)
                else:
                    log.warning("wordlist items not a list or dict")
                    action = ""

            case OptionCompletion(subopts):
                action = '_arguments -a -b -c'
                pass

            case None:
                # a single space indicates no completions can be generated, but the message will still be displayed
                # this is useful for options like --frame-start
                action = ' '

            case _:
                action = ' '
                log.warning("generation not implemented for completion spec %s",
                            arg.completion_spec.__class__.__name__)

        return arg_prefix + message + ':' + action

    # now we actually start writing the file
    output_file.write("#compdef blender\n")
    write_comment_header(blender_info, output_file)
    output_file.write("local ret=1\n") # TODO explain this
    output_file.write("local -a context state state_descr line\n") # TODO explain this
    output_file.write("\n")
    # output_file.write("typeset -A opt_args\n")

    # -S: do not continue completion after --
    # -C: set curcontext (TODO)
    output_file.write("_arguments -S -C\\\n")

    # at the time of writing blender accepts one positional argument, that being a path to a .blend file
    # its spec will look something like ::Blender File:_files -g "*.blend", following the format
    # ::message:action (two leading colons indicates an optional argument)
    for arg in arguments:
        output_file.write(f"  '{make_argspec(arg)}'\\\n")

    # on to the options, which start with - or --.

    # it'll be useful to track optnames which collide between different options
    optname_collisions = defaultdict(lambda: -1)
    for opt in options:
        optname_collisions[opt.short] += 1
        optname_collisions[opt.long] += 1

    # we'll track which category the previous argument belonged to so we can make changes on category boundaries
    previous_category = None

    # group options by category and ensure uncategorized options come first
    for opt in sorted(options, key=lambda o: (o.category is not None, o.category)):

        # it will be convenient for us to inform the _arguments function of each option category. this will make it is
        # easy to exclude a whole category at once from future matching. _arguments allows groups to be introduced with
        # a plus followed by the group name
        if opt.category != previous_category and opt.category is not None:
            output_file.write(f"  + {group_safe(opt.category)} \\\n")

        output_file.write("    ") # indentation

        # the _arguments function expects each optspec formatted like this:
        #   (-excluded --option-names)--optname[explanation]
        #
        # where the parenthesized list of option names (can also contain argument numbers) are completions to be
        # excluded from consideration when --optname is present on the command line.
        #
        # this list is optional but we'll almost always use it because we'll also use a brace expansion to match long
        # and short options together, and while matched optnames are normally excluded (unless the optspec is prefixed
        # with an *), this does not exclude all the other possible matches to the brace expansion.
        #
        # TL;DR, typically we have a long and a short optname for a single option which cannot be meaningfully repeated.
        # to represent to _arguments we'll do this:
        #   (-c --completable-tokens){-c,--completable-tokens}[explanation]

        prefix = ""
        excludes = set()
        optnames: list[str] = list(filter(ignore_none, (opt.short, opt.long))) # type: ignore
        explanation = ""


        # if this option has multiple optnames we use a brace expansion to cover both
        if len(optnames)>1:
            optname = surround(','.join(optnames), '{', '}')

            # so as explained above we'll use the exclude list to prevent repeat completions where applicable
            if not opt.repeatable:
                # inside the excludes list, _arguments lets us clarify inter-group optname conflicts by specifying the
                # exclude as "group-optname"
                excludes.update(o if optname_collisions[o] < 1 else f"{group_safe(opt.category)}-{o}"
                                for o in optnames)
                
        # if there's only one optname we don't need the brace expansion
        else:
            optname = optnames[0]

        # and in either case, if applicable, we prefix with * to tell _arguments the option is repeatable
        if opt.repeatable:
            prefix = optname

        # exclude all arguments from further completion (used for options such as --help)
        if opt.excludes_all:
            # to do this we'll use these special tokens _arguments accepts in the exclude list;
            # *: exclude all "rest" arguments (repeatable positional arguments prefixed with '*')
            # -: exclude all option arguments
            # an argument number, starting at 1: exclude the positional argument in that position
            excludes.update(('*', '-', ' '.join(str(n+1) for n in range(len(arguments)))))
            # we no longer need to exclude our own optnames anymore (if we were); we may as well keep things tidy
            excludes.difference_update(optnames)

        # add explicit excludes
        for exclude in opt.excludes:
            # 
            if isinstance(exclude, str):
                excludes.add(group_safe(exclude))
            else:
                excludes.update(o if optname_collisions[o] < 1 else f"{group_safe(exclude.category)}-{o}" # type: ignore
                                for o in filter(ignore_none, (exclude.short, exclude.long)))

        if len(opt.doc_lines)>0:
            explanation = opt.doc_lines[0]


        # if the option takes arguments, we can communicate this to _arguments by appending optarg specs like this:
        # optspec:message:action:message2:action2
        arglist_str = ""
        for arg in opt.args:
            arglist_str += make_argspec(arg, opt.doc_lines)

        if len(excludes) > 0:
            excludes_str = surround(" ".join(sorted(excludes, reverse=True)), '(', ')')
        else:
            excludes_str = ""

        if not isblank(explanation):
            explanation_str = surround(explanation_safe(explanation), '[', ']')
        else:
            explanation_str = ""

        # _arguments expects each optspec as a single argument, so both the exclude list and the explanation need to
        # be quoted to avoid being subject to unintended expansions
        output_file.write(shlex.quote(prefix + excludes_str) + optname + shlex.quote(explanation_str + arglist_str))
        output_file.write("\\\n")

        previous_category = opt.category

    # output_file.write("  '(-)*:: :->args'")
    output_file.write("&& ret=0\n")
    # output_file.write("\n")
    # output_file.write("case $state in\n")
    # output_file.write("args)\n")
    # output_file.write("  if [[ -n ${opt_args[(i)s--[cs]]} ]]; then\n")
    # # output_file.write("    _files && ret=0\n")
    # output_file.write("    ret=0\n")
    # output_file.write("  else\n")
    # output_file.write("    _normal && ret=0\n")
    # output_file.write("  fi\n")
    # output_file.write(";;\n")
    # output_file.write("esac\n")

    output_file.write("\n\nreturn ret\n")


def create_argparse() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
    description="This script generates shell completions from --help output from a blender executable."
    )
    parser.add_argument(
        "--blender",
        metavar="PATH",
        required=True,
        help="path to a blender binary."
    )
    parser.add_argument(
        "--shell",
        metavar="FLAVOR",
        required=True,
        choices=('bash', 'zsh'),
        help="the shell flavor for which to generate completions ('bash' or 'zsh')."
    )
    parser.add_argument(
        "--output",
        metavar="PATH",
        required=True,
        help="file to write completion script to."
    )
    parser.add_argument(
        "--verbose",
        default=False,
        required=False,
        action='store_true',
        help="print additional progress."
    )
    parser.add_argument(
        "--extended-explanations",
        default=False,
        required=False,
        action='store_true',
        help="generate completions with embedded help messages (zsh only, unconventional and somewhat hackish)"
    )

    return parser

def extract_arg_order_help_lines(blender_help: str) -> List[str]:
    help_lines = blender_help.splitlines()
    try:
        start = help_lines.index("Argument Order:")
    except ValueError as e:
        raise CompletionGenerationError("Couldn't find Argument Order section") from e

    for n, line in enumerate(help_lines[start:]):
        if isblank(line):
            end = n
            break
    else:
        raise CompletionGenerationError("Couldn't parse Argument Order section")

    return help_lines[start:start+end]


def main() -> None:
    _log = logging.StreamHandler()
    log.addHandler(_log)

    parser = create_argparse()
    args = parser.parse_args()

    if args.verbose:
        log.setLevel(logging.DEBUG)
        _log.setFormatter(BasicColorlogFormatter(include_filelines=True))
    else:
        _log.setFormatter(BasicColorlogFormatter(include_filelines=False))
        log.setLevel(logging.INFO)

    completion_flavors = {
        "zsh": make_zsh,
        "bash": make_bash
    }

    blender_info = extract_blender_info(args.blender)
    blender_options = parse_options_from_help(blender_info["help"])
    blender_arg_order_help_lines = extract_arg_order_help_lines(blender_info["help"])

    # there's just one positional argument
    blender_args = [BlenderArgument("Blender File", PathCompletion('blend'), optional=True)]

    # go through each option to provide extra information not parsable from the help message, which will let us spruce
    # up option completions a bit.  if blender's cli changes, chances are the code below will be what breaks, so we try
    # to write defensively as possible and be verbose if we encounter anything unexpected. ideally changes to blender's
    # help text will only require changes below and will not require changes to shell-specific functions make_*.
    for option in blender_options[:]:
        # remove windows-only options
        if is_windows_only(option):
            blender_options.remove(option)
            log.debug("ignoring windows-only option %s", option)

        # we don't really need to complete this one either
        if option.short == '--':
            blender_options.remove(option)

        # mark exclusive options
        if option.long == "--help" or option.long == "--version":
            option.excludes_all = True

        # set completion handlers where possible
        for arg in option.args:
            if arg.name == '<filepath>':
                arg.completion_spec = PathCompletion()
            if arg.name == '<path>':
                arg.completion_spec = PathCompletion(dir_only=True)

        if option.long == "--render-output":
            try:
                option.args[0].completion_spec = BPathCompletion()
            except IndexError as e:
                log.warning("expected --render-output to take an argument", exc_info=e)

        if option.long == "--python":
            try:
                option.args[0].completion_spec = PathCompletion('py')
            except IndexError as e:
                log.warning("expected --python to take an argument", exc_info=e)

        if option.long == "--engine":
            try:
                option.args[0].completion_spec = engine_completion()
            except IndexError as e:
                log.warning("expected --engine to take an argument", exc_info=e)

        if option.long == "--scene":
            try:
                option.args[0].completion_spec = scene_completion()
            except IndexError as e:
                log.warning("expected --scene to take an argument", exc_info=e)

        if option.long == "--addons":
            try:
                option.args[0].completion_spec = addon_completion()
            except IndexError as e:
                log.warning("expected --addons to take an argument", exc_info=e)

        # collect valid sound devices from list in help text, remove list from help text
        if option.short == "-setaudio":
            if len(option.args) == 0:
                try:
                    valid_devices = list(map(
                        lambda s: s.strip("'."), option.doc_lines[1].split()
                    ))
                    del option.doc_lines[1]
                    assert(len(valid_devices) > 0)
                except (IndexError, AssertionError) as e:
                    log.warning("failed to extract valid sound systems "+\
                                "from help string for -setaudio", exc_info=e)
                else:
                    option.args.append(
                        BlenderArgument("<device>", WordlistCompletion(valid_devices))
                    )
            else:
                # -setaudio is not documented as taking a <device> at the time of writing,
                # be verbose to help catch potential issues if this changes
                log.warning("was not expecting -setaudio to be documented with an argument")

        # collect valid formats from help text
        # TODO actually get formats with compiled support
        if option.long == "--render-format":
            valid_formats = []
            for n, doc_line in enumerate(option.doc_lines):
                if doc_line.endswith(":"):
                    valid_format_line = option.doc_lines[n+1]
                    valid_formats += map(lambda s: s.strip("'"), valid_format_line.split())
            if len(valid_formats) <= 0:
                log.warning("failed to extract valid render formats from help text")
            else:
                try:
                    option.args[0].completion_spec = WordlistCompletion(valid_formats)
                except IndexError as e:
                    log.warning("expected --render-format to take an argument", exc_info=e)

        # append arg order help to render options as this is probably the thing most prone to confusion when using
        # blender's cli
        # only used if --extended-explanations is enabled
        if option.category in ("Render", "Format"):
            # maintain a blank line between normal help
            if not isblank(option.doc_lines[-1]):
                extra_lines = ['', *blender_arg_order_help_lines]
            else:
                extra_lines = blender_arg_order_help_lines
            option.doc_lines += extra_lines

        if option.long == "--background":
            option.excludes.append("Window")
            option.excludes.append("Animation Playback")

        # animation playback options can be considered a fully separate mode
        if option.short == '-a' and option.category == "Animation Playback":
            # shorter first line help string, otherwise often cut off in completions.
            option.doc_lines.insert(0, "Animation player mode.")

            # extract sub arguments
            start = option.doc_lines.index("Playback Arguments:")
            option.doc_lines[start] = option.doc_lines[start].replace("Arguments", "Options")
            sub_opts = parse_options_from_help("\n".join(option.doc_lines[start:]))

            try:
                optarg = option.args[0]
                assert(optarg.name == '<options>')
            except (IndexError, AssertionError) as e:
                log.warning("expected -a (Animation Playback) to take an argument '<options>'", exc_info=e)
            else:
                optarg.optional = True
                optarg.completion_spec = OptionCompletion(sub_opts)
                # option.excludes_all = True

            try:
                optarg = option.args[1]
                assert(optarg.name == '<file(s)>')
            except (IndexError, AssertionError) as e:
                log.warning("expected -a (Animation Playback) to take an argument '<file(s)>'", exc_info=e)
            else:
                optarg.completion_spec = PathCompletion()
                # option.excludes_all = True

            continue

    with open(args.output, "w", encoding="utf-8") as fh:
        match args.shell:
            case 'zsh':
                make_zsh(blender_info, blender_args, blender_options, fh, args.extended_explanations)
            case 'bash':
                make_bash(blender_info, blender_args, blender_options, fh)

    log.info(f"Wrote {args.shell} completions to {args.output}")

    try:
        check_syntax(args.shell, args.output)
    except CompletionGenerationError as e:
        log.error("generated completion file failed syntax check", exc_info=e)
        sys.exit(1)

if __name__ == "__main__":
    try:
        main()
        sys.exit(0)
    except CompletionGenerationError as e:
        log.exception("completion generation failed")
        sys.exit(1)
