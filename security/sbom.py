#!/usr/bin/env python3
"""The software bill of materials: what goes into a SimplePLC binary, read from the sources.

    python3 security/sbom.py generate [--toolchain | --image PLATFORMS] [--version V] [-o FILE]
    python3 security/sbom.py check
    python3 security/sbom.py outdated

generate  writes a CycloneDX 1.6 SBOM. Versions come from the files that are built in (the
          vendored headers, the open62541 tag CMake fetches, the CodeMirror script), never from a
          list kept by hand. --toolchain adds the C and C++ runtime that a static build links
          (musl, libstdc++, libgcc), asked of the compiler and the Alpine package database, so run
          it in the build container. --image PLATFORMS does the same for the container image.
check     fails when the sources disagree with each other: the open62541 tag or options in CMake,
          the Dockerfile and CI, a release workflow that names its own Alpine release instead of
          the Dockerfile's, the versions in THIRD_PARTY_LICENSES.md, a third-party directory no
          component describes, or a VEX statement about a product that is not in the SBOM.
outdated  compares each component with its newest upstream release and the Alpine release with
          its end of support; prints a Markdown table and exits 2 if anything is behind.

Scanners find nothing in a stripped static binary on an empty image, so this SBOM is what
vulnerability scanning works from: each component carries a purl and, where NVD lists the
product, a CPE.
"""
import argparse
import datetime
import json
import os
import pathlib
import re
import subprocess
import sys
import urllib.parse
import urllib.request
import uuid

ROOT = pathlib.Path(__file__).resolve().parent.parent


def read(rel):
    return (ROOT / rel).read_text(encoding="utf-8")


def find(pattern, rel, group=1):
    """The first match of pattern in a file of the repository; a missing one is an error, so a
    moved definition cannot leave a stale version in the SBOM."""
    m = re.search(pattern, read(rel), re.MULTILINE)
    if not m:
        sys.exit(f"sbom: {rel} does not contain {pattern!r}")
    return m.group(group)


def open62541_version():
    return find(r'set\(SIMPLEPLC_OPEN62541_TAG "v([0-9.]+)"', "CMakeLists.txt")


def httplib_version():
    return find(r'#define CPPHTTPLIB_VERSION "([0-9.]+)"', "third_party/httplib/httplib.h")


def json_version():
    rel = "third_party/json/nlohmann/json.hpp"
    return ".".join(find(rf"#define NLOHMANN_JSON_VERSION_{part} ([0-9]+)", rel)
                    for part in ("MAJOR", "MINOR", "PATCH"))


def musl_math_version():
    return find(r"Source: musl ([0-9.]+),", "third_party/musl-math/README.md")


def codemirror_version():
    return find(r'CodeMirror\.version = "([0-9.]+)"', "src/web/assets/vendor/codemirror/codemirror.js")


def project_version():
    return find(r"project\(SimplePLC\s+VERSION ([0-9.]+)", "CMakeLists.txt")


def open62541_options(rel, pattern):
    """The open62541 options that decide which of its code is compiled in (the VEX statements
    rely on them), as CMake or the Dockerfile set them."""
    return dict(re.findall(pattern, read(rel)))


def cmake_open62541_options():
    return open62541_options("CMakeLists.txt", r'set\((UA_[A-Z_]+) "?([A-Z0-9]+)"? CACHE')


def docker_open62541_options():
    return open62541_options("Dockerfile", r"-D(UA_[A-Z_]+)=([A-Z0-9]+)")


# The components built into every binary. "where" is the directory in this repository (the check
# requires one component for each directory under third_party/ and the panel's vendor/);
# "upstream" is how `outdated` finds the newest release.
COMPONENTS = [
    {
        "name": "open62541", "version": open62541_version, "license": "MPL-2.0",
        "supplier": "open62541 project", "url": "https://www.open62541.org",
        "vcs": "https://github.com/open62541/open62541",
        "purl": "pkg:github/open62541/open62541@v{v}",
        "cpe": "cpe:2.3:a:open62541:open62541:{v}:*:*:*:*:*:*:*",
        "where": None, "upstream": ("github", "open62541/open62541"),
        "description": "OPC UA server, built from its upstream source at this tag, unmodified",
    },
    {
        "name": "cpp-httplib", "version": httplib_version, "license": "MIT",
        "supplier": "yhirose", "url": "https://github.com/yhirose/cpp-httplib",
        "vcs": "https://github.com/yhirose/cpp-httplib",
        "purl": "pkg:github/yhirose/cpp-httplib@v{v}",
        "cpe": "cpe:2.3:a:yhirose:cpp-httplib:{v}:*:*:*:*:*:*:*",
        "where": "third_party/httplib", "upstream": ("github", "yhirose/cpp-httplib"),
        "description": "HTTP server of the panel, REST API and MCP endpoint (header-only, vendored)",
    },
    {
        "name": "nlohmann/json", "version": json_version, "license": "MIT",
        "supplier": "Niels Lohmann", "url": "https://json.nlohmann.me",
        "vcs": "https://github.com/nlohmann/json",
        "purl": "pkg:github/nlohmann/json@v{v}",
        "cpe": "cpe:2.3:a:nlohmann:json:{v}:*:*:*:*:*:*:*",
        "where": "third_party/json", "upstream": ("github", "nlohmann/json"),
        "description": "JSON parser and writer (header-only, vendored)",
    },
    {
        "name": "musl libm", "version": musl_math_version, "license": "MIT",
        "supplier": "musl libc project", "url": "https://musl.libc.org",
        "vcs": "https://git.musl-libc.org/cgit/musl",
        "purl": "pkg:generic/musl-libm@{v}?download_url=https://musl.libc.org/releases/musl-{v}.tar.gz",
        "cpe": "cpe:2.3:a:musl-libc:musl:{v}:*:*:*:*:*:*:*",
        "where": "third_party/musl-math", "upstream": ("musl", None),
        "description": "the double-precision elementary functions the VM uses, so every platform "
                       "computes the same bits (vendored, symbols renamed spl_*)",
    },
    {
        "name": "CodeMirror", "version": codemirror_version, "license": "MIT",
        "supplier": "Marijn Haverbeke", "url": "https://codemirror.net/5/",
        "vcs": "https://github.com/codemirror/codemirror5",
        "purl": "pkg:npm/codemirror@{v}",
        "cpe": "cpe:2.3:a:codemirror:codemirror:{v}:*:*:*:*:*:*:*",
        "where": "src/web/assets/vendor/codemirror", "upstream": ("npm5", "codemirror"),
        "description": "the panel's program editor (embedded in the binary, runs in the browser)",
    },
]

# The runtime a static Linux build links, found by the file the compiler links and the Alpine
# package that owns it. Alpine's security database tracks them by their source package; musl also
# gets NVD's CPE (with the upstream version), so a CVE is found even where Alpine's data is late.
# GCC's CPE is left out: nearly all of its CVEs are about the compiler, not the libraries.
TOOLCHAIN = [
    ("musl", "libc.a", "MIT", "musl libc project", "https://musl.libc.org",
     "C library, linked statically", "cpe:2.3:a:musl-libc:musl:{v}:*:*:*:*:*:*:*"),
    ("libstdc++", "libstdc++.a", "GPL-3.0-or-later WITH GCC-exception-3.1", "GNU project",
     "https://gcc.gnu.org", "C++ standard library, linked statically", None),
    ("libgcc", "libgcc.a", "GPL-3.0-or-later WITH GCC-exception-3.1", "GNU project",
     "https://gcc.gnu.org", "compiler runtime, linked statically", None),
]


def run(*cmd):
    return subprocess.run(cmd, check=True, capture_output=True, text=True).stdout.strip()


def toolchain_components(any_arch):
    """musl, libstdc++ and libgcc as the Alpine packages that hold the archives this compiler
    links, with their exact package versions. any_arch leaves the architecture out of the purls,
    for a multi-platform image (one Alpine release has the same versions on every platform)."""
    try:
        distro = pathlib.Path("/etc/alpine-release").read_text().strip()
    except FileNotFoundError:
        sys.exit("sbom: --toolchain reads the Alpine package database: run it in the build container")
    arch = run("apk", "--print-arch")
    result = []
    for name, archive, license_id, supplier, url, description, cpe in TOOLCHAIN:
        path = os.path.realpath(run("cc", "-print-libgcc-file-name" if archive == "libgcc.a" else f"-print-file-name={archive}"))
        owner = run("apk", "info", "-W", path)  # "/usr/lib/libc.a is owned by musl-dev-1.2.5-r4"
        m = re.search(r"is owned by (\S+)-([0-9][^-]*-r[0-9]+)$", owner)
        if not m:
            sys.exit(f"sbom: no Alpine package owns {path}: {owner}")
        package, version = m.groups()
        origin = origin_of(package)
        result.append({
            "type": "library", "bom-ref": f"toolchain:{name}", "name": name,
            "version": version, "description": f"{description} (Alpine {distro} package {package})",
            "supplier": {"name": supplier, "url": [url]},
            "licenses": [{"expression": license_id}] if " " in license_id else [{"license": {"id": license_id}}],
            "purl": f"pkg:apk/alpine/{urllib.parse.quote(package, safe='')}@{version}?"
                    + ("" if any_arch else f"arch={arch}&") + f"upstream={origin}&distro=alpine-{distro}",
            "externalReferences": [{"type": "website", "url": url}],
        })
        if cpe:
            result[-1]["cpe"] = cpe.format(v=version.split("-r")[0])  # 1.2.6-r3: Alpine's revision 3 of 1.2.6
    return result, distro, arch


def origin_of(package):
    """The source package Alpine builds a package from (musl-dev comes from musl): the name its
    security database uses. Read from the package database ("P:" name, "o:" origin)."""
    for block in pathlib.Path("/lib/apk/db/installed").read_text().split("\n\n"):
        fields = dict((line[0], line[2:]) for line in block.splitlines() if line[1:2] == ":")
        if fields.get("P") == package:
            return fields.get("o", package)
    return package


def component(c):
    v = c["version"]()
    item = {
        "type": "library", "bom-ref": c["name"], "name": c["name"], "version": v,
        "description": c["description"],
        "supplier": {"name": c["supplier"], "url": [c["url"]]},
        "licenses": [{"license": {"id": c["license"]}}],
        "purl": c["purl"].format(v=v),
        "cpe": c["cpe"].format(v=v),
        "externalReferences": [{"type": "website", "url": c["url"]}, {"type": "vcs", "url": c["vcs"]}],
    }
    if c["where"]:
        item["properties"] = [{"name": "simpleplc:location", "value": c["where"]}]
    if c["name"] == "open62541":
        item["properties"] = [{"name": f"simpleplc:build:{k}", "value": val}
                              for k, val in sorted(cmake_open62541_options().items())]
    return item


def generate(args):
    components = [component(c) for c in COMPONENTS]
    target = "source"
    if args.toolchain or args.image:
        runtime, distro, arch = toolchain_components(bool(args.image))
        components += runtime
        target = (f"container image ({args.image}), static, Alpine {distro}" if args.image
                  else f"linux-{arch}, static, Alpine {distro}")
    version = (args.version or project_version()).removeprefix("v")
    stamp = os.environ.get("SOURCE_DATE_EPOCH")
    when = (datetime.datetime.fromtimestamp(int(stamp), datetime.timezone.utc) if stamp
            else datetime.datetime.now(datetime.timezone.utc)).replace(microsecond=0)
    bom = {
        "bomFormat": "CycloneDX", "specVersion": "1.6",
        "serialNumber": f"urn:uuid:{uuid.uuid4()}", "version": 1,
        "metadata": {
            "timestamp": when.isoformat().replace("+00:00", "Z"),
            "tools": {"components": [{"type": "application", "name": "security/sbom.py",
                                      "description": "SimplePLC's SBOM generator"}]},
            "authors": [{"name": "SimplePLC"}],
            "component": {
                "type": "application", "bom-ref": "simpleplc", "name": "SimplePLC", "version": version,
                "description": "Soft PLC: IEC 61131-3 Structured Text, Modbus TCP, OPC UA, web panel, REST API and MCP",
                "supplier": {"name": "SimplePLC", "url": ["https://github.com/tommy-berg/SimplePLC"]},
                "licenses": [{"license": {"id": "MIT"}}],
                "purl": f"pkg:github/tommy-berg/SimplePLC@v{version}",
                "externalReferences": [{"type": "vcs", "url": "https://github.com/tommy-berg/SimplePLC"}],
            },
            "properties": [{"name": "simpleplc:target", "value": target}],
        },
        "components": components,
        "dependencies": [{"ref": "simpleplc", "dependsOn": [c["bom-ref"] for c in components]}]
                        + [{"ref": c["bom-ref"], "dependsOn": []} for c in components],
    }
    text = json.dumps(bom, indent=2) + "\n"
    if args.output:
        pathlib.Path(args.output).write_text(text)
    else:
        sys.stdout.write(text)


def check(_args):
    problems = []
    tag = "v" + open62541_version()
    docker_tag = find(r"^ARG OPEN62541_TAG=(\S+)", "Dockerfile")
    if docker_tag != tag:
        problems.append(f"open62541: CMakeLists.txt fetches {tag}, the Dockerfile builds {docker_tag}")
    cache_key = find(r"key: open62541-(\S+)-src", ".github/workflows/ci.yml")
    if cache_key != tag:
        problems.append(f"open62541: CI caches the source of {cache_key}, CMakeLists.txt fetches {tag}")
    cmake_opts, docker_opts = cmake_open62541_options(), docker_open62541_options()
    for option in sorted(set(cmake_opts) | set(docker_opts)):
        if option.startswith("UA_ENABLE_") and cmake_opts.get(option) != docker_opts.get(option):
            problems.append(f"open62541: {option} is {cmake_opts.get(option)} in CMakeLists.txt "
                            f"but {docker_opts.get(option)} in the Dockerfile")

    alpine = find(r"^FROM alpine:(\S+) AS toolchain", "Dockerfile")
    for pinned in sorted(set(re.findall(r"alpine:[0-9][^\s'\"]*", read(".github/workflows/release.yml")))):
        problems.append(f"release.yml builds on {pinned}: it takes the Alpine release from the Dockerfile ({alpine})")

    covered = {c["where"] for c in COMPONENTS if c["where"]}
    for parent in ("third_party", "src/web/assets/vendor"):
        for d in sorted(p for p in (ROOT / parent).iterdir() if p.is_dir()):
            rel = d.relative_to(ROOT).as_posix()
            if rel not in covered:
                problems.append(f"{rel}: no component in security/sbom.py describes it")

    licenses = read("THIRD_PARTY_LICENSES.md")
    for c in COMPONENTS:
        v = c["version"]()
        row = re.search(rf"^\| \[{re.escape(c['name'])}\]\([^)]*\) \| ([^|]+)\|", licenses, re.MULTILINE)
        if not row:
            problems.append(f"THIRD_PARTY_LICENSES.md: no row for {c['name']}")
        elif not row.group(1).strip().startswith(v):
            problems.append(f"THIRD_PARTY_LICENSES.md: {c['name']} is {row.group(1).strip()}, the source has {v}")
    for name, *_ in TOOLCHAIN:
        if not re.search(rf"^\| \[{re.escape(name)}\]\(", licenses, re.MULTILINE):
            problems.append(f"THIRD_PARTY_LICENSES.md: no row for {name}, which the static Linux builds link")

    # A VEX statement is about one version of a component: when the component moves on, the
    # statement no longer matches it and has to be looked at again. The toolchain's versions are
    # known only in the build container, so statements about Alpine packages are not checked here.
    purls = {component(c)["purl"] for c in COMPONENTS}
    vex = json.loads(read("security/vex.json"))
    for s in vex.get("statements", []):
        name = s["vulnerability"]["name"]
        for product in s.get("products", []):
            purl = product.get("@id", "")
            if purl not in purls and not purl.startswith("pkg:apk/alpine/"):
                problems.append(f"security/vex.json: {name} is about {purl}, which is not in the SBOM")
        if s.get("status") == "not_affected" and not (s.get("justification") and s.get("impact_statement")):
            problems.append(f"security/vex.json: {name} is not_affected without a justification and impact_statement")

    for p in problems:
        print("FAIL", p)
    if problems:
        sys.exit(1)
    print(f"PASS SBOM sources agree ({len(COMPONENTS)} components, toolchain from Alpine {alpine}, "
          f"{len(vex.get('statements', []))} VEX statements)")


def fetch(url):
    headers = {"User-Agent": "SimplePLC-sbom", "Accept": "application/json"}
    if os.environ.get("GITHUB_TOKEN") and url.startswith("https://api.github.com/"):
        headers["Authorization"] = "Bearer " + os.environ["GITHUB_TOKEN"]
    with urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=30) as r:
        return r.read().decode()


def key(version):
    return tuple(int(x) for x in re.findall(r"[0-9]+", version))


def latest(kind, name):
    if kind == "github":
        return json.loads(fetch(f"https://api.github.com/repos/{name}/releases/latest"))["tag_name"].removeprefix("v")
    if kind == "npm5":  # CodeMirror 5 is maintained as its own line beside 6
        versions = json.loads(fetch(f"https://registry.npmjs.org/{name}"))["versions"]
        return max((v for v in versions if re.fullmatch(r"5\.[0-9]+\.[0-9]+", v)), key=key)
    if kind == "musl":
        return max(re.findall(r"musl-([0-9]+\.[0-9]+\.[0-9]+)\.tar\.gz", fetch("https://musl.libc.org/releases.html")), key=key)
    raise ValueError(kind)


def outdated(_args):
    rows, behind = [], 0
    for c in COMPONENTS:
        have = c["version"]()
        try:
            new = latest(*c["upstream"])
        except Exception as e:  # one unreachable site must not hide the others
            rows.append(f"| {c['name']} | {have} | ? | could not ask upstream: {e} |")
            continue
        if key(new) > key(have):
            behind += 1
            rows.append(f"| {c['name']} | {have} | **{new}** | update |")
        else:
            rows.append(f"| {c['name']} | {have} | {new} | current |")
    alpine = find(r"^FROM alpine:(\S+) AS toolchain", "Dockerfile")
    try:
        cycles = json.loads(fetch("https://endoflife.date/api/alpine-linux.json"))
        mine = next(c for c in cycles if c["cycle"] == alpine)
        newest = max(cycles, key=lambda c: key(c["cycle"]))["cycle"]
        ended = datetime.date.fromisoformat(mine["eol"]) <= datetime.date.today()
        if ended:
            behind += 1
        rows.append(f"| Alpine (musl, libstdc++, libgcc) | {alpine} | {newest} | "
                    f"{'**support ended ' + mine['eol'] + '**: no more security fixes' if ended else 'supported until ' + mine['eol']} |")
    except Exception as e:
        rows.append(f"| Alpine | {alpine} | ? | could not ask endoflife.date: {e} |")
    print("| Component | Built in | Newest | |\n|---|---|---|---|")
    print("\n".join(rows))
    sys.exit(2 if behind else 0)


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = p.add_subparsers(dest="command", required=True)
    g = sub.add_parser("generate")
    g.add_argument("--toolchain", action="store_true", help="add musl, libstdc++ and libgcc (run in the build container)")
    g.add_argument("--image", metavar="PLATFORMS", help="for the container image built for these platforms (implies --toolchain)")
    g.add_argument("--version", help="SimplePLC's version (default: CMake's project version)")
    g.add_argument("-o", "--output")
    g.set_defaults(func=generate)
    sub.add_parser("check").set_defaults(func=check)
    sub.add_parser("outdated").set_defaults(func=outdated)
    args = p.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
