# Security

## Reporting a vulnerability

Report it privately: on GitHub, open the repository's **Security** tab and choose **Report a
vulnerability**. Please do not open a public issue. Say which version you run
(`simpleplc --version`), what you did and what happened.

Fixes come as a new release; the latest release is the one supported.

Some things are known and by design: Modbus TCP has no authentication, OPC UA runs with security
policy None, and the panel and API are served over plain HTTP. The README's Security section says
how to deploy with that in mind.

## What is in a release

Each binary and the container image comes with an SBOM in CycloneDX format
(`simpleplc-linux-<arch>.cdx.json` on the release page; the image's is attached to it on Docker
Hub). Both are signed by the release workflow, as is the provenance of each build:

```
gh attestation verify simpleplc-linux-arm64 --repo tommy-berg/SimplePLC
gh attestation verify simpleplc-linux-arm64 --repo tommy-berg/SimplePLC --predicate-type https://cyclonedx.org/bom
gh attestation verify oci://tommyhbe/simpleplc:<version> --repo tommy-berg/SimplePLC
```

The SBOM is made from the sources and the build container, not kept by hand
([`security/sbom.py`](security/sbom.py)). Each week the
[Security workflow](.github/workflows/security.yml) checks it against the vulnerability databases
and against the newest upstream releases, and opens an issue when something needs attention.
[`security/vex.json`](security/vex.json) lists the reported vulnerabilities that do not affect
SimplePLC, each with the reason.
