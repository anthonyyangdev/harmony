import dataclasses
import json
import os
import pathlib
from typing import List

from antlr4 import *

import sys
import argparse
from harmony_model_checker.exception import HarmonyCompilerErrorCollection

from harmony_model_checker.harmony import Code, Scope, FrameOp, ReturnOp, optimize, dumpCode, Brief, GenHTML, namestack, PushOp, \
    StoreOp, novalue, imported, files, HarmonyCompilerError, State, ContextValue, constants, modules, run, htmldump
from harmony_model_checker.HarmonyParser import HarmonyParser
from harmony_model_checker.model_checker import check_charm_model_checker_status_is_ok
from harmony_model_checker.parser.HarmonyParserErrorListener import HarmonyParserErrorListener
from harmony_model_checker.parser.HarmonyTokenStream import HarmonyTokenStream
from harmony_model_checker.HarmonyLexer import HarmonyLexer
from harmony_model_checker.antlr_rule_visitor import HarmonyVisitorImpl


def build_parser(progam_input):
    lexer = HarmonyLexer(progam_input)
    parser = HarmonyParser(None)

    lexer.removeErrorListeners()
    parser.removeErrorListeners()

    stream = HarmonyTokenStream(lexer, parser)
    parser._input = stream
    return parser


def load_string(string, scope, code):
    namestack.append("/string-code/")
    ast = parse_string(string)
    for mod in ast.getImports():
        do_import(scope, code, mod)
    # method names and label names get a temporary value
    # they are filled in with their actual values after compilation
    # TODO.  Look for duplicates?
    for ((lexeme, file, line, column), lb) in ast.getLabels():
        scope.names[lexeme] = ("constant", (lb, file, line, column))

    ast.compile(scope, code)
    namestack.pop()


def load_file(filename: str, scope: Scope, code: Code):
    if filename in files:
        return
    namestack.append(filename)
    with open(filename, "r") as f:
        files[filename] = f.read().split("\n")

    ast = parse(filename)
    if ast is None:
        raise HarmonyCompilerError(
            message="Unknown error: unable to parse Harmony file",
            filename=filename
        )

    for mod in ast.getImports():
        do_import(scope, code, mod)

    # method names and label names get a temporary value
    # they are filled in with their actual values after compilation
    # TODO.  Look for duplicates?
    for ((lexeme, file, line, column), lb) in ast.getLabels():
        scope.names[lexeme] = ("constant", (lb, file, line, column))

    ast.compile(scope, code)
    namestack.pop()


def do_import(scope, code, module):
    (lexeme, file, line, column) = module
    # assert lexeme not in scope.names        # TODO
    if lexeme not in imported:
        # TODO.  Only do the following if the modules have variables?
        code.append(PushOp((novalue, file, line, column)))
        code.append(StoreOp(module, module, []))

        # module name replacement with -m flag
        modname = modules[lexeme] if lexeme in modules else lexeme

        # create a new scope
        scope2 = Scope(None)
        scope2.prefix = [lexeme]
        scope2.labels = scope.labels

        found = False
        install_path = os.path.dirname(os.path.realpath(__file__))
        for directory in [os.path.dirname(namestack[-1]), os.path.join(install_path, "modules"), "."]:
            filename = os.path.join(directory, modname + ".hny")
            if os.path.exists(filename):
                load_file(filename, scope2, code)
                found = True
                break
        if not found:
            raise HarmonyCompilerError(
                filename=file,
                lexeme=modname,
                message="Can't import module %s from %s" % (modname, namestack),
                line=line,
                column=column
            )
        imported[lexeme] = scope2

    scope.names[lexeme] = ("module", imported[lexeme])


def parse_constant(name: str, v: str):
    filename = "<constant argument>"
    _input = InputStream(v)
    parser = build_parser(_input)
    visitor = HarmonyVisitorImpl(filename)

    tree = parser.expr()
    ast = visitor.visit(tree)

    scope = Scope(None)
    code = Code()
    ast.compile(scope, code)
    state = State(code, scope.labels)
    ctx = ContextValue(("__arg__", None, None, None), 0, novalue, novalue)
    ctx.atomic = 1
    while ctx.pc != len(code.labeled_ops):
        code.labeled_ops[ctx.pc].op.eval(state, ctx)
    constants[name] = ctx.pop()


def parse_string(string: str):
    _input = InputStream(string)
    parser = build_parser(_input)
    visitor = HarmonyVisitorImpl("<string code>")

    tree = parser.program()
    return visitor.visit(tree)


def parse(filename: str):
    _input = FileStream(filename)
    parser = build_parser(_input)
    error_listener = HarmonyParserErrorListener(filename)
    parser.addErrorListener(error_listener)

    tree = parser.program()
    if error_listener.errors:
        raise HarmonyCompilerErrorCollection(error_listener.errors)

    visitor = HarmonyVisitorImpl(filename)
    return visitor.visit(tree)


def do_compile(filenames: List[str], consts: List[str], mods: List[str], interface: List[str]):
    for c in consts:
        try:
            i = c.index("=")
            parse_constant(c[0:i], c[i + 1:])
        except (IndexError, ValueError):
            raise HarmonyCompilerError(
                message="Usage: -c C=V to define a constant"
            )

    for m in mods:
        try:
            i = m.index("=")
            modules[m[0:i]] = m[i + 1:]
        except (IndexError, ValueError):
            raise HarmonyCompilerError(
                message="Usage: -m module=version to specify a module version"
            )

    scope = Scope(None)
    code = Code()
    code.append(FrameOp(("__init__", None, None, None), []))
    for fname in filenames:
        load_file(fname, scope, code)
    if interface is not None:
        load_string("def __iface__(): result = (%s)" % interface, scope, code)
    
    code.append(ReturnOp())  # to terminate "__init__" process

    # Analyze liveness of variables
    newcode = code.liveness()

    newcode.link()
    optimize(newcode)
    return newcode, scope


args = argparse.ArgumentParser("harmony")
args.add_argument("-a", action="store_true", help="list machine code")
args.add_argument("--parse", "-p", action="store_true", help="parse code without running")
args.add_argument("--const", "-c", action='append', type=str, metavar="name=value", help="define a constant")
args.add_argument("-d", action='store_true', help="htmldump full state into html file")
args.add_argument("--module", "-m", action="append", type=str, metavar="module=version", help="select a module version")
args.add_argument("-i", "--intf", type=str, metavar="expr", help="specify in interface function")
args.add_argument("-s", action="store_true", help="silent (do not print periodic status updates)")
args.add_argument("-v", help="print version number")
args.add_argument("-f", action="store_true", help="run with internal model checker (not supported)")

# Internal flags
args.add_argument("-b", action="store_true", help=argparse.SUPPRESS)
args.add_argument("--cf", action="append", type=str, help=argparse.SUPPRESS)
args.add_argument("-t", action="store_true", help=argparse.SUPPRESS)
args.add_argument("-A", action="store_true", help=argparse.SUPPRESS)
args.add_argument("-j", action="store_true", help=argparse.SUPPRESS)
args.add_argument("-o", action="store_true", help=argparse.SUPPRESS)
args.add_argument("--suppress", action="store_true", help=argparse.SUPPRESS)
args.add_argument("--grammar", action="store_true", help=argparse.SUPPRESS)

args.add_argument("files", metavar="harmony-file", type=pathlib.Path, nargs='*', help="files to compile")


def main():
    ns = args.parse_args()

    if ns.grammar:
        for f in ns.files:
            print("Parsing", f)
            parse(str(f))
            print()
        return

    if not check_charm_model_checker_status_is_ok():
        return 1

    consts = ns.const or []
    interface = ns.intf
    mods = ns.module or []
    parse_code_only = ns.parse
    print_code = None
    if ns.a:
        print_code = "verbose"
    if ns.A:
        print_code = "terse"
    if ns.j:
        print_code = "json"

    block_flag = ns.b
    charm_flag = True and not any({ns.a, ns.A, ns.j, ns.f})
    fulldump = ns.d
    silent = ns.s
    suppress_output = ns.suppress
    charm_options = ns.cf or []
    output_flag = ns.o

    outputfiles = {
        "hfa": None,
        "htm": None,
        "hco": None,
        "hvm": None,
        "png": None,
        "gv":  None
    }
    behavior = None

    filenames = [str(f) for f in ns.files]
    if not filenames:
        print("harmony: fatal error: no input files")
        return 1
    for f in filenames:
        if not os.path.exists(f):
            print("harmony: error: file named '" + f + "' does not exist.")
            return 1
    stem = os.path.splitext(os.path.basename(filenames[0]))[0]

    if outputfiles["hvm"] is None:
        outputfiles["hvm"] = stem + ".hvm"
    if outputfiles["hco"] is None:
        outputfiles["hco"] = stem + ".hco"
    if outputfiles["htm"] is None:
        outputfiles["htm"] = stem + ".htm"
    if outputfiles["png"] is not None and outputfiles["gv"] == None:
        outputfiles["gv"] = stem + ".gv"

    try:
        code, scope = do_compile(filenames, consts, mods, interface)
    except (HarmonyCompilerErrorCollection, HarmonyCompilerError) as e:
        if isinstance(e, HarmonyCompilerErrorCollection):
                errors = e.errors
        else:
            errors = [e.token]
        if parse_code_only:
            with open(outputfiles["hvm"], "w") as fp:
                data = dict(errors=[dataclasses.asdict(e) for e in errors], status="error")
                json.dump(data, fp)
        else:
            for e in errors:
                print(f"Line {e.line}:{e.column} at {e.filename}, {e.message}")
                print()
        return 1

    # Analyze liveness of variables
    install_path = os.path.dirname(os.path.realpath(__file__))
    # see if there is a configuration file
    if charm_flag:
        # see if there is a configuration file
        outfile = os.path.join(install_path, "charm.exe")
        with open(outputfiles["hvm"], "w") as fd:
            dumpCode("json", code, scope, f=fd)
        r = os.system("%s %s -o%s %s" % (outfile, " ".join(charm_options), outputfiles["hco"], outputfiles["hvm"]))
        if r != 0:
            print("charm model checker failed")
            return r
        b = Brief()
        b.run(outputfiles, behavior)
        gh = GenHTML()
        gh.run(outputfiles)
        if not suppress_output:
            p = pathlib.Path(outputfiles["hvm"]).resolve()
            print("open file://" + str(p) + " for more information", file=sys.stderr)
        return 0
    if print_code is None:
        nodes, bad_node = run(code, scope.labels, block_flag)
        if bad_node is not None:
            if not silent:
                htmldump(nodes, code, scope, bad_node, fulldump, False)
            return 1
    else:
        dumpCode(print_code, code, scope)
