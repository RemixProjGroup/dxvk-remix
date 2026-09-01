#!/usr/bin/env python3

import pathlib
import sys


def patchTaskHeader(path: pathlib.Path) -> None:
    contents = path.read_text()
    oldEnum = """    enum kind_type {
        isolated,
        bound
    };"""
    newEnum = """    enum kind_type {
        isolated,
        bound,
        binding_completed,
        detached,
        dying
    };"""
    oldConstants = """    static const kind_type binding_required = bound;
    static const kind_type binding_completed = kind_type(bound+1);
    static const kind_type detached = kind_type(binding_completed+1);
    static const kind_type dying = kind_type(detached+1);"""
    newConstants = """    static const kind_type binding_required = bound;"""

    if newEnum in contents and oldConstants not in contents:
        return
    if contents.count(oldEnum) != 1 or contents.count(oldConstants) != 1:
        raise RuntimeError(f"unexpected TBB task header layout: {path}")

    path.write_text(contents.replace(oldEnum, newEnum).replace(oldConstants, newConstants))


def patchVtArrayHeader(path: pathlib.Path) -> None:
    contents = path.read_text()
    oldTemplates = """#define VT_ARRAY_EXTERN_TMPL(r, unused, elem) \\
    extern template class VtArray< VT_TYPE(elem) >;
BOOST_PP_SEQ_FOR_EACH(VT_ARRAY_EXTERN_TMPL, ~, VT_SCALAR_VALUE_TYPES)"""
    newTemplates = """#if !defined(__clang__)
#define VT_ARRAY_EXTERN_TMPL(r, unused, elem) \\
    extern template class VtArray< VT_TYPE(elem) >;
BOOST_PP_SEQ_FOR_EACH(VT_ARRAY_EXTERN_TMPL, ~, VT_SCALAR_VALUE_TYPES)
#endif"""

    if newTemplates in contents:
        return
    if contents.count(oldTemplates) != 1:
        raise RuntimeError(f"unexpected USD VtArray header layout: {path}")

    path.write_text(contents.replace(oldTemplates, newTemplates))


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: {sys.argv[0]} <nv-usd-directory>")
    nvUsdDirectory = pathlib.Path(sys.argv[1])
    patchTaskHeader(nvUsdDirectory / "include/tbb/task.h")
    patchVtArrayHeader(nvUsdDirectory / "include/pxr/base/vt/array.h")
