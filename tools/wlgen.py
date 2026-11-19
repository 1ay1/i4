#!/usr/bin/env python3
"""wlgen — compile a Wayland protocol XML into a C++ header of *types*.

The output contains no logic: every interface becomes a struct of constexpr
data and empty tag types. All behaviour lives in the generic headers, so this
generator is ~300 lines and will never need to grow.

usage: wlgen.py <protocol.xml> [more.xml ...] -o out.hpp [--namespace ns]
"""
from __future__ import annotations

import argparse
import re
import sys
import xml.etree.ElementTree as ET

# Real C++ keywords only. (Notably NOT Python's: `global`, `import`, `from`,
# `class`... — `wl_registry.global` is a legitimate Wayland event name and must
# survive as `global_`, while `pass` or `lambda` must not be renamed at all.)
CXX_KEYWORDS = {
    "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor",
    "bool", "break", "case", "catch", "char", "char8_t", "char16_t",
    "char32_t", "class", "compl", "concept", "const", "consteval",
    "constexpr", "constinit", "const_cast", "continue", "co_await",
    "co_return", "co_yield", "decltype", "default", "delete", "do", "double",
    "dynamic_cast", "else", "enum", "explicit", "export", "extern", "false",
    "float", "for", "friend", "goto", "if", "inline", "int", "long",
    "mutable", "namespace", "new", "noexcept", "not", "not_eq", "nullptr",
    "operator", "or", "or_eq", "private", "protected", "public", "register",
    "reinterpret_cast", "requires", "return", "short", "signed", "sizeof",
    "static", "static_assert", "static_cast", "struct", "switch", "template",
    "this", "thread_local", "throw", "true", "try", "typedef", "typeid",
    "typename", "union", "unsigned", "using", "virtual", "void", "volatile",
    "wchar_t", "while", "xor", "xor_eq",
}

ATOM = {
    "int": "i4::wl::Int",
    "uint": "i4::wl::Uint",
    "fixed": "i4::wl::Fixed",
    "string": "i4::wl::String",
    "array": "i4::wl::Array",
    "fd": "i4::wl::Fd",
}


def ident(name: str) -> str:
    """A C++ identifier for a protocol name."""
    n = re.sub(r"[^0-9a-zA-Z_]", "_", name)
    if n and n[0].isdigit():
        n = "_" + n
    if n in CXX_KEYWORDS:
        n += "_"
    return n


def doc(el: ET.Element, indent: str) -> list[str]:
    """Carry the protocol's own documentation into the header."""
    d = el.find("description")
    if d is None:
        return []
    summary = (d.get("summary") or "").strip()
    body = (d.text or "").strip()
    out: list[str] = []
    if summary:
        out.append(f"{indent}/// {summary}")
    for line in body.splitlines():
        line = line.strip()
        if line:
            out.append(f"{indent}/// {line}")
        elif out:
            out.append(f"{indent}///")
    while out and out[-1].endswith("///"):
        out.pop()
    return out


class Protocol:
    def __init__(self, path: str):
        self.root = ET.parse(path).getroot()
        self.name = self.root.get("name")
        self.path = path


def enum_type(iface: str, ref: str) -> str:
    """Resolve an enum reference, which may be 'name' or 'other_iface.name'.

    Enums are hoisted into namespace `enums` ahead of every interface, because
    protocols reference each other's enums freely and in any order.
    """
    if "." in ref:
        owner, enum = ref.split(".", 1)
        return f"enums::{ident(owner)}_{ident(enum)}"
    return f"enums::{ident(iface)}_{ident(ref)}"


def atom_for(arg: ET.Element, iface: str) -> str:
    t = arg.get("type")
    enum = arg.get("enum")
    nullable = arg.get("allow-null") == "true"

    if enum and t in ("uint", "int"):
        base = "i4::wl::Uint" if t == "uint" else "i4::wl::Int"
        a = f"i4::wl::Enum<{enum_type(iface, enum)}, {base}>"
    elif t == "object":
        i = arg.get("interface")
        a = f"i4::wl::Object<{ident(i)}>" if i else "i4::wl::Object<void>"
    elif t == "new_id":
        i = arg.get("interface")
        a = f"i4::wl::NewId<{ident(i)}>" if i else "i4::wl::DynNewId"
    else:
        a = ATOM[t]

    if nullable and t in ("object", "string", "array"):
        a = f"i4::wl::Nullable<{a}>"
    return a


def emit_hoisted_enums(iface: ET.Element, out: list[str]) -> None:
    """Emit an interface's enums at namespace scope, flat-named.

    Also emits, for each enum, a constexpr validator derived from the entries.
    The XML lists every legal value, so "is this a valid enumerator" is a fact
    the generator knows and the library should not have to guess. That closes
    the invalid-enum hole for decoding without a hand-maintained table.
    """
    iname = ident(iface.get("name"))
    for en in iface.findall("enum"):
        name = f"{iname}_{ident(en.get('name'))}"
        bitfield = en.get("bitfield") == "true"
        out += doc(en, "")
        out.append(f"enum class {name} : std::uint32_t {{")
        seen: dict[str, str] = {}
        for e in en.findall("entry"):
            ename = ident(e.get("name"))
            if ename in seen:
                continue
            seen[ename] = e.get("value")
            summary = (e.get("summary") or "").strip()
            comment = f"  ///< {summary}" if summary else ""
            out.append(f"    {ename} = {e.get('value')},{comment}")
        out.append("};")

        values = list(seen.values())
        if bitfield:
            # A bitfield's legal domain is the OR of its flags. Zero is always
            # legal: it means "no flags set".
            mask = " | ".join(f"std::uint32_t({v})" for v in values) or "0u"
            out.append(f"constexpr std::uint32_t {name}_mask = {mask};")
            out.append(f"constexpr bool valid({name} v) {{")
            out.append(f"    return (static_cast<std::uint32_t>(v) & ~{name}_mask) == 0;")
            out.append("}")
            out.append(f"constexpr {name} operator|({name} a, {name} b) {{")
            out.append(f"    return static_cast<{name}>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));")
            out.append("}")
            out.append(f"constexpr {name} operator&({name} a, {name} b) {{")
            out.append(f"    return static_cast<{name}>(static_cast<std::uint32_t>(a) & static_cast<std::uint32_t>(b));")
            out.append("}")
            out.append(f"constexpr {name}& operator|=({name}& a, {name} b) {{ a = a | b; return a; }}")
            out.append(f"constexpr bool any({name} a) {{ return static_cast<std::uint32_t>(a) != 0; }}")
        else:
            # A plain enum's domain is exactly its listed entries.
            if values:
                tests = " || ".join(f"u == std::uint32_t({v})" for v in values)
            else:
                tests = "false"
            out.append(f"constexpr bool valid({name} v) {{")
            out.append("    const std::uint32_t u = static_cast<std::uint32_t>(v);")
            out.append(f"    return {tests};")
            out.append("}")
        out.append("")


def emit_enums(iface: ET.Element, out: list[str]) -> None:
    """Inside the interface, alias the hoisted enums back to their short names,
    so user code writes the natural `wl_shm::format::argb8888`."""
    iname = ident(iface.get("name"))
    for en in iface.findall("enum"):
        short = ident(en.get("name"))
        out += doc(en, "    ")
        out.append(f"    using {short} = enums::{iname}_{short};")
    if iface.findall("enum"):
        out.append("")


def emit_messages(iface: ET.Element, kind: str, out: list[str]) -> None:
    iname = ident(iface.get("name"))
    plural = "requests" if kind == "request" else "events"
    msgs = iface.findall(kind)

    out.append(f"    struct {plural} {{")
    names: list[str] = []
    for op, m in enumerate(msgs):
        mname = ident(m.get("name"))
        names.append(mname)
        since = m.get("since", "1")
        dtor = m.get("type") == "destructor"
        args = [atom_for(a, iface.get("name")) for a in m.findall("arg")]
        sig = f"i4::wl::Sig<{', '.join(args)}>" if args else "i4::wl::Sig<>"

        out += doc(m, "        ")
        for a in m.findall("arg"):
            s = (a.get("summary") or "").strip()
            if s:
                out.append(f"        /// @param {ident(a.get('name'))} {s}")
        out.append(f"        struct {mname} {{")
        out.append(f"            using iface = {iname};")
        out.append(f"            using sig = {sig};")
        out.append(f"            static constexpr std::uint32_t opcode = {op};")
        out.append(f"            static constexpr std::uint32_t since = {since};")
        out.append(f"            static constexpr bool destructor = {'true' if dtor else 'false'};")
        out.append(f'            static constexpr std::string_view wl_name = "{m.get("name")}";')
        arg_names = ", ".join(f'"{a.get("name")}"' for a in m.findall("arg"))
        out.append(f"            static constexpr std::array<std::string_view, {len(args)}> arg_names{{{{{arg_names}}}}};")
        out.append("        };")
    out.append(f"        using all = i4::wl::List<{', '.join(names)}>;")
    out.append("    };")
    out.append("")


def emit_interface(iface: ET.Element, out: list[str]) -> None:
    name = iface.get("name")
    iname = ident(name)
    version = iface.get("version", "1")

    out += doc(iface, "")
    out.append(f"struct {iname} {{")
    out.append(f'    static constexpr std::string_view wl_name = "{name}";')
    out.append(f"    static constexpr std::uint32_t wl_version = {version};")
    out.append("")
    emit_enums(iface, out)
    emit_messages(iface, "request", out)
    emit_messages(iface, "event", out)
    out.append("};")
    out.append("")


def signature_string(msg: ET.Element) -> str:
    """libwayland's wl_message.signature: the wire types as a compact string.

    A leading digit is the `since` version; '?' marks a nullable argument.
    This is the format wl_proxy_marshal parses at RUNTIME, so a C client
    linking against us needs byte-identical strings.
    """
    codes = {
        "int": "i", "uint": "u", "fixed": "f", "string": "s",
        "object": "o", "new_id": "n", "array": "a", "fd": "h",
    }
    out = ""
    since = int(msg.get("since", "1"))
    if since > 1:
        out += str(since)
    for a in msg.findall("arg"):
        t = a.get("type")
        if a.get("allow-null") == "true":
            out += "?"
        if t == "new_id" and not a.get("interface"):
            # An untyped new_id expands to (string, uint, new_id) on the wire.
            out += "sun"
        else:
            out += codes[t]
    return out


def message_types(msg: ET.Element) -> list[str]:
    """The wl_message.types array: one entry per argument, NULL unless the
    argument names an interface."""
    types: list[str] = []
    for a in msg.findall("arg"):
        t = a.get("type")
        iface = a.get("interface")
        if t == "new_id" and not iface:
            types += ["NULL", "NULL", "NULL"]  # string, uint, new_id
        elif t in ("object", "new_id") and iface:
            types.append(f"&{ident(iface)}_interface")
        else:
            types.append("NULL")
    return types


def emit_c_abi(ifaces: list[ET.Element], namespace: str) -> str:
    """Generate the runtime wl_interface tables libwayland's C API needs.

    The C ABI is *dynamically* typed: wl_proxy_marshal takes a varargs list and
    consults a signature string at runtime. That is the opposite of everything
    else here, but it is what existing C clients link against — and both come
    from the same XML, so they cannot disagree.
    """
    out: list[str] = []
    out.append("// GENERATED by tools/wlgen.py --c-abi — do not edit.")
    out.append("//")
    out.append("// Runtime interface tables for the C ABI. Same protocol data as the")
    out.append("// C++ headers, in the shape libwayland's wl_proxy_marshal expects.")
    out.append('#include "i4/wl/c/wayland-util.h"')
    out.append("")
    out.append("#ifdef __GNUC__")
    out.append('#define WL_PRIVATE __attribute__((visibility("default")))')
    out.append("#else")
    out.append("#define WL_PRIVATE")
    out.append("#endif")
    out.append("")

    for i in ifaces:
        out.append(f"extern const struct wl_interface {ident(i.get('name'))}_interface;")
    out.append("")

    # Forward-declare interfaces referenced but defined in another file.
    declared = {i.get("name") for i in ifaces}
    referenced: set[str] = set()
    for i in ifaces:
        for kind in ("request", "event"):
            for m in i.findall(kind):
                for a in m.findall("arg"):
                    if a.get("interface"):
                        referenced.add(a.get("interface"))
    for name in sorted(referenced - declared):
        out.append(f"extern const struct wl_interface {ident(name)}_interface;")
    out.append("")

    for i in ifaces:
        iname = ident(i.get("name"))
        for kind, plural in (("request", "requests"), ("event", "events")):
            msgs = i.findall(kind)
            if not msgs:
                continue
            # The types arrays, flattened per message.
            for op, m in enumerate(msgs):
                types = message_types(m)
                if types:
                    joined = ", ".join(types)
                    out.append(
                        f"static const struct wl_interface *{iname}_{plural}_{op}_types[] = {{{joined}}};")
            out.append(f"static const struct wl_message {iname}_{plural}[] = {{")
            for op, m in enumerate(msgs):
                types = message_types(m)
                tref = f"{iname}_{plural}_{op}_types" if types else "NULL"
                out.append(f'    {{ "{m.get("name")}", "{signature_string(m)}", {tref} }},')
            out.append("};")
            out.append("")

    for i in ifaces:
        iname = ident(i.get("name"))
        nreq = len(i.findall("request"))
        nev = len(i.findall("event"))
        req = f"{iname}_requests" if nreq else "NULL"
        ev = f"{iname}_events" if nev else "NULL"
        out.append(f"WL_PRIVATE const struct wl_interface {iname}_interface = {{")
        out.append(f'    "{i.get("name")}", {i.get("version", "1")},')
        out.append(f"    {nreq}, {req},")
        out.append(f"    {nev}, {ev},")
        out.append("};")
        out.append("")

    return "\n".join(out)


def generate(protocols: list[Protocol], namespace: str, includes: list[str],
             list_name: str) -> str:
    out: list[str] = []
    srcs = ", ".join(p.path.split("/")[-1] for p in protocols)
    out.append("// GENERATED by tools/wlgen.py — do not edit.")
    out.append(f"// source: {srcs}")
    out.append("//")
    out.append("// Pure data: every interface below is constexpr values and empty tag types.")
    out.append("// All behaviour lives in <wl/algebra.hpp>, <wl/wire.hpp>, <wl/proxy.hpp>.")
    out.append("#pragma once")
    out.append("")
    out.append('#include <i4/wl/algebra.hpp>')
    for inc in includes:
        out.append(f'#include <{inc}>')
    out.append("")
    out.append(f"namespace {namespace} {{")
    out.append("")

    ifaces: list[ET.Element] = []
    seen: set[str] = set()
    for p in protocols:
        for i in p.root.findall("interface"):
            n = i.get("name")
            if n in seen:
                continue  # e.g. wl_display re-declared by an extension
            seen.add(n)
            ifaces.append(i)

    # Forward declarations. A protocol may reference interfaces it does not
    # declare (cursor-shape names zwp_tablet_tool_v2, xdg-shell names
    # wl_surface), and the reference may be to a protocol the user never
    # includes. Declaring them is enough: the algebra only needs the *name* of
    # an interface to type an Object<I> argument, never its definition.
    declared = {i.get("name") for i in ifaces}
    referenced: set[str] = set()
    for i in ifaces:
        for kind in ("request", "event"):
            for m in i.findall(kind):
                for arg in m.findall("arg"):
                    if arg.get("type") in ("object", "new_id") and arg.get("interface"):
                        referenced.add(arg.get("interface"))
    external = sorted(referenced - declared)

    out.append("// forward declarations")
    for i in ifaces:
        out.append(f"struct {ident(i.get('name'))};")
    if external:
        out.append("")
        out.append("// interfaces referenced by this protocol but defined elsewhere")
        for name in external:
            out.append(f"struct {ident(name)};")
    out.append("")

    # Enums are hoisted ahead of every interface: a message signature may name
    # an enum belonging to an interface declared later, or in another file.
    out.append("namespace enums {")
    out.append("")
    for i in ifaces:
        emit_hoisted_enums(i, out)
    out.append("}  // namespace enums")
    out.append("using namespace enums;")
    out.append("")

    for i in ifaces:
        emit_interface(i, out)

    # The algebraic laws, checked at compile time on every generated interface.
    out.append("// Every generated interface must satisfy the algebra's laws.")
    for i in ifaces:
        n = ident(i.get("name"))
        out.append(f"static_assert(i4::wl::Interface<{n}>);")
        out.append(f'static_assert(i4::wl::laws::well_formed<{n}>, "{i.get("name")}: malformed protocol record");')
    out.append("")
    out.append(f"using {list_name} = i4::wl::List<{', '.join(ident(i.get('name')) for i in ifaces)}>;")
    out.append("")
    out.append(f"}}  // namespace {namespace}")
    out.append("")
    return "\n".join(out)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("xml", nargs="+")
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--namespace", default="i4::wl::proto")
    ap.add_argument("--include", action="append", default=[],
                    help="header this protocol depends on, e.g. i4/wl/proto/wayland.hpp")
    ap.add_argument("--list-name", default=None,
                    help="name of the generated i4::wl::List of interfaces")
    ap.add_argument("--c-abi", action="store_true",
                    help="emit runtime wl_interface tables (C) instead of types (C++)")
    a = ap.parse_args()

    protocols = [Protocol(x) for x in a.xml]

    if a.c_abi:
        ifaces: list[ET.Element] = []
        seen: set[str] = set()
        for p in protocols:
            for i in p.root.findall("interface"):
                if i.get("name") in seen:
                    continue
                seen.add(i.get("name"))
                ifaces.append(i)
        text = emit_c_abi(ifaces, a.namespace)
        with open(a.output, "w") as f:
            f.write(text)
        print(f"wlgen: {len(ifaces)} C interfaces -> {a.output}", file=sys.stderr)
        return 0

    list_name = a.list_name or (
        re.sub(r"[^0-9a-zA-Z_]", "_", protocols[0].name or "protocol") + "_interfaces")
    text = generate(protocols, a.namespace, a.include, list_name)
    with open(a.output, "w") as f:
        f.write(text)

    n = sum(len(p.root.findall("interface")) for p in protocols)
    print(f"wlgen: {n} interfaces -> {a.output} ({len(text.splitlines())} lines)", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
