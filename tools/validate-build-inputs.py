#!/usr/bin/env python3
"""Check kext build inputs without compiling, installing, or touching hardware."""

import pathlib
import plistlib
import re
import sys


def main():
    root = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parent.parent
    errors = []
    sources = sorted((root / "MPR").glob("*.cpp"))
    if not sources:
        errors.append("MPR contains no C++ translation units")

    inputs = set((root / "MPR").rglob("*.h"))
    inputs.update(sources)
    inputs.update((root / "tools").glob("*.sh"))
    inputs.update((root / "tools").glob("*.py"))
    inputs.add(root / "MPR/Info.plist")
    inputs.add(root / "MPR.xcodeproj/project.pbxproj")
    for path in sorted(inputs):
        if b"\r" in path.read_bytes():
            errors.append("CR line endings: " + str(path.relative_to(root)))

    with (root / "MPR/Info.plist").open("rb") as stream:
        info = plistlib.load(stream)
    version = info.get("CFBundleVersion")
    if not version or info.get("CFBundleShortVersionString") != version:
        errors.append("Info.plist bundle versions are missing or differ")
    if info.get("CFBundleExecutable") != "MPR" or info.get("CFBundleIdentifier") != "com.ling.MPR":
        errors.append("Info.plist executable or identifier differs from the build entry points")

    build = (root / "tools/build-noxcode.sh").read_text(encoding="utf-8")
    for label, pattern in (
        ("CLI version", r"(?m)^VERSION=([^\s]+)$"),
        ("kmod version", r'KMOD_EXPLICIT_DECL\(com\.ling\.MPR,\s*"([^"]+)"'),
    ):
        values = re.findall(pattern, build)
        if values != [version]:
            errors.append(label + " does not match Info.plist")

    controller = (root / "MPR/MPRController.cpp").read_text(encoding="utf-8")
    registry_versions = re.findall(
        r'(?:setProperty|MPR_DEBUG_PROP)\("MPRBuild",\s*"([^"]+)"\)',
        controller,
    )
    if registry_versions != [version]:
        errors.append("IORegistry MPRBuild does not match Info.plist")

    project = (root / "MPR.xcodeproj/project.pbxproj").read_text(encoding="utf-8")
    for setting in ("CURRENT_PROJECT_VERSION", "MARKETING_VERSION", "MODULE_VERSION"):
        values = re.findall(r"\b" + setting + r"\s*=\s*([^;\s]+);", project)
        if len(values) != 2 or any(value != version for value in values):
            errors.append("Xcode " + setting + " does not match Info.plist in both configurations")

    file_refs = dict(re.findall(
        r"([A-Fa-f0-9]{24})\s*/\*[^*]*\*/\s*=\s*\{isa = PBXFileReference;[^}]*?path = ([^;]+);",
        project,
    ))
    build_files = dict(re.findall(
        r"([A-Fa-f0-9]{24})\s*/\*[^*]*\*/\s*=\s*\{isa = PBXBuildFile;\s*fileRef = ([A-Fa-f0-9]{24})",
        project,
    ))
    compiled = []
    for block in re.findall(r"isa = PBXSourcesBuildPhase;([^}]+)", project):
        for build_id in re.findall(r"([A-Fa-f0-9]{24})\s*/\*", block):
            file_id = build_files.get(build_id)
            name = file_refs.get(file_id, "").strip('"')
            if not name:
                errors.append("Xcode source reference cannot be resolved: " + build_id)
            else:
                compiled.append(name)
    for source in sources:
        count = compiled.count(source.name)
        if count != 1:
            errors.append("Xcode Sources must compile {} exactly once (found {})".format(source.name, count))
    for source in compiled:
        if not (root / "MPR" / source).is_file():
            errors.append("Xcode compiles a missing source: " + source)

    if errors:
        for error in errors:
            print("error: " + error, file=sys.stderr)
        return 1
    print("Build inputs OK: {} translation units, version {}, LF text.".format(len(sources), version))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, plistlib.InvalidFileException) as error:
        print("error: " + str(error), file=sys.stderr)
        sys.exit(1)
