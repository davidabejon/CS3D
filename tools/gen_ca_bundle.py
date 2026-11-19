"""Generate source/net/ca_bundle.c: the root certificates your app trusts.

    python tools/gen_ca_bundle.py            # regenerate from tools/hosts.txt
    python tools/gen_ca_bundle.py --verify   # check the bundle against them

The 3DS has no usable certificate store, so the app carries its own roots.
Which ones are needed is discovered by walking the real chains: for every host
in tools/hosts.txt, the topmost certificate the server sends names its issuer,
and that issuer is looked up as a subject in a root store (Mozilla's, as
shipped by the `certifi` package, or any PEM file passed with --store).

Hosts may name a port (host:8066); without one, 443. Some servers (often
self-hosted Icecast radios) send only their own certificate and leave out the
intermediate. A browser fetches it on the fly; mbedTLS cannot. For those, the
missing intermediate is downloaded from the certificate's AIA "CA Issuers"
URL, checked against the root store with openssl, and carried as an anchor
itself (mbedTLS accepts a trusted intermediate as the end of a chain).

On top of that, the roots in ALWAYS are carried unconditionally: CDNs answer
from whichever edge is closest, and edges do not all chain to the same root.

Every root costs parse time at start-up and ~1-2 KB of RAM, so keep the list
to what you need. Re-run this when a handshake fails with "unknown CA" or a
root is close to expiring.

Requires Python 3.10+ and the `openssl` command (Git Bash and devkitPro's
MSYS2 both ship it).
"""
import argparse
import re
import tempfile
import urllib.request
import shutil
import socket
import ssl
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
OUT = HERE.parent / "source" / "net" / "ca_bundle.c"
OUT_PEM = HERE.parent / "source" / "net" / "ca_bundle.pem"
HOSTS_FILE = HERE / "hosts.txt"

# Roots behind most of the public web. Remove the ones you do not need to
# speed up start-up; add others by their CN (or OU when they have no CN).
ALWAYS = [
    "ISRG Root X1",                          # Let's Encrypt (RSA)
    "ISRG Root X2",                          # Let's Encrypt (ECDSA)
    "DigiCert Global Root CA",
    "DigiCert Global Root G2",
    "DigiCert Global Root G3",
    "GlobalSign Root R46",
    "GTS Root R1",                           # Google Trust Services
    "GTS Root R4",
    "Amazon Root CA 1",
    "Starfield Root Certificate Authority - G2",
    "USERTrust RSA Certification Authority", # Sectigo
    "USERTrust ECC Certification Authority",
    "Certainly Root R1",
]


def openssl(args, stdin=None):
    return subprocess.run(["openssl", *args], input=stdin, capture_output=True,
                          text=True, check=True).stdout


def subject_of(pem):
    return openssl(["x509", "-noout", "-subject", "-nameopt", "RFC2253"],
                   pem).strip().removeprefix("subject=").strip()


def issuer_of(pem):
    return openssl(["x509", "-noout", "-issuer", "-nameopt", "RFC2253"],
                   pem).strip().removeprefix("issuer=").strip()


def default_store():
    try:
        import certifi
    except ImportError:
        raise SystemExit("No root store: `pip install certifi`, or pass "
                         "--store PATH to a PEM bundle (e.g. Mozilla's).")
    return Path(certifi.where())


def store_roots(store: Path):
    """Subject (RFC 2253) -> PEM, for every certificate in the store."""
    roots = {}
    blocks = re.findall(r"-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----",
                        store.read_text(encoding="utf-8"), re.S)
    for b in blocks:
        pem = b + "\n"
        try:
            roots.setdefault(subject_of(pem), pem)
        except subprocess.CalledProcessError:
            continue
    return roots


def read_hosts():
    hosts = []
    for line in HOSTS_FILE.read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            hosts.append(line)
    if not hosts:
        raise SystemExit(f"No hosts listed in {HOSTS_FILE}")
    return hosts


def split_host(entry):
    """'host' or 'host:port' -> (host, port)."""
    host, _, port = entry.partition(":")
    return host, int(port) if port else 443


def chain_of(entry):
    """The certificates the server sends, unverified: an incomplete chain
    must still be readable to find out what is missing."""
    host, port = split_host(entry)
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    with socket.create_connection((host, port), timeout=10) as s:
        with ctx.wrap_socket(s, server_hostname=host) as t:
            # Python 3.13 returns raw DER; older builds, certificate objects.
            return [ssl.DER_cert_to_PEM_cert(
                        c if isinstance(c, bytes) else c.public_bytes("DER"))
                    for c in t.get_unverified_chain()]


def ca_issuers_url(pem):
    """The AIA "CA Issuers" URL of a certificate, or None."""
    try:
        text = openssl(["x509", "-noout", "-ext", "authorityInfoAccess"], pem)
    except subprocess.CalledProcessError:
        return None
    m = re.search(r"CA Issuers - URI:(\S+)", text)
    return m.group(1) if m else None


def fetch_issuer(pem, store: Path):
    """Downloads the issuer a server left out of its chain and returns it as
    PEM, once openssl has verified it against the root store."""
    url = ca_issuers_url(pem)
    if not url:
        return None, "no AIA CA Issuers URL"
    with urllib.request.urlopen(url, timeout=20) as r:
        data = r.read()
    # Usually DER (.crt / .cer), sometimes PEM. Through a file: Windows
    # builds of openssl read stdin in text mode and mangle DER.
    form = "PEM" if b"-----BEGIN" in data else "DER"
    with tempfile.TemporaryDirectory() as tmp:
        raw = Path(tmp) / "issuer.crt"
        raw.write_bytes(data)
        issuer = openssl(["x509", "-inform", form, "-outform", "PEM",
                          "-in", str(raw)])
        if subject_of(issuer) != issuer_of(pem):
            return None, f"{url} is not the issuer"
        converted = Path(tmp) / "issuer.pem"
        converted.write_text(issuer, encoding="ascii")
        ok = subprocess.run(["openssl", "verify", "-CAfile", str(store),
                             str(converted)], capture_output=True, text=True)
    if ok.returncode != 0:
        return None, f"{url} does not chain to the store: {ok.stdout.strip()}"
    return issuer, url


def common_name(subject):
    m = re.search(r"(?:^|,)CN=([^,]+)", subject) or \
        re.search(r"(?:^|,)OU=([^,]+)", subject)
    return m.group(1).strip() if m else subject


def verify(hosts):
    """Checks the generated bundle validates every host, over TLS 1.2 (what
    the 3DS client speaks)."""
    bad = 0
    for entry in hosts:
        host, port = split_host(entry)
        ctx = ssl.create_default_context(cafile=str(OUT_PEM))
        ctx.minimum_version = ctx.maximum_version = ssl.TLSVersion.TLSv1_2
        # Anchors may be intermediates (see the module docstring).
        ctx.verify_flags |= ssl.VERIFY_X509_PARTIAL_CHAIN
        try:
            with socket.create_connection((host, port), timeout=10) as s:
                with ctx.wrap_socket(s, server_hostname=host) as t:
                    print(f"  ok   {entry:40} {t.version()} {t.cipher()[0]}")
        except Exception as e:
            print(f"  FAIL {entry:40} {type(e).__name__}: {e}")
            bad += 1
    return bad


def generate(hosts, store: Path):
    roots = store_roots(store)
    wanted, failed = {}, []

    for host in hosts:
        try:
            chain = chain_of(host)
        except Exception as e:
            failed.append(f"{host}: {type(e).__name__}: {e}")
            continue
        # The server stops one short of the root (or sends it, in which case
        # it is its own issuer): the last certificate names the anchor. A
        # cross-signed chain may end at a retired root instead; then a
        # certificate in the chain whose subject is itself a current root
        # is the anchor (mbedTLS stops there too).
        name = issuer_of(chain[-1])
        pem = roots.get(name)
        label = None
        for cert in reversed(chain):
            if pem is not None:
                break
            name = subject_of(cert)
            pem = roots.get(name)
        if pem is None and issuer_of(chain[-1]) != subject_of(chain[-1]):
            # The server did not send its intermediate: carry it ourselves.
            try:
                pem, why = fetch_issuer(chain[-1], store)
            except Exception as e:
                pem, why = None, f"{type(e).__name__}: {e}"
            if pem is not None:
                name = subject_of(pem)
                label = common_name(name) + " (intermediate)"
                print(f"  {host}: the server omits its intermediate; "
                      f"carrying it, from {why}")
            else:
                failed.append(f"{host}: incomplete chain, and its issuer "
                              f"could not be fetched: {why}")
                continue
        if pem is None:
            failed.append(f"{host}: no root for {issuer_of(chain[-1])!r} "
                          f"in {store}")
            continue
        wanted.setdefault(name, (label or common_name(name), pem, []))
        wanted[name][2].append(host)

    for cn in ALWAYS:
        hit = [(n, pem) for n, pem in roots.items() if common_name(n) == cn]
        if not hit:
            failed.append(f"ALWAYS root {cn!r} is not in {store}")
            continue
        name, pem = hit[0]
        wanted.setdefault(name, (cn, pem, ["always"]))

    if not wanted:
        raise SystemExit("Found no roots at all:\n  " + "\n  ".join(failed))

    ordered = sorted(wanted.values(), key=lambda v: v[0].lower())
    OUT_PEM.write_text("".join(
        f"# {cn} -- {', '.join(hosts)}\n{pem.strip()}\n"
        for cn, pem, hosts in ordered), encoding="utf-8", newline="\n")

    body = []
    for cn, pem, hosts in ordered:
        body.append(f"\t/* {cn} -- {', '.join(hosts)} */\n")
        for line in pem.strip().splitlines():
            body.append(f'\t"{line}\\n"\n')
        body.append("\n")
    OUT.write_text(
        "/* Generated by tools/gen_ca_bundle.py -- do not edit.\n"
        " *\n"
        " * The trust anchors the app accepts, as PEM. mbedTLS parses this once\n"
        " * at start-up (tls_global_init); there is no certificate store on the\n"
        " * console.\n"
        " */\n"
        '#include "ca_bundle.h"\n\n'
        "const char ca_bundle[] =\n" + "".join(body).rstrip() + ";\n\n"
        "const size_t ca_bundle_len = sizeof(ca_bundle);\n",
        encoding="utf-8", newline="\n")

    print(f"Wrote {OUT.name} and {OUT_PEM.name} with {len(wanted)} root(s):")
    for cn, _, hosts in ordered:
        print(f"  {cn:42} {', '.join(hosts)}")
    for f in failed:
        print(f"  ! {f}", file=sys.stderr)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--verify", action="store_true",
                    help="check the current bundle against tools/hosts.txt")
    ap.add_argument("--store", type=Path,
                    help="PEM root store to pick from (default: certifi)")
    args = ap.parse_args()
    hosts = read_hosts()
    if args.verify:
        raise SystemExit(1 if verify(hosts) else 0)
    if not shutil.which("openssl"):
        raise SystemExit("The `openssl` command is needed; run this from Git "
                         "Bash or devkitPro's MSYS2 shell.")
    generate(hosts, args.store or default_store())


if __name__ == "__main__":
    main()
